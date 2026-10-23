#!/usr/bin/env bash
# Preview integration (SPECIFICATION §2.5, §9.3), no input route, no NMOS registry, no GPU:
#  1. SCOPES_PREVIEW_MODE=off: nothing published, no MediaMTX started.
#  2. webrtc, own mode: the built-in MediaMTX (`mediamtx` from PATH) gets the H.264 scope picture
#     (RTSP DESCRIBE and HLS), the preview state and metrics, the web UI and the widget routes;
#     SIGTERM stops MediaMTX.
#  3. webrtc, shared mode: a separate MediaMTX gets <prefix>/scope and none is started here; that
#     MediaMTX goes away (error) and comes back (publishing).
# Adapted from mxl-replay tests/integration/preview.sh (same author, used with permission).
set -euo pipefail

BIN=${1:-./build/mxl-scopes}
MEDIAMTX=${MEDIAMTX_BIN:-/tmp/mediamtx/mediamtx}
LIB_DIR="${MXL_LIB_DIR:-/opt/mxl/lib}"
if [[ ! -x "$BIN" || ! -x "$MEDIAMTX" ]]; then
  echo "missing binary: scopes=$BIN mediamtx=$MEDIAMTX" >&2
  exit 1
fi
export LD_LIBRARY_PATH="${LIB_DIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK=$(mktemp -d /dev/shm/scopes-preview-XXXXXX)
PID=""
SHARED_PID=""
cleanup() {
  local status=$?
  if [[ -n "$PID" ]]; then kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
  if [[ -n "$SHARED_PID" ]]; then kill "$SHARED_PID" 2>/dev/null || true; wait "$SHARED_PID" 2>/dev/null || true; fi
  if [[ "$status" -ne 0 ]]; then
    for log in "$WORK"/*.log; do echo "== $log" >&2; tail -40 "$log" >&2 || true; done
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

WEB_PORT=${SCOPES_TEST_PORT:-18190}
NMOS_PORT=13342
RTSP_PORT=18574
WHEP_PORT=18579
HLS_PORT=18578
ICE_PORT=18279
SHARED_RTSP_PORT=28574
SHARED_HLS_PORT=28578
HOST_ADDRESS=$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n 1 || true)
HOST_ADDRESS=${HOST_ADDRESS:-10.255.0.10}
# Own mode starts `mediamtx` from PATH (the image has it in /usr/local/bin).
export PATH="$(dirname "$MEDIAMTX"):$PATH"

start_scopes() { # <name> [KEY=value ...]
  local name=$1
  shift
  mkdir -p "$WORK/$name/config" "$WORK/$name/mxl"
  env MXL_DOMAIN_SCAN_PATH="$WORK/$name/mxl" MXL_OUTPUT_DOMAIN_DIR="$WORK/$name/mxl/scopes" SCOPES_STATE_DIR="$WORK/$name/config" \
    SCOPES_OUTPUT_FORMAT=640x360 NMOS_ENABLE=false NMOS_PORT="$NMOS_PORT" NMOS_HOST_ADDRESS="$HOST_ADDRESS" WEB_PORT="$WEB_PORT" \
    MEDIAMTX_RTSP_PORT="$RTSP_PORT" MEDIAMTX_WHEP_PORT="$WHEP_PORT" MEDIAMTX_HLS_PORT="$HLS_PORT" MEDIAMTX_ICE_UDP_PORT="$ICE_PORT" \
    WIDGET_FRAME_ANCESTORS="'self' http://designer.test" "$@" \
    "$BIN" >"$WORK/$name.log" 2>&1 &
  PID=$!
  for _ in $(seq 1 100); do
    if curl -sf "http://127.0.0.1:${WEB_PORT}/readyz" >/dev/null; then return 0; fi
    if ! kill -0 "$PID" 2>/dev/null; then echo "$name: mxl-scopes exited early" >&2; exit 1; fi
    sleep 0.2
  done
  echo "$name: /readyz did not answer" >&2
  exit 1
}

stop_scopes() {
  kill -TERM "$PID"
  local code=0
  wait "$PID" || code=$?
  PID=""
  if [[ "$code" != 143 ]]; then echo "exit code $code, expected 143" >&2; exit 1; fi
}

preview() { # <python expression on the preview object `p`>
  curl -sf "http://127.0.0.1:${WEB_PORT}/statusz" | python3 -c "import json,sys; p=json.load(sys.stdin)['preview']; print(($1))"
}

wait_state() { # <state> <seconds>
  for _ in $(seq 1 $(($2 * 4))); do
    if [[ "$(preview "p.get('state')")" == "$1" ]]; then return 0; fi
    sleep 0.25
  done
  echo "preview state is not $1: $(preview p)" >&2
  exit 1
}

# RTSP DESCRIBE: 200 with an H.264 track once the path has a publisher.
describe() { # <port> <path>
  python3 - "$1" "$2" <<'PY'
import socket, sys
port, path = int(sys.argv[1]), sys.argv[2]
try:
    s = socket.create_connection(("127.0.0.1", port), timeout=2)
    s.sendall(f"DESCRIBE rtsp://127.0.0.1:{port}/{path} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n".encode())
    reply = s.recv(4096).decode(errors="replace")
except OSError:
    sys.exit(1)
sys.exit(0 if reply.startswith("RTSP/1.0 200") and "H264" in reply else 1)
PY
}

wait_describe() { # <port> <path> <seconds>
  for _ in $(seq 1 $(($3 * 2))); do
    if describe "$1" "$2"; then return 0; fi
    sleep 0.5
  done
  echo "rtsp://127.0.0.1:$1/$2 has no H.264 publisher" >&2
  exit 1
}

closed() { # <port>: nothing listens there
  ! python3 -c "import socket,sys; socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=1)" "$1" 2>/dev/null
}

# ---- 1. off ---------------------------------------------------------------------------------------
start_scopes off SCOPES_PREVIEW_MODE=off
[[ "$(preview "p['mode'], p['state']")" == "('off', 'off')" ]] || { echo "off: $(preview p)" >&2; exit 1; }
sleep 1
closed "$RTSP_PORT" || { echo "off: a MediaMTX runs" >&2; exit 1; }
! grep -q '^mxl_scopes_preview_publish_state' <<<"$(curl -sf "http://127.0.0.1:${WEB_PORT}/metrics")" || { echo "off: preview metrics" >&2; exit 1; }
stop_scopes
echo "off: ok"

# ---- 2. webrtc, own MediaMTX ----------------------------------------------------------------------
start_scopes own
wait_state publishing 20
wait_describe "$RTSP_PORT" mxl-scopes/scope 10
[[ "$(preview "p['publish'], p['encoder'], p['width'], p['height'], p['mediamtx']['running']")" == "('own', 'x264', 640, 360, True)" ]] \
  || { echo "own: $(preview p)" >&2; exit 1; }
[[ "$(preview "p['whep']")" == "http://${HOST_ADDRESS}:${WHEP_PORT}/mxl-scopes/scope/whep" ]] || { echo "own: whep $(preview p)" >&2; exit 1; }
for _ in $(seq 1 40); do
  if [[ "$(curl -sL -o /dev/null -w '%{http_code}' "http://127.0.0.1:${HLS_PORT}/mxl-scopes/scope/index.m3u8")" == 200 ]]; then break; fi
  sleep 0.5
done
[[ "$(curl -sL -o /dev/null -w '%{http_code}' "http://127.0.0.1:${HLS_PORT}/mxl-scopes/scope/index.m3u8")" == 200 ]] || { echo "own: no HLS playlist" >&2; exit 1; }
METRICS=$(curl -sf "http://127.0.0.1:${WEB_PORT}/metrics")
for line in 'mxl_scopes_preview_publish_mode{mode="own"} 1' 'mxl_scopes_preview_publish_state{state="publishing"} 1' 'mxl_scopes_preview_encoder{encoder="x264"} 1' \
  'mxl_scopes_preview_encode_seconds_count'; do
  grep -qF "$line" <<<"$METRICS" || { echo "own: metric $line" >&2; exit 1; }
done
FRAMES=$(grep '^mxl_scopes_preview_frames_total ' <<<"$METRICS" | awk '{print $2}')
(( FRAMES > 0 )) || { echo "own: preview_frames_total $FRAMES" >&2; exit 1; }
[[ -f "$WORK/own/config/mediamtx.yml" ]] || { echo "own: no mediamtx.yml in the state directory" >&2; exit 1; }
# The web UI and the scope widget (§9.1, §9.3): the embedded Vue page; /widgets with CORS for a
# listed origin; /widget/scope with frame-ancestors.
curl -sf "http://127.0.0.1:${WEB_PORT}/" | grep -q '<div id="app">' || { echo "own: the web UI is not embedded" >&2; exit 1; }
HDRS=$(curl -s -D - -o /dev/null -H 'Origin: http://designer.test' "http://127.0.0.1:${WEB_PORT}/widgets")
grep -qi '^access-control-allow-origin: http://designer.test' <<<"$HDRS" || { echo "own: /widgets CORS: $HDRS" >&2; exit 1; }
HDRS=$(curl -s -D - -o /dev/null "http://127.0.0.1:${WEB_PORT}/widget/scope?controls=true&measurements=true&theme=dark")
grep -qi "^content-security-policy: frame-ancestors 'self' http://designer.test" <<<"$HDRS" || { echo "own: widget CSP: $HDRS" >&2; exit 1; }
! grep -qi '^x-frame-options' <<<"$HDRS" || { echo "own: widget has X-Frame-Options" >&2; exit 1; }
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${WEB_PORT}/widget/scope?theme=blue")" == 400 ]] || { echo "own: widget accepted theme=blue" >&2; exit 1; }
stop_scopes
sleep 1
closed "$RTSP_PORT" || { echo "own: MediaMTX still runs after SIGTERM" >&2; exit 1; }
echo "own MediaMTX: ok"

# ---- 3. webrtc, shared MediaMTX -------------------------------------------------------------------
shared_up() {
  cat >"$WORK/shared.yml" <<EOF
logLevel: warn
api: false
rtspAddress: 127.0.0.1:${SHARED_RTSP_PORT}
rtspTransports: [tcp]
rtmp: false
srt: false
webrtc: false
hlsAddress: :${SHARED_HLS_PORT}
paths:
  all_others:
EOF
  "$MEDIAMTX" "$WORK/shared.yml" >"$WORK/shared-mediamtx.log" 2>&1 &
  SHARED_PID=$!
}
shared_up
start_scopes shared PREVIEW_PUBLISH_URL="rtsp://127.0.0.1:${SHARED_RTSP_PORT}" PREVIEW_PATH_PREFIX=prod/scope1
wait_state publishing 20
wait_describe "$SHARED_RTSP_PORT" prod/scope1/scope 10
[[ "$(preview "p['publish']")" == shared ]] || { echo "shared: $(preview p)" >&2; exit 1; }
closed "$RTSP_PORT" || { echo "shared: an own MediaMTX runs" >&2; exit 1; }
kill "$SHARED_PID"; wait "$SHARED_PID" 2>/dev/null || true; SHARED_PID=""
wait_state error 15
shared_up
wait_state publishing 20
stop_scopes
echo "shared MediaMTX: ok"
echo "preview ok"
