#!/usr/bin/env bash
# Cluster PostgreSQL 18 de desarrollo, corriendo como el usuario actual.
#
# No usa pg_createcluster a propósito: el cluster pertenece a quien
# desarrolla, así `make install` + `make installcheck` no necesitan pelearse
# con permisos del usuario postgres.
#
#   tools/devcluster.sh init|start|stop|status|psql|destroy
set -euo pipefail

PG_MAJOR="${PG_MAJOR:-18}"
PGBIN="${PGBIN:-/usr/lib/postgresql/${PG_MAJOR}/bin}"
PGDATA="${PGDATA:-${HOME}/.dbengine/pg${PG_MAJOR}/data}"
PGPORT="${PGPORT:-54318}"
LOGFILE="${PGDATA%/data}/server.log"
SOCKDIR="${PGSOCKDIR:-${PGDATA%/data}/run}"

usage() { sed -n '2,10p' "$0"; exit 1; }

case "${1:-}" in
init)
	if [ -d "$PGDATA" ]; then
		echo "ya existe: $PGDATA"; exit 0
	fi
	mkdir -p "$PGDATA" "$SOCKDIR"
	"$PGBIN/initdb" -D "$PGDATA" -U "$(id -un)" --encoding=UTF8 --locale=C
	# Cargar la extensión en todos los backends: hace falta para probar los
	# hooks del planner y el background worker (F3 en adelante).
	cat >> "$PGDATA/postgresql.conf" <<-CONF

		# --- dbengine (desarrollo) ---
		port = ${PGPORT}
		listen_addresses = 'localhost'
		# El build de Debian apunta a /var/run/postgresql, que no es
		# escribible por un usuario común.
		unix_socket_directories = '${SOCKDIR}'
		shared_preload_libraries = 'dbengine'
		# El time travel lee pg_commit_ts.  Sin esto, la suite falla en
		# timetravel, merge y grouped.
		track_commit_timestamp = on
		log_min_messages = warning
	CONF
	echo "cluster creado en $PGDATA (puerto $PGPORT)"
	;;
start)
	mkdir -p "$SOCKDIR"
	"$PGBIN/pg_ctl" -D "$PGDATA" -l "$LOGFILE" -w start
	;;
stop)
	"$PGBIN/pg_ctl" -D "$PGDATA" -w -m fast stop
	;;
status)
	"$PGBIN/pg_ctl" -D "$PGDATA" status
	;;
psql)
	shift
	"$PGBIN/psql" -h "$SOCKDIR" -p "$PGPORT" -d postgres "$@"
	;;
destroy)
	"$PGBIN/pg_ctl" -D "$PGDATA" -m immediate stop 2>/dev/null || true
	rm -rf "${PGDATA%/data}"
	echo "destruido"
	;;
*)
	usage
	;;
esac
