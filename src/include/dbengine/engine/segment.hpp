//===----------------------------------------------------------------------===//
//
// segment.hpp
//   Un *segmento* es los datos de UNA columna para UN stripe.
//
// Se serializa de forma autocontenida para que el lector pueda traer del
// storage solo los segmentos de las columnas que la query pide (column
// pruning) sin leer ni interpretar el resto.
//
// El motor no sabe nada de tipos de Postgres: solo maneja "valores de ancho
// fijo de W bytes" o "valores de largo variable".  La conversión Datum <->
// bytes la hace la capa C.  Por eso la compresión que se elige acá es la
// que se puede decidir mirando los bytes, no la semántica del tipo.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <vector>

#include "dbengine/engine/engine.hpp"

namespace dbe {

/// Codificación del cuerpo del segmento (el "byte de esquema" que pide F2).
enum SegmentEncoding : uint8_t
{
	kEncNone = 0,				///< valores crudos
	kEncRle = 1,				///< ancho fijo: (largo de corrida, valor)*
	kEncFor = 2,				///< ancho fijo: mínimo + bit-packing del resto
	kEncDict = 3,				///< largo variable: diccionario + códigos
	kEncDictFsst = 4			///< igual, con el diccionario comprimido con FSST
};

/// Layout serializado (little-endian nativo; el storage no es portable
/// entre arquitecturas, igual que el heap de Postgres):
///
///   SegmentHeader
///   null_bitmap   (null_bitmap_bytes)   bit prendido = NO nulo
///   offsets       (offsets_bytes)
///   data          (data_bytes)
///
/// Qué hay en `offsets` y en `data` depende de `encoding`:
///
///   kEncNone, ancho fijo   offsets vacío; data = row_count valores
///   kEncNone, variable     offsets = row_count+1 uint32; data = bytes
///   kEncRle                offsets vacío; data = aux_count corridas
///   kEncFor                offsets vacío; data = mínimo + valores empacados
///   kEncDict               offsets = aux_count+1 uint32 del diccionario;
///                          data = bytes del diccionario + códigos empacados
///   kEncDictFsst           offsets = aux_count+1 uint32 del diccionario YA
///                          COMPRIMIDO; data = [u32 largo de la tabla de
///                          símbolos][tabla][diccionario comprimido][códigos]
///
/// FSST comprime el CONTENIDO del diccionario, no valor por valor: se
/// descomprime entero una vez al abrir el segmento y de ahí en más el acceso
/// es idéntico a kEncDict.  Medido sobre ClickBench el diccionario baja 1,6×
/// a 2,0× y descomprimirlo cuesta 0,53 s para un scan completo de las tres
/// columnas de texto, contra 7,8 s de I/O que ahorra en frío.
///
/// Los nulos ocupan lugar igual: un slot en cero si es de ancho fijo, un
/// rango vacío si es variable.  Cuesta espacio y compra acceso aleatorio
/// O(1) por fila sin tener que contar bits.
struct SegmentHeader
{
	uint32_t	magic;
	uint16_t	version;
	uint16_t	flags;
	uint32_t	row_count;
	uint32_t	null_bitmap_bytes;
	uint32_t	fixed_width;	///< 0 = largo variable
	uint32_t	offsets_bytes;
	uint32_t	data_bytes;
	uint8_t		encoding;
	uint8_t		bit_width;		///< bits por valor en kEncFor / kEncDict
	uint16_t	reserved;
	uint32_t	aux_count;		///< corridas (RLE) o entradas del diccionario
	uint32_t	reserved2;		///< deja el header en 40 bytes, múltiplo de 8
};

constexpr uint32_t kSegmentMagic = 0x44424553;	// "DBES"
constexpr uint16_t kSegmentVersion = 2;
constexpr uint16_t kSegFlagHasNulls = 1u << 0;

static_assert(sizeof(SegmentHeader) == 40, "el header del segmento cambió de tamaño");

/// Cuánto ocupó cada codificación al evaluarla; se expone para poder
/// explicar por qué se eligió una y no otra.
struct EncodingChoice
{
	uint8_t		encoding = kEncNone;
	uint32_t	raw_bytes = 0;
	uint32_t	encoded_bytes = 0;
};

/// Acumula los valores de una columna y los serializa.
class SegmentBuilder
{
public:
	/// fixed_width == 0 significa largo variable.  Con allow_compression en
	/// false siempre sale kEncNone: sirve para medir F2 contra F1 con
	/// exactamente el mismo camino de código.
	SegmentBuilder(uint32_t fixed_width, bool allow_compression);

	/// `data` puede ser nullptr si is_null.  Para ancho fijo, len se ignora.
	void		append(const void *data, uint32_t len, bool is_null);

	/// Serializa, eligiendo la codificación más chica.  El buffer devuelto
	/// vive hasta el próximo finish() o hasta que se destruya el builder.
	const std::vector<uint8_t> &finish();

	uint32_t	row_count() const noexcept { return row_count_; }
	const EncodingChoice &choice() const noexcept { return choice_; }

private:
	/// Evalúa las codificaciones posibles y arma el cuerpo con la mejor.
	void		encode_body(SegmentHeader *hdr, std::vector<uint8_t> *offsets,
							std::vector<uint8_t> *data);
	bool		try_rle(std::vector<uint8_t> *out, uint32_t *runs) const;
	bool		try_for(std::vector<uint8_t> *out, uint32_t *bits) const;
	/// Comprime con FSST el diccionario ya armado.  Devuelve false si no
	/// paga: la tabla de símbolos son ~560 bytes, así que en diccionarios
	/// chicos no compensa.
	static bool	fsst_pack(const std::vector<const std::string *> &order,
						  std::vector<uint8_t> *offsets,
						  std::vector<uint8_t> *dict_bytes);

	/// `encoding_out` sale en kEncDict o kEncDictFsst según haya pagado
	/// comprimir el diccionario.
	bool		try_dict(std::vector<uint8_t> *offsets, std::vector<uint8_t> *data,
						 uint32_t *entries, uint32_t *bits,
						 uint8_t *encoding_out) const;

	uint32_t	fixed_width_;
	bool		allow_compression_;
	uint32_t	row_count_ = 0;
	bool		has_nulls_ = false;
	std::vector<uint8_t> data_;
	std::vector<uint8_t> nulls_;	///< bit prendido = NO nulo
	std::vector<uint32_t> offsets_;	///< row_count+1 entradas si es variable
	std::vector<uint8_t> out_;
	EncodingChoice choice_;
};

/// Lee un segmento serializado.  Para kEncNone y kEncDict apunta adentro
/// del buffer que le pasan, que tiene que sobrevivir al reader.  Para las
/// codificaciones que no dan acceso aleatorio directo (RLE, FOR) expande el
/// cuerpo una vez al abrirlo.
class SegmentReader
{
public:
	SegmentReader(const uint8_t *bytes, uint32_t len);

	uint32_t	row_count() const noexcept { return header_.row_count; }
	uint8_t		encoding() const noexcept { return header_.encoding; }

	/// Devuelve el valor de la fila `row`.  Si es nulo, *data queda en
	/// nullptr y *len en 0.
	void		get(uint32_t row, const uint8_t **data, uint32_t *len,
					bool *is_null) const;

	/// Vista del segmento como arreglo contiguo de ancho fijo, para poder
	/// recorrerlo sin una llamada por valor.  Devuelve false cuando no
	/// existe tal arreglo — largo variable o diccionario — y ahí hay que
	/// usar get() fila por fila.
	bool		vector(const uint8_t **data, uint32_t *width,
					   const uint8_t **nulls) const;

	/// ¿El segmento usa diccionario?  Si sí, *size queda con la cantidad de
	/// entradas.  Sirve para agrupar por el código en vez de por el valor:
	/// se resuelve el string una vez por entrada del diccionario y no una
	/// vez por fila.
	bool		dict(uint32_t *size) const noexcept;

	/// Código de diccionario de una fila.  Solo tiene sentido si dict()
	/// devolvió true.
	void		code(uint32_t row, uint32_t *code_out, bool *is_null) const;

	/// Valor de una entrada del diccionario.
	void		dict_value(uint32_t idx, const uint8_t **data,
						   uint32_t *len) const;

private:
	void		materialize_rle(const uint8_t *data, uint32_t len);
	void		materialize_for(const uint8_t *data, uint32_t len);

	SegmentHeader header_;
	const uint8_t *nulls_ = nullptr;
	const uint32_t *offsets_ = nullptr;
	const uint8_t *data_ = nullptr;
	std::vector<uint8_t> expanded_;	///< usado por RLE, FOR y el diccionario FSST
	std::vector<uint32_t> dict_offsets_;	///< offsets ya descomprimidos
	const uint8_t *codes_ = nullptr;	///< códigos empacados (kEncDict*)

	void		materialize_fsst_dict(const uint8_t *data, uint32_t len);
};

}	// namespace dbe
