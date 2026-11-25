#!/usr/bin/env bash
# Generate a decodable Annex B stream with ffmpeg (libx264 / libx265).
# usage: make_real_stream.sh <out.h264|out.h265> <h264|h265> [seconds]
# Encoder settings keep a fixed GOP and repeat parameter sets so the server can
# build SDP and a client can start decoding at every IDR.

# Re-exec under bash: arrays are bash-only, and a caller running `sh script.sh`
# must not turn into a syntax error.
if [ -z "${BASH_VERSION:-}" ]; then
  exec /bin/bash "$0" "$@"
fi

set -uo pipefail

usage() {
  printf 'usage: %s <out.h264|out.h265> <h264|h265> [seconds] [kbps]\n' "$0" >&2
  printf '  kbps 0 (default) keeps the const-QP 320x240 clip; >0 targets that bitrate\n' >&2
}

if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
  usage
  exit 2
fi

OUT="$1"
CODEC="$2"
SECONDS_ARG="${3:-4}"
KBPS="${4:-0}"

case "${KBPS}" in
  '' | *[!0-9]*)
    printf 'make_real_stream: kbps must be a non-negative integer\n' >&2
    usage
    exit 2
    ;;
esac

case "${CODEC}" in
  h264 | h265) ;;
  *)
    printf 'make_real_stream: unknown codec %s\n' "${CODEC}" >&2
    usage
    exit 2
    ;;
esac

if ! command -v ffmpeg >/dev/null 2>&1; then
  printf 'make_real_stream: ffmpeg not found in PATH\n' >&2
  exit 3
fi

FFMPEG_VERSION="$(ffmpeg -hide_banner -version 2>&1 | head -1)"

# A target bitrate needs enough pixels for x264/x265 to spend it; pick a
# resolution per rate instead of forcing 720p on the const-QP smoke clip.
SIZE="320x240"
RATE_ARGS=()
if [ "${KBPS}" -gt 0 ]; then
  if [ "${KBPS}" -ge 2000 ]; then
    SIZE="1280x720"
  elif [ "${KBPS}" -ge 1000 ]; then
    SIZE="960x540"
  else
    SIZE="640x360"
  fi
  RATE_ARGS=(-b:v "${KBPS}k" -maxrate "${KBPS}k" -bufsize "$((KBPS * 2))k")
fi

COMMON=(-hide_banner -nostdin -loglevel error -y
  -f lavfi -i "testsrc2=size=${SIZE}:rate=25"
  -t "${SECONDS_ARG}" -an -pix_fmt yuv420p
  -threads 4)

if [ "${CODEC}" = "h264" ]; then
  FORMAT_ARGS=(-c:v libx264 -preset veryfast
    -x264-params "keyint=25:min-keyint=25:scenecut=0:aud=1:repeat-headers=1:threads=4"
    -f h264 "${OUT}")
else
  FORMAT_ARGS=(-c:v libx265 -preset veryfast
    -x265-params "keyint=25:min-keyint=25:scenecut=0:aud=1:repeat-headers=1:pools=4:frame-threads=1:log-level=error"
    -f hevc "${OUT}")
fi

if ! ffmpeg "${COMMON[@]}" "${RATE_ARGS[@]}" "${FORMAT_ARGS[@]}" 2>&1; then
  printf 'make_real_stream: ffmpeg failed to write %s\n' "${OUT}" >&2
  printf 'make_real_stream: ffmpeg version: %s\n' "${FFMPEG_VERSION}" >&2
  exit 1
fi

if [ ! -s "${OUT}" ]; then
  printf 'make_real_stream: %s is empty\n' "${OUT}" >&2
  printf 'make_real_stream: ffmpeg version: %s\n' "${FFMPEG_VERSION}" >&2
  exit 1
fi

printf 'make_real_stream: %s (%s, %ss, target=%skbps) bytes=%s\n' "${OUT}" "${CODEC}" "${SECONDS_ARG}" \
  "${KBPS}" "$(wc -c <"${OUT}")"
printf 'make_real_stream: ffmpeg version: %s\n' "${FFMPEG_VERSION}"
exit 0
