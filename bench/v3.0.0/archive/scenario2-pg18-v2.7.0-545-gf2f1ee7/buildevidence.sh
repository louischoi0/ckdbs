source /home/cdkbs/bench-runs/pg18-f2f1ee7/lib.sh
OUT=$RUN/pg-build-evidence.txt
{
echo "## pg_config --configure"; /home/cdkbs/.local/pgsql/bin/pg_config --configure
echo "## postgres --version"; postgres --version
echo "## ldd postgres | grep -i -E 'libz|icu|readline|ssl|crypto'"; ldd $(command -v postgres) | grep -i -E "libz|icu|readline|ssl|crypto"
echo "## ldd psql | grep -i -E 'libz|icu|readline' (empty = none linked)"; ldd $(command -v psql) | grep -i -E "libz|icu|readline"
echo "## df -T of the data directory's filesystem"; df -T $PGROOT | cat
} > $OUT 2>&1
fresh_pg evidence
{
echo "## pg_database provider/collation (libc/C: ICU is not in play)"
$PSQL -d postgres -Atc "select datname, datlocprovider, datcollate, datctype, pg_encoding_to_char(encoding) from pg_database order by 1"
echo "## compression settings on the hot paths"
$PSQL -d postgres -Atc "show default_toast_compression; show wal_compression; show io_method; show wal_sync_method; show fsync; show synchronous_commit; show full_page_writes; show autovacuum; show max_wal_size; show shared_buffers"
echo "## SELECT version()"
$PSQL -d postgres -Atc "select version()"
echo "## pg_settings non-default, verbatim (the same dump is in each cell's logs/<cell>.pg_settings)"
$PSQL -d postgres -Atc "SELECT name, setting, source FROM pg_settings WHERE source <> 'default' ORDER BY name"
} >> $OUT 2>&1
stop_pg evidence
cat $OUT
