#!/usr/bin/env bash
# Mixed TCP/UDP RTSP clients for DURATION seconds (default 600) while sampling
# server RSS, fd count, thread count and CPU. Results go to a CSV plus a
# summary file that also carries the server's own final counters and the
# per-client exit codes. Requires ffmpeg; if it is missing the run is reported
# as "not executed" instead of producing fabricated numbers.

# Re-exec under bash so `sh scripts/stress.sh` behaves like `bash`/direct exec.
if [ -z "${BASH_VERSION:-}" ]; then
  exec /bin/bash "$0" "$@"
fi

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-${ROOT}/build}"
DURATION="${DURATION:-600}"
INTERVAL="${INTERVAL:-2}"
CLIENTS_PER_TRANSPORT="${CLIENTS_PER_TRANSPORT:-8}"
CODEC="${CODEC:-h264}"
KBPS="${KBPS:-0}"
INPUT_FILE="${INPUT_FILE:-}"
DECODE="${DECODE:-0}"
TRANSPORTS="${TRANSPORTS:-tcp udp}"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --codec)
      if [ "$#" -lt 2 ]; then
        printf 'stress: --codec needs h264|h265\n' >&2
        exit 2
      fi
      CODEC="$2"
      shift 2
      ;;
    --codec=*)
      CODEC="${1#--codec=}"
      shift
      ;;
    --duration)
      DURATION="${2:?--duration needs seconds}"
      shift 2
      ;;
    --duration=*)
      DURATION="${1#--duration=}"
      shift
      ;;
    --clients-per-transport)
      CLIENTS_PER_TRANSPORT="${2:?--clients-per-transport needs a count}"
      shift 2
      ;;
    --clients-per-transport=*)
      CLIENTS_PER_TRANSPORT="${1#--clients-per-transport=}"
      shift
      ;;
    --kbps)
      KBPS="${2:?--kbps needs a rate}"
      shift 2
      ;;
    --kbps=*)
      KBPS="${1#--kbps=}"
      shift
      ;;
    --file)
      INPUT_FILE="${2:?--file needs a path}"
      shift 2
      ;;
    --file=*)
      INPUT_FILE="${1#--file=}"
      shift
      ;;
    --decode)
      DECODE=1
      shift
      ;;
    --transports)
      TRANSPORTS="${2:?--transports needs a list}"
      shift 2
      ;;
    --transports=*)
      TRANSPORTS="${1#--transports=}"
      shift
      ;;
    *)
      printf 'usage: %s [--codec h264|h265] [--duration S] [--clients-per-transport N]\n' "$0" >&2
      printf '          [--kbps N | --file PATH] [--transports "tcp udp"] [--decode]\n' "$0" >&2
      printf '  default clients send -c copy; --decode adds real decoder load\n' >&2
      exit 2
      ;;
  esac
done

case "${CODEC}" in
  h264 | h265) ;;
  *)
    printf 'stress: unknown codec %s (expected h264|h265)\n' "${CODEC}" >&2
    exit 2
    ;;
esac

STAMP="$(date +%Y%m%d_%H%M%S)"
OUT="${BUILD}/e2e/stress_${STAMP}"
CSV="${OUT}/samples.csv"
SUMMARY="${OUT}/summary.txt"
STREAM="/tmp/rs_stress_$$.${CODEC}"

mkdir -p "${OUT}"
cd "${ROOT}" || exit 2

log() { printf '%s\n' "$*" | tee -a "${SUMMARY}"; }

if ! command -v ffmpeg >/dev/null 2>&1; then
  log "stress: not executed (ffmpeg not found)"
  exit 2
fi

case "${KBPS}" in
  '' | *[!0-9]*)
    printf 'stress: --kbps must be a non-negative integer\n' >&2
    exit 2
    ;;
esac

TRANSPORT_COUNT=0
for transport in ${TRANSPORTS}; do
  case "${transport}" in
    tcp | udp) TRANSPORT_COUNT=$((TRANSPORT_COUNT + 1)) ;;
    *)
      printf 'stress: unknown transport %s\n' "${transport}" >&2
      exit 2
      ;;
  esac
done

CLIENTS=$((CLIENTS_PER_TRANSPORT * TRANSPORT_COUNT))
log "stress: start $(date -Is) codec=${CODEC} duration=${DURATION}s interval=${INTERVAL}s clients=${CLIENTS}"

cmake --build "${BUILD}" -j"$(nproc)" >"${OUT}/build.log" 2>&1
if [ $? -ne 0 ]; then
  log "stress: build failed, see ${OUT}/build.log"
  exit 1
fi

if [ -n "${INPUT_FILE}" ]; then
  STREAM="${INPUT_FILE}"
  if [ ! -s "${STREAM}" ]; then
    log "stress: input file ${STREAM} is missing or empty"
    exit 1
  fi
  log "stress: input=${STREAM} bytes=$(wc -c <"${STREAM}")"
elif [ "${KBPS}" -gt 0 ]; then
  ./scripts/make_real_stream.sh "${STREAM}" "${CODEC}" "$((DURATION + 5))" "${KBPS}" >"${OUT}/stream.log" 2>&1
  if [ $? -ne 0 ]; then
    log "stress: stream generation failed, see ${OUT}/stream.log"
    exit 1
  fi
  log "stress: input=${STREAM} bytes=$(wc -c <"${STREAM}") target=${KBPS}kbps"
else
  "${BUILD}/make_test_stream" "${STREAM}" --aus "$((DURATION * 25))" --gop 25 --codec "${CODEC}" \
    >"${OUT}/stream.log" 2>&1
  if [ $? -ne 0 ]; then
    log "stress: stream generation failed, see ${OUT}/stream.log"
    exit 1
  fi
fi

CLIENT_MODE=(-c copy)
if [ "${DECODE}" -eq 1 ]; then
  CLIENT_MODE=()
fi

PORT=$((20000 + ($$ % 500)))
"${BUILD}/rtspserver_demo" --port "${PORT}" --file "${STREAM}" --ip 127.0.0.1 --codec auto \
  >"${OUT}/server.log" 2>&1 &
SERVER_PID=$!

READY=0
for _ in $(seq 1 100); do
  if (exec 3<>"/dev/tcp/127.0.0.1/${PORT}") 2>/dev/null; then
    READY=1
    break
  fi
  if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
if [ "${READY}" -ne 1 ]; then
  log "stress: server did not start on port ${PORT}"
  tee -a "${SUMMARY}" <"${OUT}/server.log"
  kill -KILL "${SERVER_PID}" 2>/dev/null
  exit 1
fi

read_rss_kb() { ps -o rss= -p "${SERVER_PID}" 2>/dev/null | tr -d ' '; }
read_fds() { ls "/proc/${SERVER_PID}/fd" 2>/dev/null | wc -l; }
read_threads() { awk '/^Threads:/{print $2}' "/proc/${SERVER_PID}/status" 2>/dev/null; }
read_cpu_ticks() { awk '{print $14 + $15}' "/proc/${SERVER_PID}/stat" 2>/dev/null; }

CLOCK_TICKS="$(getconf CLK_TCK 2>/dev/null || echo 100)"
BASELINE_RSS="$(read_rss_kb)"
BASELINE_FDS="$(read_fds)"
BASELINE_THREADS="$(read_threads)"
log "stress: baseline rss=${BASELINE_RSS}KB fds=${BASELINE_FDS} threads=${BASELINE_THREADS}"

PIDS=()
for transport in ${TRANSPORTS}; do
  for _ in $(seq 1 "${CLIENTS_PER_TRANSPORT}"); do
    # info + a long stats period keeps one final "video:...KiB" line per client
    # so UDP egress can be verified (server tx_bytes covers TCP control only).
    ffmpeg -nostdin -loglevel info -stats -stats_period 60 -rtsp_transport "${transport}" \
      -i "rtsp://127.0.0.1:${PORT}/live" -t "${DURATION}" "${CLIENT_MODE[@]}" -f null - \
      >/dev/null 2>>"${OUT}/client_${transport}.log" &
    PIDS+=($!)
  done
done

printf 'elapsed_s,rss_kb,fds,threads,cpu_pct,clients_alive\n' >"${CSV}"
START_MS="$(date +%s%3N)"
LAST_TICKS="$(read_cpu_ticks)"
LAST_MS="${START_MS}"

while :; do
  NOW_MS="$(date +%s%3N)"
  ELAPSED=$(((NOW_MS - START_MS) / 1000))
  if [ "${ELAPSED}" -ge "${DURATION}" ]; then
    break
  fi
  ALIVE=0
  for pid in "${PIDS[@]}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      ALIVE=$((ALIVE + 1))
    fi
  done
  TICKS="$(read_cpu_ticks)"
  DELTA_TICKS=$((TICKS - LAST_TICKS))
  DELTA_MS=$((NOW_MS - LAST_MS))
  CPU_PCT=0
  if [ "${DELTA_MS}" -gt 0 ]; then
    CPU_PCT="$(awk -v t="${DELTA_TICKS}" -v hz="${CLOCK_TICKS}" -v ms="${DELTA_MS}" \
      'BEGIN{printf "%.2f", (t / hz) * 100000 / ms}')"
  fi
  printf '%s,%s,%s,%s,%s,%s\n' "${ELAPSED}" "$(read_rss_kb)" "$(read_fds)" "$(read_threads)" \
    "${CPU_PCT}" "${ALIVE}" >>"${CSV}"
  LAST_TICKS="${TICKS}"
  LAST_MS="${NOW_MS}"
  if [ "${ALIVE}" -eq 0 ]; then
    log "stress: all clients exited early at ${ELAPSED}s"
    break
  fi
  sleep "${INTERVAL}"
done

# ffmpeg stops itself at -t DURATION; killing it at the same instant would
# report SIGTERM (143) as a client failure. Wait for natural exit first.
GRACE_DEADLINE=$((SECONDS + 15))
while [ "${SECONDS}" -lt "${GRACE_DEADLINE}" ]; do
  ALIVE=0
  for pid in "${PIDS[@]}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      ALIVE=$((ALIVE + 1))
    fi
  done
  if [ "${ALIVE}" -eq 0 ]; then
    break
  fi
  sleep 0.2
done
for pid in "${PIDS[@]}"; do
  kill -TERM "${pid}" 2>/dev/null
done
CLIENT_OK=0
CLIENT_FAIL=0
for pid in "${PIDS[@]}"; do
  wait "${pid}" 2>/dev/null
  CLIENT_RC=$?
  case "${CLIENT_RC}" in
    0 | 130 | 143) CLIENT_OK=$((CLIENT_OK + 1)) ;;
    *) CLIENT_FAIL=$((CLIENT_FAIL + 1)) ;;
  esac
done

FINAL_RSS="$(read_rss_kb)"
FINAL_FDS="$(read_fds)"
FINAL_THREADS="$(read_threads)"

STOP_START_MS="$(date +%s%3N)"
kill -TERM "${SERVER_PID}" 2>/dev/null
wait "${SERVER_PID}" 2>/dev/null
STOP_MS=$(( $(date +%s%3N) - STOP_START_MS ))

log "stress: final rss=${FINAL_RSS}KB fds=${FINAL_FDS} threads=${FINAL_THREADS} stop=${STOP_MS}ms"
log "stress: clients_ok=${CLIENT_OK} clients_fail=${CLIENT_FAIL} of ${CLIENTS} (decode=${DECODE})"
for transport in ${TRANSPORTS}; do
  VIDEO_KIB="$(grep -o 'video:[0-9]*KiB' "${OUT}/client_${transport}.log" 2>/dev/null |
    tr -dc '0-9\n' | awk '{sum += $1} END {print sum + 0}')"
  log "stress: transport=${transport} video_kib=${VIDEO_KIB}"
done
FINAL_STATS="$(grep -m1 '^\[final\]' "${OUT}/server.log" 2>/dev/null || true)"
if [ -n "${FINAL_STATS}" ]; then
  log "stress: ${FINAL_STATS}"
fi
awk -F',' 'NR > 1 {
  n++;
  rss += $2; fds += $3; threads += $4; cpu += $5;
  if (n == 1 || $2 < rss_min) rss_min = $2;
  if (n == 1 || $2 > rss_max) rss_max = $2;
  if (n == 1 || $3 > fds_max) fds_max = $3;
  if (n == 1 || $4 > threads_max) threads_max = $4;
  if (n == 1 || $5 > cpu_max) cpu_max = $5;
  if ($6 < clients_min || n == 1) clients_min = $6;
}
END {
  if (n == 0) { print "stress: no samples collected"; exit 1; }
  printf "samples=%d rss_avg=%dKB rss_min=%dKB rss_max=%dKB\n", n, rss/n, rss_min, rss_max;
  printf "fds_avg=%.1f fds_max=%d threads_avg=%.1f threads_max=%d\n", fds/n, fds_max, threads/n, threads_max;
  printf "cpu_avg=%.2f%% cpu_max=%.2f%% clients_min=%d\n", cpu/n, cpu_max, clients_min;
}' "${CSV}" >>"${SUMMARY}"

if [ -z "${INPUT_FILE}" ]; then
  rm -f "${STREAM}" "${STREAM}.expect"
fi
log "stress: done, csv=${CSV}"
exit 0
