#!/usr/bin/env bash
# AMWA nmos-testing against mxl-scopes: IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01
# (SPECIFICATION §14, IMPLEMENTATION_PLAN.md M4). Adapted from mxl-audio-mixer ci/amwa.sh (MIT,
# same author); every suite gets a fresh node with its own MXL root and state directory.
#
#   tests/nmos/amwa.sh off <mxl-scopes-binary>   # DNS-SD off (the platform default), nmos-cpp registry
#
# With AMWA_REGISTRY_EXTERNAL=1 the registry is not started here (one already runs on
# AMWA_REGISTRY_PORT on HOST_IP, e.g. started from outside a container).
# Needs: docker (mode off), python3, an nmos-testing checkout with its requirements in
# $NMOS_TESTING_DIR (default /tmp/nmos-testing) and $NMOS_TESTING_PYTHON (default
# /tmp/nmos-venv/bin/python), and the kernel TAI offset set (CLOCK_TAI = UTC + 37 s).
# A fresh node runs for each suite. Results go to $AMWA_OUT (default ci-artifacts/amwa-<mode>);
# tests/nmos/check_amwa.py then fails on any result not listed in tests/nmos/amwa-expected-<mode>.json.
set -euo pipefail

MODE="${1:?usage: amwa.sh off|on <node-binary>}"
NODE_BIN="${2:?usage: amwa.sh off|on <node-binary>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOL="${NMOS_TESTING_DIR:-/tmp/nmos-testing}"
PY="${NMOS_TESTING_PYTHON:-/tmp/nmos-venv/bin/python}"
OUT="${AMWA_OUT:-$HERE/../ci-artifacts/amwa-$MODE}"
REGISTRY_IMAGE="${NMOS_REGISTRY_IMAGE:-mirror.gcr.io/rhastie/nmos-cpp@sha256:c54c8ded3b61740dbd579c03892b0229842250417aa397d87a92e2b91ba645b8}"
# The node's IS-04/IS-05 port and its web port (another node on the host may
# have the defaults).
NODE_PORT="${AMWA_NODE_PORT:-3352}"
WEB_PORT="${AMWA_WEB_PORT:-8195}"
# The registry's ten ports from AMWA_REGISTRY_PORT on (registration, query, its
# WebSocket, …): another one on these ports needs another base.
REG_PORT="${AMWA_REGISTRY_PORT:-8010}"
QUERY_PORT=$((REG_PORT + 1))
ROOTS=()
HOST_IP="${HOST_IP:-$(ip -4 route get 1.1.1.1 | sed -n 's/.* src \([0-9.]*\).*/\1/p')}"
mkdir -p "$OUT"
NODE_PID=""

log() { echo "[amwa-$MODE] $*"; }

wait_http() { # <url> <seconds>
    for _ in $(seq 1 $(($2 * 5))); do
        curl -fsS -o /dev/null "$1" 2>/dev/null && return 0
        sleep 0.2
    done
    log "timeout waiting for $1"
    return 1
}

stop_node() {
    if [[ -n "$NODE_PID" ]] && kill -0 "$NODE_PID" 2>/dev/null; then
        kill -TERM "$NODE_PID"
        wait "$NODE_PID" || true
    fi
    NODE_PID=""
}

cleanup() {
    stop_node
    if [[ "$MODE" == off && -z "${AMWA_REGISTRY_EXTERNAL:-}" ]]; then
        docker logs mxl-scopes-amwa-registry >"$OUT/registry.log" 2>&1 || true
        docker rm -f mxl-scopes-amwa-registry >/dev/null 2>&1 || true
    fi
    for r in "${ROOTS[@]}"; do rm -rf "$r"; done
}
trap cleanup EXIT

start_node() { # <suite>
    local root
    root=$(mktemp -d /dev/shm/scopes-amwa.XXXXXX)
    ROOTS+=("$root")
    mkdir -p "$root/mxl" "$root/state"
    local env=(NMOS_SEED=amwa-ci NMOS_HOST_ADDRESS="$HOST_IP" NMOS_PORT="$NODE_PORT" WEB_PORT="$WEB_PORT" HOST_ID=amwa
        MXL_DOMAIN_SCAN_PATH="$root/mxl" MXL_OUTPUT_DOMAIN_DIR="$root/mxl/scopes" SCOPES_STATE_DIR="$root/state" MXL_CLEANUP_ON_EXIT=true
        SCOPES_PREVIEW_MODE=off SCOPES_OUTPUT_FORMAT=640x360)
    if [[ "$MODE" == off ]]; then
        env+=(NMOS_REGISTRY_ADDRESS="$HOST_IP" NMOS_REGISTRY_PORT="$REG_PORT")
    else
        env+=(NMOS_DNS_SD=true)
    fi
    env "${env[@]}" "$NODE_BIN" >"$OUT/node-$1.log" 2>&1 &
    NODE_PID=$!
    wait_http "http://$HOST_IP:$NODE_PORT/x-nmos/node/v1.3/self" 30
    # The sender has its output flow once the writer created it.
    wait_http "http://127.0.0.1:$WEB_PORT/readyz" 30
}

for p in "$NODE_PORT" "$WEB_PORT"; do
    if (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null; then
        log "port $p is in use (another node?): set AMWA_NODE_PORT and AMWA_WEB_PORT to free ports"
        exit 1
    fi
done

if [[ "$MODE" == off && -z "${AMWA_REGISTRY_EXTERNAL:-}" ]]; then
    docker rm -f mxl-scopes-amwa-registry >/dev/null 2>&1 || true
    for p in $(seq "$REG_PORT" $((REG_PORT + 9))); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null; then
            log "port $p is in use (another registry?): set AMWA_REGISTRY_PORT to the first of ten free ports"
            exit 1
        fi
    done
    python3 - "$HERE/registry.json" "$REG_PORT" >"$OUT/registry.json" <<'EOF'
import json, sys
c = json.load(open(sys.argv[1]))
base = int(sys.argv[2])
for k, name in enumerate(["registration_port", "query_port", "query_ws_port", "node_port", "system_port",
                          "settings_port", "logging_port", "admin_port", "mdns_port", "schemas_port"]):
    c[name] = base + k
print(json.dumps(c, indent=2))
EOF
    docker run -d --name mxl-scopes-amwa-registry --network host \
        -v "$OUT/registry.json:/home/registry.json:ro" "$REGISTRY_IMAGE" >/dev/null
fi
if [[ "$MODE" == off ]]; then
    wait_http "http://$HOST_IP:$QUERY_PORT/x-nmos/query/v1.3/nodes" 60
    cat >"$TOOL/nmostesting/UserConfig.py" <<EOF
from . import Config as CONFIG
CONFIG.ENABLE_DNS_SD = False
CONFIG.QUERY_API_HOST = "$HOST_IP"
CONFIG.QUERY_API_PORT = $QUERY_PORT
EOF
else
    cat >"$TOOL/nmostesting/UserConfig.py" <<EOF
from . import Config as CONFIG
CONFIG.ENABLE_DNS_SD = True
CONFIG.DNS_SD_MODE = "multicast"
CONFIG.DNS_SD_ADVERT_TIMEOUT = 60
EOF
fi

declare -A ARGS=(
    [IS-04-01]="--host $HOST_IP --port $NODE_PORT --version v1.3"
    [IS-05-01]="--host $HOST_IP --port $NODE_PORT --version v1.2"
    [IS-05-02]="--host $HOST_IP $HOST_IP --port $NODE_PORT $NODE_PORT --version v1.3 v1.2"
    [BCP-007-03-01]="--host $HOST_IP $HOST_IP --port $NODE_PORT $NODE_PORT --version v1.3 v1.2"
)
for suite in IS-04-01 IS-05-01 IS-05-02 BCP-007-03-01; do
    start_node "$suite"
    log "running $suite"
    set +e
    # shellcheck disable=SC2086
    (cd "$TOOL" && "$PY" nmos-test.py suite "$suite" --selection all ${ARGS[$suite]} \
        --output "$OUT/$suite.json") >"$OUT/$suite.log" 2>&1
    rc=$?
    set -e
    log "$suite exited $rc"
    stop_node
done

if [[ "$MODE" == off ]]; then
    # The last node stopped with SIGTERM: it and its devices must be gone from the registry.
    # UUIDv5 in the URL namespace of mxl-scopes/<seed>/node (SPECIFICATION §7.1).
    node_id=$(python3 -c "import uuid; print(uuid.uuid5(uuid.NAMESPACE_URL, 'mxl-scopes/amwa-ci/node'))")
    code=$(curl -s -o /dev/null -w '%{http_code}' "http://$HOST_IP:$QUERY_PORT/x-nmos/query/v1.3/nodes/$node_id")
    devices=$(curl -fsS "http://$HOST_IP:$QUERY_PORT/x-nmos/query/v1.3/devices" |
        python3 -c "import json,sys; print(sum(d['node_id'] == sys.argv[1] for d in json.load(sys.stdin)))" "$node_id")
    log "after SIGTERM: Query API node $node_id -> HTTP $code, devices left: $devices"
    [[ "$code" == 404 && "$devices" == 0 ]] || { log "FAIL: the node did not deregister"; exit 1; }
fi

python3 "$HERE/check_amwa.py" "$OUT" "$HERE/amwa-expected-$MODE.json"
