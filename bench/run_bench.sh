#!/bin/sh
# Запуск замеров: поднимает процесс-экспортёр, прогоняет клиента и
# базовые линии IPC, затем гасит экспортёра.
#
# Использование: run_bench.sh [каталог_сборки] [итераций]

set -e

BUILD_DIR="${1:-build}"
ITERS="${2:-4000}"
BENCH_DIR="$BUILD_DIR/bench"

if [ ! -x "$BENCH_DIR/bench_srpc_client" ]; then
    echo "Не найдено $BENCH_DIR/bench_srpc_client — сначала выполните сборку" >&2
    exit 1
fi

echo "=== окружение ==="
uname -srm
grep -m1 'model name' /proc/cpuinfo || true
echo "CPU: $(nproc)"
echo

echo "=== базовые линии IPC (без libsrpc) ==="
"$BENCH_DIR/bench_ipc_baseline" "$ITERS"
echo

echo "=== sRPC ==="
"$BENCH_DIR/bench_srpc_server" 120 &
SERVER_PID=$!
sleep 2

"$BENCH_DIR/bench_srpc_client" "$ITERS" 4 500 || true

kill -TERM "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
