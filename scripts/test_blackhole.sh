#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-${ROOT}/build}"
TRANSPORT="${2:-tcp}"

if [[ "${1:-}" != "--isolated" ]]; then
  TRANSPORT="${1:-tcp}"
  if [[ "${TRANSPORT}" != "tcp" && "${TRANSPORT}" != "udp" ]]; then
    printf 'usage: %s [tcp|udp]\n' "$0" >&2
    exit 2
  fi
  exec unshare --user --map-root-user --net --pid --fork --mount-proc --kill-child \
    /bin/bash "$0" --isolated "${TRANSPORT}"
fi

if [[ "$$" != 1 ]]; then
  printf 'test_blackhole: must run as PID 1 in a private network namespace\n' >&2
  exit 1
fi

for command in ip iptables ss ffmpeg timeout awk; do
  if ! command -v "${command}" >/dev/null; then
    printf 'test_blackhole: missing %s\n' "${command}" >&2
    exit 1
  fi
done

OUT="$(mktemp -d /tmp/rs-blackhole-XXXXXX)"
PORT=18554
ip link set lo up
"${ROOT}/scripts/make_real_stream.sh" "${OUT}/input.h264" h264 35 4000 >"${OUT}/stream.log" 2>&1
"${BUILD}/rtspserver_demo" --port "${PORT}" --file "${OUT}/input.h264" --ip 127.0.0.1 \
  >"${OUT}/server.log" 2>&1 &
server_pid=$!

for attempt in $(seq 1 100); do
  if ss -Hltn "sport = :${PORT}" | grep -q .; then
    break
  fi
  if ! kill -0 "${server_pid}" 2>/dev/null; then
    cat "${OUT}/server.log" >&2
    exit 1
  fi
  sleep 0.1
done
if ! ss -Hltn "sport = :${PORT}" | grep -q .; then
  printf 'test_blackhole: listener not ready\n' >&2
  exit 1
fi

ffmpeg -nostdin -loglevel error -rtsp_transport "${TRANSPORT}" -i "rtsp://127.0.0.1:${PORT}/live" \
  -t 90 -c:v copy -f null - >"${OUT}/blackhole-client.log" 2>&1 &
client_pid=$!

client_port=""
for attempt in $(seq 1 100); do
  client_port="$(ss -Htn state established "dport = :${PORT}" | awk 'NR == 1 {n=split($3,a,":"); print a[n]}')"
  if [[ "${client_port}" =~ ^[0-9]+$ ]]; then
    break
  fi
  sleep 0.1
done
if [[ ! "${client_port}" =~ ^[0-9]+$ ]]; then
  printf 'test_blackhole: first client did not connect\n' >&2
  exit 1
fi
if [[ "${TRANSPORT}" == "udp" ]]; then
  udp_ports=""
  for attempt in $(seq 1 100); do
    udp_ports="$(ss -Huanp | awk -v pid="pid=${client_pid}," \
      'index($0,pid) {n=split($4,a,":"); print a[n]}' | sort -un)"
    if [[ "$(printf '%s\n' "${udp_ports}" | grep -c '^[0-9]\+$')" -ge 2 ]]; then
      break
    fi
    sleep 0.1
  done
  if [[ "$(printf '%s\n' "${udp_ports}" | grep -c '^[0-9]\+$')" -lt 2 ]]; then
    printf 'test_blackhole: UDP ports not ready\n' >&2
    exit 1
  fi
fi
sleep 2
iptables -A INPUT -p tcp --sport "${client_port}" -j DROP
iptables -A OUTPUT -p tcp --dport "${client_port}" -j DROP
if [[ "${TRANSPORT}" == "udp" ]]; then
  for udp_port in ${udp_ports}; do
    iptables -A INPUT -p udp --sport "${udp_port}" -j DROP
    iptables -A OUTPUT -p udp --dport "${udp_port}" -j DROP
  done
  duration=68
  min_frames=1200
else
  duration=12
  min_frames=200
fi

timeout "$((duration + 20))s" ffmpeg -nostdin -loglevel error -rtsp_transport "${TRANSPORT}" \
  -i "rtsp://127.0.0.1:${PORT}/live" -t "${duration}" -c:v copy \
  -progress "${OUT}/healthy.progress" -stats_period 1 -f null - >"${OUT}/healthy.log" 2>&1 &
healthy_pid=$!

closed=0
rss_min=0
rss_max=0
for attempt in $(seq 1 "$(((duration + 10) * 10))"); do
  if ! kill -0 "${server_pid}" 2>/dev/null; then
    printf 'test_blackhole: server exited unexpectedly\n' >&2
    exit 1
  fi
  rss="$(awk '/^VmRSS:/{print $2}' "/proc/${server_pid}/status")"
  if [[ "${rss}" =~ ^[0-9]+$ ]]; then
    if (( rss_min == 0 || rss < rss_min )); then rss_min="${rss}"; fi
    if (( rss > rss_max )); then rss_max="${rss}"; fi
  fi
  if ! ss -Htnp "sport = :${PORT} and dport = :${client_port}" | grep -q "pid=${server_pid},"; then
    closed=1
  fi
  if ! kill -0 "${healthy_pid}" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
if ! wait "${healthy_pid}"; then
  cat "${OUT}/healthy.log" >&2
  printf 'test_blackhole: healthy client failed\n' >&2
  exit 1
fi
frames="$(awk -F= '$1 == "frame" {value=$2} END {gsub(/ /,"",value); print value+0}' "${OUT}/healthy.progress")"
if (( closed != 1 || frames < min_frames )); then
  printf 'test_blackhole: closed=%s healthy_frames=%s\n' "${closed}" "${frames}" >&2
  exit 1
fi
printf 'test_blackhole: PASS transport=%s closed=%s healthy_frames=%s rss_min_kib=%s rss_max_kib=%s output=%s\n' \
  "${TRANSPORT}" "${closed}" "${frames}" "${rss_min}" "${rss_max}" "${OUT}"
