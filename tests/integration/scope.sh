#!/usr/bin/env bash
# CPU integration (SPECIFICATION §14.2): a restored route to a missing flow, IS-05 activation,
# a real MXL pattern writer, the output flow and its pixels, a live settings change, the update
# rate, metrics, snapshot, export and import, no signal, SIGTERM with deregistration and domain
# removal.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-scopes}"
WRITER="${2:-$ROOT/build/mxl-scopes-writer}"
SAMPLE="${3:-$ROOT/build/mxl-scopes-sample}"
LIB_DIR="${MXL_LIB_DIR:-/opt/mxl/lib}"

for tool in "$BIN" "$WRITER" "$SAMPLE"; do
  [[ -x "$tool" ]] || { echo "missing $tool (pass mxl-scopes mxl-scopes-writer mxl-scopes-sample)" >&2; exit 1; }
done

WORK="$(mktemp -d /dev/shm/scopes-int-XXXXXX)"
cleanup() {
  [[ -n "${SC_PID:-}" ]] && { kill "$SC_PID" 2>/dev/null || true; wait "$SC_PID" 2>/dev/null || true; }
  [[ -n "${W1:-}" ]] && { kill "$W1" 2>/dev/null || true; }
  [[ -n "${W2:-}" ]] && { kill "$W2" 2>/dev/null || true; }
  [[ -n "${REG_PID:-}" ]] && { kill "$REG_PID" 2>/dev/null || true; }
  rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  echo "--- scopes log (tail)" >&2
  tail -40 "$WORK/scopes.log" >&2 || true
  exit 1
}

DOMAIN_ID="aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
FLOW_ID="bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
REG_PORT=13210
WEB_PORT=18170
NMOS_PORT=13322
API="http://127.0.0.1:${WEB_PORT}"

mkdir -p "$WORK/src" "$WORK/config"
# A persisted route to a flow that does not exist yet: restored at start, then `waiting`.
cat > "$WORK/config/routes.json" <<EOF
{"routes":[{"input":1,"video":true,"enable":true,"domain_id":"$DOMAIN_ID","flow_id":"$FLOW_ID","sender_id":""}]}
EOF
echo "{\"id\":\"$DOMAIN_ID\",\"label\":\"src\",\"description\":\"test source\",\"tags\":{}}" > "$WORK/src/domain_def.json"
echo '{"urn:x-mxl:option:history_duration/v1.0":200000000}' > "$WORK/src/options.json"

python3 "$ROOT/tests/integration/registry_stub.py" "$REG_PORT" >"$WORK/registry.log" 2>&1 &
REG_PID=$!

export LD_LIBRARY_PATH="${LIB_DIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
MXL_DOMAIN_SCAN_PATH="$WORK" \
MXL_OUTPUT_DOMAIN_DIR="$WORK/scopes" \
MXL_CLEANUP_ON_EXIT=true \
SCOPES_STATE_DIR="$WORK/config" \
SCOPES_OUTPUT_FORMAT=640x360 \
SCOPES_UPDATE_RATE=25 \
SCOPES_HOLD_MS=600 \
SCOPES_PREVIEW_MODE=off \
NMOS_ENABLE=true \
NMOS_REGISTRY_ADDRESS=127.0.0.1 \
NMOS_REGISTRY_PORT="$REG_PORT" \
NMOS_PORT="$NMOS_PORT" \
NMOS_SEED=scopes-test \
WEB_PORT="$WEB_PORT" \
LOG_LEVEL=info \
"$BIN" >"$WORK/scopes.log" 2>&1 &
SC_PID=$!

json() { python3 -c "import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1], {'d': d}))" "$1"; }
input_state() { curl -sf "$API/api/v1/input" | json 'd["state"]'; }
wait_state() {
  local want="$1" got=""
  for _ in $(seq 1 60); do
    got="$(input_state || true)"
    [[ "$got" == "$want" ]] && return 0
    sleep 0.2
  done
  fail "expected input state $want, got ${got:-none}"
}

for _ in $(seq 1 50); do
  curl -sf "$API/livez" >/dev/null && break
  kill -0 "$SC_PID" 2>/dev/null || fail "mxl-scopes exited early"
  sleep 0.2
done
curl -sf "$API/livez" >/dev/null || fail "livez"

wait_state waiting
echo "restored route: waiting"

ready=0
for _ in $(seq 1 50); do
  [[ "$(curl -s -o /dev/null -w '%{http_code}' "$API/readyz")" == "200" ]] && { ready=1; break; }
  sleep 0.2
done
[[ "$ready" == 1 ]] || fail "readyz did not become 200: $(curl -s "$API/readyz")"
echo "ready"

INFO="$(curl -sf "$API/api/v1/info")"
RX="$(echo "$INFO" | json 'd["receiver_id"]')"
NODE="$(echo "$INFO" | json 'd["node_id"]')"

# The restored route is the receiver's IS-05 active state.
ACTIVE="$(curl -sf "http://127.0.0.1:${NMOS_PORT}/x-nmos/connection/v1.2/single/receivers/${RX}/active")"
echo "$ACTIVE" | python3 -c 'import json,sys; a=json.load(sys.stdin); p=a["transport_params"][0]; sys.exit(0 if a["master_enable"] and p["mxl_flow_id"]==sys.argv[1] else 1)' "$FLOW_ID" \
  || fail "restored route is not the IS-05 active state: $ACTIVE"

# IS-05 activation (the same flow, still missing).
BODY="{\"sender_id\":null,\"master_enable\":true,\"activation\":{\"mode\":\"activate_immediate\"},\"transport_params\":[{\"mxl_domain_id\":\"$DOMAIN_ID\",\"mxl_flow_id\":\"$FLOW_ID\"}]}"
CODE="$(curl -s -o "$WORK/patch.txt" -w '%{http_code}' -X PATCH -H 'Content-Type: application/json' -d "$BODY" \
  "http://127.0.0.1:${NMOS_PORT}/x-nmos/connection/v1.2/single/receivers/${RX}/staged")"
[[ "$CODE" == "200" || "$CODE" == "202" ]] || fail "IS-05 PATCH: $CODE $(cat "$WORK/patch.txt")"
wait_state waiting

# The writer starts: a flat grey at code 512, 320x180 50p.
"$WRITER" --domain "$WORK/src" --flow "$FLOW_ID" --pattern flat --level 512 --width 320 --height 180 --rate 50 --label "Test grey" \
  >"$WORK/writer.log" 2>&1 &
W1=$!
wait_state running
echo "running"

python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if all(k in d for k in ("id","label","description")) and isinstance(d.get("tags"), dict) else 1)' \
  "$WORK/scopes/domain_def.json" || fail "output domain_def.json is not BCP-007-03"

OUT="$(curl -sf "$API/api/v1/output")"
FLOW_OUT="$(echo "$OUT" | json 'd["flow_id"]')"
python3 - "$WORK/scopes/$FLOW_OUT.mxl-flow/flow_def.json" <<'PY' || fail "output flow_def.json"
import json, sys
doc = json.load(open(sys.argv[1]))
assert doc["frame_width"] == 640, doc
assert doc["frame_height"] == 360, doc
assert doc["media_type"] == "video/v210", doc
assert doc["grain_rate"] == {"numerator": 50, "denominator": 1}, doc
assert doc["description"] == "Luma waveform", doc
PY

# The trace: code 512 is one row of the plot; the rows around it are background.
read -r PX PY PH <<<"$(echo "$OUT" | json '"%d %d %d" % (d["plot"]["x"], d["plot"]["y"], d["plot"]["height"])')"
ROW=$(( PY + PH - 1 - (512 * PH >> 10) ))
COL=$(( PX + 101 ))
sleep 0.3
mapfile -t PIX < <("$SAMPLE" --domain "$WORK/scopes" --flow "$FLOW_OUT" --width 640 --at "$COL,$ROW" --at "$COL,$((ROW - 12))" --at "$COL,$((ROW + 12))")
echo "trace=${PIX[0]} above=${PIX[1]} below=${PIX[2]}"
(( PIX[0] > 400 )) || fail "no trace at row $ROW"
(( PIX[1] == 64 && PIX[2] == 64 )) || fail "background around the trace is not black"

# Live settings change: no new flow id.
curl -sf -X PATCH -H 'Content-Type: application/json' -d '{"graticule_units":"percent","update_rate":10}' "$API/api/v1/scope" >/dev/null || fail "PATCH scope"
[[ "$(curl -sf "$API/api/v1/scope" | json 'd["update_rate"]')" == "10" ]] || fail "update_rate not applied"
sleep 0.5
[[ "$(curl -sf "$API/api/v1/output" | json 'd["flow_id"]')" == "$FLOW_OUT" ]] || fail "a live change minted a new flow id"
curl -s -X PATCH -H 'Content-Type: application/json' -d '{"intensity":0}' -o /dev/null -w '%{http_code}' "$API/api/v1/scope" | grep -q 400 \
  || fail "an invalid PATCH was accepted"

# The update counter follows the update rate (10 Hz from 50p) over 4 s.
U0="$(curl -sf "$API/api/v1/status" | json 'd["renderer"]["updates"]')"
sleep 4
U1="$(curl -sf "$API/api/v1/status" | json 'd["renderer"]["updates"]')"
echo "updates in 4 s at 10 Hz: $((U1 - U0))"
(( U1 - U0 >= 34 && U1 - U0 <= 46 )) || fail "update count $((U1 - U0)) does not match 10 Hz"

# SCOPES_ACCUMULATE=all: every input frame goes into the update; the update rate stays.
curl -sf -X PATCH -H 'Content-Type: application/json' -d '{"accumulate":"all"}' "$API/api/v1/scope" >/dev/null || fail "PATCH accumulate"
sleep 0.5
U0="$(curl -sf "$API/api/v1/status" | json 'd["renderer"]["updates"]')"
sleep 4
U1="$(curl -sf "$API/api/v1/status" | json 'd["renderer"]["updates"]')"
echo "accumulate all: $((U1 - U0)) updates in 4 s at 10 Hz"
(( U1 - U0 >= 34 && U1 - U0 <= 46 )) || fail "accumulate all made $((U1 - U0)) updates in 4 s"
TRACE="$("$SAMPLE" --domain "$WORK/scopes" --flow "$FLOW_OUT" --width 640 --at "$COL,$ROW")"
(( TRACE > 400 )) || fail "no trace with accumulate all ($TRACE)"
curl -sf -X PATCH -H 'Content-Type: application/json' -d '{"accumulate":"latest"}' "$API/api/v1/scope" >/dev/null

# At 1 Hz a mode change must show at once, not at the next tick (the picture is redrawn when
# the settings change, also while the input holds).
curl -sf -X PATCH -H 'Content-Type: application/json' -d '{"update_rate":1}' "$API/api/v1/scope" >/dev/null || fail "PATCH 1 Hz"
# Live mode changes keep the flow id; NMOS describes the new mode. A flat grey is R′ = G′ =
# B′ = Cb = Cr = 512: one trace row in every panel of the parades.
for mode in rgb-parade ycbcr-parade rgb-overlay; do
  curl -sf -X PATCH -H 'Content-Type: application/json' -d "{\"mode\":\"$mode\"}" "$API/api/v1/scope" >/dev/null || fail "PATCH mode $mode"
  sleep 0.6
  OUTM="$(curl -sf "$API/api/v1/output")"
  [[ "$(echo "$OUTM" | json 'd["flow_id"]')" == "$FLOW_OUT" ]] || fail "mode $mode minted a new flow id"
  [[ "$(echo "$OUTM" | json 'd["mode"]')" == "$mode" ]] || fail "/api/v1/output does not show $mode"
  read -r PY PH <<<"$(echo "$OUTM" | json '"%d %d" % (d["plot"]["y"], d["plot"]["height"])')"
  ROWM=$(( PY + PH - 1 - (512 * PH >> 10) ))
  AT=()
  for x in $(echo "$OUTM" | json '" ".join(str(p["x"] + p["width"] // 3) for p in d["plot"]["panels"])'); do
    AT+=(--at "$x,$ROWM" --at "$x,$((ROWM - 12))")
  done
  mapfile -t PIXM < <("$SAMPLE" --domain "$WORK/scopes" --flow "$FLOW_OUT" --width 640 "${AT[@]}")
  echo "$mode: ${PIXM[*]}"
  for ((i = 0; i < ${#PIXM[@]}; i += 2)); do
    (( PIXM[i] > 200 )) || fail "$mode: no trace in panel $((i / 2)) (${PIXM[*]})"
    (( PIXM[i + 1] == 64 )) || fail "$mode: panel $((i / 2)) not black off the trace (${PIXM[*]})"
  done
  DESC="$(curl -sf "http://127.0.0.1:${NMOS_PORT}/x-nmos/node/v1.3/flows/${FLOW_OUT}" | json 'd["description"]')"
  echo "$mode: NMOS flow description $DESC"
  [[ "$DESC" == "$(curl -sf "$API/api/v1/scope" | json 'd["title"]')" ]] || fail "NMOS flow description $DESC does not follow $mode"
done
curl -sf -X PATCH -H 'Content-Type: application/json' -d '{"update_rate":10}' "$API/api/v1/scope" >/dev/null || fail "PATCH 10 Hz"
grep -q 'mxl_scopes_updates_total{mode="rgb-parade"}' <<<"$(curl -sf "$API/metrics")" || fail "no updates_total for rgb-parade"
curl -sf -X PATCH -H 'Content-Type: application/json' -d '{"mode":"luma"}' "$API/api/v1/scope" >/dev/null || fail "PATCH mode luma"
curl -s -X PATCH -H 'Content-Type: application/json' -d '{"mode":"vectorscope"}' -o /dev/null -w '%{http_code}' "$API/api/v1/scope" | grep -q 400 \
  || fail "a mode that is not built was accepted"
echo "modes ok"

METRICS="$(curl -sf "$API/metrics")"
for name in mxl_scopes_accumulate_dropped_total mxl_scopes_updates_total mxl_scopes_analyse_seconds_bucket mxl_scopes_output_frames_total 'mxl_scopes_input_state{state="running"} 1' mxl_scopes_simd_level; do
  grep -q "$name" <<<"$METRICS" || fail "metric $name missing"
done

curl -sf -o "$WORK/snap.png" "$API/api/v1/snapshot.png" || fail "snapshot"
python3 -c 'import sys; sys.exit(0 if open(sys.argv[1],"rb").read(8)==b"\x89PNG\r\n\x1a\n" else 1)' "$WORK/snap.png" || fail "snapshot is not a PNG"

EXPORT="$(curl -sf "$API/api/v1/config/export")"
echo "$EXPORT" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["secrets"] is False and d["routes"]["routes"][0]["flow_id"]=="'"$FLOW_ID"'"' || fail "export"
curl -sf -X POST -H 'Content-Type: application/json' -d "$EXPORT" "$API/api/v1/config/import" >/dev/null || fail "import"

# The writer stops: the picture holds, then NO SIGNAL without a trace.
kill "$W1"; wait "$W1" 2>/dev/null || true; W1=
wait_state no_signal
sleep 0.3
TRACE="$("$SAMPLE" --domain "$WORK/scopes" --flow "$FLOW_OUT" --width 640 --at "$COL,$ROW")"
(( TRACE < 400 )) || fail "the trace is still shown after no signal ($TRACE)"
echo "no signal"

# Route the receiver to another flow over IS-05.
route() {
  local body="{\"sender_id\":null,\"master_enable\":true,\"activation\":{\"mode\":\"activate_immediate\"},\"transport_params\":[{\"mxl_domain_id\":\"$DOMAIN_ID\",\"mxl_flow_id\":\"$1\"}]}"
  local code
  code="$(curl -s -o /dev/null -w '%{http_code}' -X PATCH -H 'Content-Type: application/json' -d "$body" \
    "http://127.0.0.1:${NMOS_PORT}/x-nmos/connection/v1.2/single/receivers/${RX}/staged")"
  [[ "$code" == "200" || "$code" == "202" ]] || fail "IS-05 PATCH to $1: $code"
}
# CPU of the scopes process over $1 seconds, in percent of one core.
cpu_over() {
  local hz a b
  hz="$(getconf CLK_TCK)"
  a="$(awk '{print $14 + $15}' "/proc/$SC_PID/stat")"
  sleep "$1"
  b="$(awk '{print $14 + $15}' "/proc/$SC_PID/stat")"
  echo $(( (b - a) * 100 / (hz * $1) ))
}

# Interlaced input (1080i50, tff and bff): MXL grains are fields of 540 lines at 50 Hz. The
# scope analyses both fields of a frame, follows the field rate and shows NO SIGNAL when it stops.
for order in tff bff; do
  case "$order" in tff) FI="cccccccc-cccc-4ccc-8ccc-cccccccccccc" ;; bff) FI="dddddddd-dddd-4ddd-8ddd-dddddddddddd" ;; esac
  "$WRITER" --domain "$WORK/src" --flow "$FI" --pattern flat --level 512 --width 1920 --height 1080 --rate 25 --interlace "$order" \
    --label "Test 1080i $order" >"$WORK/writer-$order.log" 2>&1 &
  W2=$!
  sleep 0.5
  route "$FI"
  wait_state running
  IN="$(curl -sf "$API/api/v1/input")"
  echo "$IN" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["interlaced"] is True and d["rate"] == "50" and d["height"] == 1080, d' \
    || fail "interlaced input not seen as 1080i50 ($order): $IN"
  sleep 0.5
  PIXI="$("$SAMPLE" --domain "$WORK/scopes" --flow "$FLOW_OUT" --width 640 --at "$COL,$ROW")"
  echo "interlaced $order trace=$PIXI"
  (( PIXI > 400 )) || fail "no trace for the interlaced input ($order)"
  if [[ "$order" == "tff" ]]; then
    # `frame` on 1080i50: one update per frame (25/s), each field analysed once.
    curl -sf -X PATCH -H 'Content-Type: application/json' -d '{"update_rate":"frame"}' "$API/api/v1/scope" >/dev/null || fail "PATCH frame"
    sleep 0.5
    U0="$(curl -sf "$API/api/v1/status" | json 'd["renderer"]["updates"]')"
    sleep 4
    U1="$(curl -sf "$API/api/v1/status" | json 'd["renderer"]["updates"]')"
    echo "interlaced frame mode: $((U1 - U0)) updates in 4 s"
    (( U1 - U0 >= 85 && U1 - U0 <= 115 )) || fail "interlaced frame mode made $((U1 - U0)) updates in 4 s, expected about 100"
    curl -sf -X PATCH -H 'Content-Type: application/json' -d '{"update_rate":10}' "$API/api/v1/scope" >/dev/null
  fi
  [[ "$(curl -sf "$API/api/v1/output" | json 'd["flow_id"]')" == "$FLOW_OUT" ]] || fail "1080i50 (50 fields) changed the 50p output flow"
  kill "$W2"; wait "$W2" 2>/dev/null || true; W2=
  wait_state no_signal
done
echo "interlaced ok"

# A writer that commits only invalid grains, and one that commits each grain in two halves:
# the reader waits for the next grain instead of spinning on the head.
"$WRITER" --domain "$WORK/src" --flow "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee" --pattern flat --width 320 --height 180 --rate 50 --invalid 1 \
  >"$WORK/writer-invalid.log" 2>&1 &
W2=$!
sleep 0.5
route "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"
sleep 1
CPU="$(cpu_over 3)"
echo "invalid grains: state $(input_state), cpu ${CPU} %"
(( CPU < 40 )) || fail "the renderer spins on invalid grains (${CPU} % of a core)"
kill "$W2"; wait "$W2" 2>/dev/null || true; W2=
"$WRITER" --domain "$WORK/src" --flow "ffffffff-ffff-4fff-8fff-ffffffffffff" --pattern flat --level 512 --width 320 --height 180 --rate 50 --partial 1 \
  >"$WORK/writer-partial.log" 2>&1 &
W2=$!
sleep 0.5
route "ffffffff-ffff-4fff-8fff-ffffffffffff"
wait_state running
CPU="$(cpu_over 3)"
echo "partial grains: cpu ${CPU} %"
(( CPU < 40 )) || fail "the renderer spins on partial grains (${CPU} % of a core)"
kill "$W2"; wait "$W2" 2>/dev/null || true; W2=

kill -TERM "$SC_PID"
set +e
wait "$SC_PID"
CODE=$?
set -e
SC_PID=
[[ "$CODE" == "143" ]] || fail "expected exit 143 on SIGTERM, got $CODE"
[[ ! -e "$WORK/scopes" ]] || fail "the output domain was not removed"
QUERY="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$((REG_PORT + 1))/x-nmos/query/v1.3/nodes/${NODE}")"
[[ "$QUERY" == "404" ]] || fail "the node is still registered ($QUERY)"
echo "integration ok"
