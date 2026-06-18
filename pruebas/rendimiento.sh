#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
PORT=9103
WORKDIR=/tmp/aws-s3-rendimiento
DATADIR="$WORKDIR/data"
LOCALDIR="$WORKDIR/local"
DOWNLOADDIR="$WORKDIR/download"
SERVER_LOG="$WORKDIR/server.log"

cleanup() {
    if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}

trap cleanup EXIT INT TERM

rm -rf "$WORKDIR"
mkdir -p "$DATADIR" "$LOCALDIR/pequenos" "$DOWNLOADDIR"

"$ROOT_DIR/bin/aws-s3_server" --host 127.0.0.1 --port "$PORT" --data "$DATADIR" \
    >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 1

run_client() {
    AWS_S3_HOST=127.0.0.1 AWS_S3_PORT=$PORT "$ROOT_DIR/bin/aws-s3" "$@"
}

measure_ms() {
    START_NS=$(date +%s%N)
    "$@"
    END_NS=$(date +%s%N)
    echo $(((END_NS - START_NS) / 1000000))
}

run_client mb s3://rendimiento

dd if=/dev/zero of="$LOCALDIR/grande.bin" bs=1024 count=16384 status=none

i=1
while [ "$i" -le 200 ]; do
    dd if=/dev/zero of="$LOCALDIR/pequenos/f$i.bin" bs=1024 count=4 status=none
    i=$((i + 1))
done

GRANDE_SUBIDA_MS=$(measure_ms run_client cp "$LOCALDIR/grande.bin" s3://rendimiento/grande.bin)
GRANDE_DESCARGA_MS=$(measure_ms run_client cp s3://rendimiento/grande.bin "$DOWNLOADDIR/grande.bin")
PEQUENOS_SUBIDA_MS=$(measure_ms run_client cp "$LOCALDIR/pequenos" s3://rendimiento/pequenos/ --recursive)
PEQUENOS_DESCARGA_MS=$(measure_ms run_client cp s3://rendimiento/pequenos "$DOWNLOADDIR/pequenos" --recursive)

cmp "$LOCALDIR/grande.bin" "$DOWNLOADDIR/grande.bin"
cmp "$LOCALDIR/pequenos/f1.bin" "$DOWNLOADDIR/pequenos/f1.bin"

BUCKET_SIZE=$(wc -c < "$DATADIR/rendimiento.s3b")

echo "subida_grande_ms=$GRANDE_SUBIDA_MS"
echo "descarga_grande_ms=$GRANDE_DESCARGA_MS"
echo "subida_pequenos_ms=$PEQUENOS_SUBIDA_MS"
echo "descarga_pequenos_ms=$PEQUENOS_DESCARGA_MS"
echo "tamano_bucket_bytes=$BUCKET_SIZE"
