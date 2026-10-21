/*-------------------------------------------------------------------------
 *
 * storage.c
 *	  Archivo lógico direccionable por bytes sobre el fork `main`.
 *	  Ver storage.h y doc/adr-001-storage-fisico.md.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"

#include "dbengine/dbengine.h"
#include "dbengine/storage.h"

/* Offset lógico -> bloque físico y offset adentro de la página. */
#define DBE_BLOCK_OF(off)	((BlockNumber) (DBE_FIRST_DATA_BLOCK + (off) / DBE_PAGE_USABLE))
#define DBE_INPAGE_OF(off)	((uint32) (SizeOfPageHeaderData + (off) % DBE_PAGE_USABLE))

static DbeStorageMeta *
dbe_meta_ptr(Page page)
{
	return (DbeStorageMeta *) (((char *) page) + SizeOfPageHeaderData);
}

/*
 * Marca cuánto de la página está ocupado.
 *
 * Importa para el WAL: log_newpage_buffer(buf, true) omite el "agujero"
 * entre pd_lower y pd_upper.  Si dejáramos pd_lower en el header, el
 * agujero sería toda nuestra data y el WAL no la guardaría.  Poniendo
 * pd_lower al final de lo escrito, el agujero es solo la cola sin usar,
 * que es exactamente lo que se puede omitir.
 */
static void
dbe_page_set_used(Page page, uint32 used_bytes)
{
	PageHeader	ph = (PageHeader) page;
	uint32		lower = SizeOfPageHeaderData + used_bytes;

	Assert(lower <= BLCKSZ);
	if (lower > ph->pd_lower)
		ph->pd_lower = (LocationIndex) lower;
}

/*
 * Devuelve el buffer del bloque pedido con lock exclusivo, extendiendo la
 * relación si hace falta.  Las páginas nuevas se inicializan.
 */
static Buffer
dbe_get_block_for_write(Relation rel, BlockNumber blkno)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	Buffer		buf;

	if (blkno < nblocks)
	{
		buf = ReadBuffer(rel, blkno);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	}
	else
	{
		/* ExtendBufferedRelTo toma el lock de extensión por nosotros. */
		buf = ExtendBufferedRelTo(BMR_REL(rel), MAIN_FORKNUM, NULL,
								  EB_LOCK_TARGET | EB_CREATE_FORK_IF_NEEDED,
								  blkno + 1, RBM_NORMAL);
	}

	if (PageIsNew(BufferGetPage(buf)))
		PageInit(BufferGetPage(buf), BLCKSZ, 0);

	return buf;
}

void
dbe_storage_init(Relation rel)
{
	Buffer		buf;
	Page		page;
	DbeStorageMeta *meta;

	if (RelationGetNumberOfBlocks(rel) > 0)
		return;

	buf = dbe_get_block_for_write(rel, DBE_META_BLOCK);
	page = BufferGetPage(buf);

	/* Otro backend pudo haberlo inicializado mientras esperábamos el lock. */
	meta = dbe_meta_ptr(page);
	if (meta->magic == DBE_STORAGE_MAGIC)
	{
		UnlockReleaseBuffer(buf);
		return;
	}

	START_CRIT_SECTION();

	meta->magic = DBE_STORAGE_MAGIC;
	meta->version = DBE_STORAGE_VERSION;
	meta->next_offset = 0;
	meta->next_stripe_id = 1;
	meta->next_row_number = 0;

	dbe_page_set_used(page, sizeof(DbeStorageMeta));
	MarkBufferDirty(buf);

	if (RelationNeedsWAL(rel))
		PageSetLSN(page, log_newpage_buffer(buf, true));

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);
}

bool
dbe_storage_read_meta(Relation rel, DbeStorageMeta *out)
{
	Buffer		buf;
	DbeStorageMeta *meta;

	if (RelationGetNumberOfBlocks(rel) == 0)
		return false;

	buf = ReadBuffer(rel, DBE_META_BLOCK);
	LockBuffer(buf, BUFFER_LOCK_SHARE);

	meta = dbe_meta_ptr(BufferGetPage(buf));
	if (meta->magic != DBE_STORAGE_MAGIC)
	{
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("dbengine: la relación \"%s\" no tiene una página de "
						"metadatos válida",
						RelationGetRelationName(rel))));
	}
	if (meta->version != DBE_STORAGE_VERSION)
	{
		uint32		got = meta->version;

		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dbengine: versión de storage %u no soportada (se esperaba %d)",
						got, DBE_STORAGE_VERSION)));
	}

	*out = *meta;
	UnlockReleaseBuffer(buf);
	return true;
}

/*
 * Toma el bloque de metadatos con lock exclusivo.  Ese lock es lo que
 * serializa toda reserva, de espacio y de números de fila.
 */
static Buffer
dbe_meta_lock(Relation rel)
{
	Buffer		buf;

	dbe_storage_init(rel);

	buf = ReadBuffer(rel, DBE_META_BLOCK);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	if (dbe_meta_ptr(BufferGetPage(buf))->magic != DBE_STORAGE_MAGIC)
	{
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("dbengine: página de metadatos inválida en \"%s\"",
						RelationGetRelationName(rel))));
	}

	return buf;
}

static void
dbe_meta_release(Relation rel, Buffer buf)
{
	Page		page = BufferGetPage(buf);

	MarkBufferDirty(buf);
	if (RelationNeedsWAL(rel))
		PageSetLSN(page, log_newpage_buffer(buf, true));

	UnlockReleaseBuffer(buf);
}

uint64
dbe_storage_reserve_bytes(Relation rel, uint64 nbytes, uint64 *stripe_id_out)
{
	Buffer		buf = dbe_meta_lock(rel);
	DbeStorageMeta *meta = dbe_meta_ptr(BufferGetPage(buf));
	uint64		start;

	/* Los stripes arrancan en borde de página: ver el comentario en storage.h. */
	start = meta->next_offset;
	if (start % DBE_PAGE_USABLE != 0)
		start += DBE_PAGE_USABLE - (start % DBE_PAGE_USABLE);

	START_CRIT_SECTION();
	meta->next_offset = start + nbytes;
	/*
	 * El id de stripe solo le sirve al storage de tablas.  El índice PGM
	 * reserva bytes en el mismo archivo lógico y no lo quiere.
	 */
	if (stripe_id_out != NULL)
		*stripe_id_out = meta->next_stripe_id;
	meta->next_stripe_id++;
	dbe_meta_release(rel, buf);
	END_CRIT_SECTION();

	return start;
}

uint64
dbe_storage_reserve_rows(Relation rel, uint64 nrows)
{
	Buffer		buf = dbe_meta_lock(rel);
	DbeStorageMeta *meta = dbe_meta_ptr(BufferGetPage(buf));
	uint64		first;

	START_CRIT_SECTION();
	first = meta->next_row_number;
	meta->next_row_number += nrows;
	dbe_meta_release(rel, buf);
	END_CRIT_SECTION();

	return first;
}

void
dbe_storage_release_rows(Relation rel, uint64 base, uint64 reserved,
						 uint64 used)
{
	Buffer		buf;
	DbeStorageMeta *meta;

	if (used >= reserved)
		return;

	buf = dbe_meta_lock(rel);
	meta = dbe_meta_ptr(BufferGetPage(buf));

	/* Solo si somos los últimos: si otro reservó después, no se puede. */
	if (meta->next_row_number != base + reserved)
	{
		UnlockReleaseBuffer(buf);
		return;
	}

	START_CRIT_SECTION();
	meta->next_row_number = base + used;
	dbe_meta_release(rel, buf);
	END_CRIT_SECTION();
}

void
dbe_storage_write(Relation rel, uint64 offset, const char *data, uint64 len)
{
	while (len > 0)
	{
		BlockNumber blkno = DBE_BLOCK_OF(offset);
		uint32		inpage = DBE_INPAGE_OF(offset);
		uint64		chunk = Min(len, (uint64) (BLCKSZ - inpage));
		Buffer		buf;
		Page		page;

		buf = dbe_get_block_for_write(rel, blkno);
		page = BufferGetPage(buf);

		START_CRIT_SECTION();

		memcpy(((char *) page) + inpage, data, chunk);
		dbe_page_set_used(page, (uint32) (inpage - SizeOfPageHeaderData + chunk));
		MarkBufferDirty(buf);

		if (RelationNeedsWAL(rel))
			PageSetLSN(page, log_newpage_buffer(buf, true));

		END_CRIT_SECTION();

		UnlockReleaseBuffer(buf);

		offset += chunk;
		data += chunk;
		len -= chunk;
	}
}

void
dbe_storage_read(Relation rel, uint64 offset, char *dest, uint64 len)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

	while (len > 0)
	{
		BlockNumber blkno = DBE_BLOCK_OF(offset);
		uint32		inpage = DBE_INPAGE_OF(offset);
		uint64		chunk = Min(len, (uint64) (BLCKSZ - inpage));
		Buffer		buf;

		if (blkno >= nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("dbengine: lectura fuera del storage de \"%s\" "
							"(bloque %u de %u)",
							RelationGetRelationName(rel), blkno, nblocks)));

		buf = ReadBuffer(rel, blkno);
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		memcpy(dest, ((char *) BufferGetPage(buf)) + inpage, chunk);

		UnlockReleaseBuffer(buf);

		offset += chunk;
		dest += chunk;
		len -= chunk;
	}
}
