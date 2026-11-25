#!/usr/bin/env bash
# Build, run unit/E2E tests and play both codecs with ffprobe/ffmpeg over
# RTSP/TCP and RTSP/UDP. Every result is written to build/e2e/run_e2e.log;
# nothing is reported as passing when the corresponding command failed.

# Re-exec under bash so `sh scripts/run_e2e.sh` behaves like `bash`/direct exec.
if [ -z "${BASH_VERSION:-}" ]; then
  exec /bin/bash "$0" "$@"
fi

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"
OUT="${BUILD}/e2e"
LOG="${OUT}/run_e2e.log"
STAMP="$$"
SYNTH_H264="/tmp/rs_run_e2e_${STAMP}.h264"
SYNTH_H265="/tmp/rs_run_e2e_${STAMP}.h265"
REAL_H264="${OUT}/real_${STAMP}.h264"
REAL_H265="${OUT}/real_${STAMP}.h265"
mkdir -p "${OUT}"
cd "${ROOT}" || exit 2

: >"${LOG}"
log() { printf '%s\n' "$*" | tee -a "${LOG}"; }
now_ms() { date +%s%3N; }

rc_configure=0
rc_build=0
rc_unit=0
rc_stream=0
rc_e2e=0
rc_probe=0

log "== configure =="
cmake -S . -B build -DRS_BUILD_E2E=ON 2>&1 | tee -a "${LOG}"
rc_configure=${PIPESTATUS[0]}

log "== build =="
cmake --build build -j"$(nproc)" 2>&1 | tee -a "${LOG}"
rc_build=${PIPESTATUS[0]}

if [ "${rc_build}" -eq 0 ]; then
  log "== unit: rtp_transport / packetizers / sdp =="
  ctest --test-dir build --output-on-failure \
    -R 'test_(rtp_transport|h264_packetizer|h265_packetizer|sdp)$' 2>&1 | tee -a "${LOG}"
  rc_unit=${PIPESTATUS[0]}

  log "== generate synthetic streams (make_test_stream) =="
  ./build/make_test_stream "${SYNTH_H264}" --aus 60 2>&1 | tee -a "${LOG}"
  rc_h264=${PIPESTATUS[0]}
  ./build/make_test_stream "${SYNTH_H265}" --aus 60 --codec h265 2>&1 | tee -a "${LOG}"
  rc_h265=${PIPESTATUS[0]}
  rc_stream=$((rc_h264 | rc_h265))
  sha256sum "${SYNTH_H264}" "${SYNTH_H264}.expect" "${SYNTH_H265}" "${SYNTH_H265}.expect" 2>&1 | tee -a "${LOG}"
  if command -v ffprobe >/dev/null 2>&1; then
    log "-- ffprobe -show_streams (synthetic H.265: headers only, slice payload is random) --"
    ffprobe -v error -show_entries stream=codec_name,profile,width,height \
      -of default=noprint_wrappers=1 "${SYNTH_H265}" 2>&1 | tee -a "${LOG}"
  fi

  log "== e2e: test_e2e =="
  t0="$(now_ms)"
  ctest --test-dir build --output-on-failure -R 'test_e2e$' 2>&1 | tee -a "${LOG}"
  rc_e2e=${PIPESTATUS[0]}
  log "test_e2e elapsed=$(( $(now_ms) - t0 )) ms exit=${rc_e2e}"
else
  log "build failed; unit and e2e tests not run"
  rc_unit=1
  rc_stream=1
  rc_e2e=1
fi

# ---- real-stream playback probes (ffprobe + ffmpeg, TCP and UDP) -----------
if ! command -v ffmpeg >/dev/null 2>&1 || ! command -v ffprobe >/dev/null 2>&1; then
  log "real-stream probe: not executed (ffmpeg and/or ffprobe missing)"
  rc_probe=1
elif [ "${rc_build}" -ne 0 ]; then
  log "real-stream probe: not executed (build failed)"
  rc_probe=1
else
  for codec in h264 h265; do
    if [ "${codec}" = "h264" ]; then
      STREAM="${REAL_H264}"
    else
      STREAM="${REAL_H265}"
    fi
    log "== make_real_stream ${codec} =="
    t0="$(now_ms)"
    /bin/bash ./scripts/make_real_stream.sh "${STREAM}" "${codec}" 4 2>&1 | tee -a "${LOG}"
    rc_gen=${PIPESTATUS[0]}
    log "make_real_stream ${codec} elapsed=$(( $(now_ms) - t0 )) ms exit=${rc_gen}"
    if [ "${rc_gen}" -ne 0 ]; then
      log "probe ${codec}: skipped (stream generation failed)"
      rc_probe=1
      continue
    fi
    ffprobe -v error -show_entries stream=codec_name,width,height -of default=noprint_wrappers=1 \
      "${STREAM}" 2>&1 | tee -a "${LOG}"

    PORT=$((21000 + (STAMP % 400) * 2 + ($([ "${codec}" = "h265" ] && echo 1 || echo 0))))
    SERVER_LOG="${OUT}/server_probe_${codec}_${STAMP}.log"
    ./build/rtspserver_demo --port "${PORT}" --file "${STREAM}" --ip 127.0.0.1 --codec auto \
      >"${SERVER_LOG}" 2>&1 &
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
      log "probe ${codec}: server did not start on port ${PORT}"
      tee -a "${LOG}" <"${SERVER_LOG}"
      kill -KILL "${SERVER_PID}" 2>/dev/null
      wait "${SERVER_PID}" 2>/dev/null
      rc_probe=1
      continue
    fi
    URL="rtsp://127.0.0.1:${PORT}/live"
    for transport in tcp udp; do
      PROBE_LOG="${OUT}/ffprobe_${codec}_${transport}_${STAMP}.log"
      t0="$(now_ms)"
      timeout 20 ffprobe -v error -rtsp_transport "${transport}" -select_streams v:0 \
        -show_entries stream=codec_name,width,height,avg_frame_rate -of default=noprint_wrappers=1 \
        "${URL}" >"${PROBE_LOG}" 2>&1
      PROBE_RC=$?
      log "ffprobe ${codec}/${transport} exit=${PROBE_RC} elapsed=$(( $(now_ms) - t0 )) ms"
      tee -a "${LOG}" <"${PROBE_LOG}"
      [ "${PROBE_RC}" -ne 0 ] && rc_probe=1

      DECODE_LOG="${OUT}/ffmpeg_${codec}_${transport}_${STAMP}.log"
      t0="$(now_ms)"
      timeout 30 ffmpeg -hide_banner -v error -rtsp_transport "${transport}" -i "${URL}" \
        -frames:v 25 -f null - >"${DECODE_LOG}" 2>&1
      DECODE_RC=$?
      log "ffmpeg ${codec}/${transport} exit=${DECODE_RC} elapsed=$(( $(now_ms) - t0 )) ms"
      tee -a "${LOG}" <"${DECODE_LOG}"
      [ "${DECODE_RC}" -ne 0 ] && rc_probe=1
    done
    kill -TERM "${SERVER_PID}" 2>/dev/null
    wait "${SERVER_PID}" 2>/dev/null
    tee -a "${LOG}" <"${SERVER_LOG}"
  done
fi

rm -f "${SYNTH_H264}" "${SYNTH_H264}.expect" "${SYNTH_H265}" "${SYNTH_H265}.expect"

log "== summary =="
log "configure=${rc_configure} build=${rc_build} unit=${rc_unit} stream=${rc_stream} e2e=${rc_e2e} probe=${rc_probe}"
if [ "${rc_configure}" -ne 0 ] || [ "${rc_build}" -ne 0 ] || [ "${rc_unit}" -ne 0 ] ||
  [ "${rc_stream}" -ne 0 ] || [ "${rc_e2e}" -ne 0 ] || [ "${rc_probe}" -ne 0 ]; then
  exit 1
fi
exit 0
