#!/usr/bin/env bash
# SPDK Hot Upgrade Execution Script (Parallel Flow)
#
# Parallel flow: Secondary starts BEFORE Primary exits, minimizing IO interruption.
#   1. Read Primary params from /proc/<pid>/cmdline
#   2. Start Secondary (Primary still RUNNING, IO uninterrupted)
#   3. Wait for Secondary SECONDARY_PRE_INIT_DONE
#   4. Trigger primary_exit (IO interruption STARTS)
#   5. Trigger secondary_init (takeover, IO interruption continues)
#   6. Verify Secondary COMPLETE (IO interruption ENDS)
#
# Usage: spdk_hot_upgrade_do.sh [options]
#   -p, --primary-socket PATH    Primary RPC socket
#   -s, --secondary-socket PATH  Secondary RPC socket
#   -n, --new-binary PATH        New SPDK binary path
#   -t, --timeout SECONDS        Timeout for each step (default: 30)
#   -v, --verbose                Verbose output
#   -h, --help                   Show this help

set -euo pipefail

# ======================== Defaults ========================
PRIMARY_SOCK="/var/tmp/spdk_hot_upgrade.sock"
SECONDARY_SOCK="/var/tmp/spdk_secondary.sock"
SECONDARY_LOG="/var/tmp/spdk_secondary_hu.log"
NEW_BINARY="./build/bin/spdk_tgt"
TIMEOUT=30
VERBOSE=0
SECONDARY_PID=""
CURRENT_STEP=""

# Primary params (read from /proc/<pid>/cmdline)
SHM_ID=""
BASE_VIRTADDR=""
CORE_MASK=""
PRIMARY_PID=""

# IO interruption measurement
T_IO_START=""
T_IO_END=""

# ======================== Argument Parsing ========================
usage() {
    cat <<EOF
spdk_hot_upgrade_do.sh - Execute SPDK hot upgrade (parallel flow)

Usage: $0 [options]

Options:
  -p, --primary-socket PATH    Primary RPC socket (default: $PRIMARY_SOCK)
  -s, --secondary-socket PATH  Secondary RPC socket (default: $SECONDARY_SOCK)
  -n, --new-binary PATH        New SPDK binary path (default: $NEW_BINARY)
  -t, --timeout SECONDS        Timeout for each step (default: $TIMEOUT)
  -v, --verbose                Verbose output
  -h, --help                   Show this help

Exit codes:
  0   Hot upgrade completed successfully
  1   Parameter resolution failed
  2   Primary exit failed
  3   Secondary startup failed
  4   Secondary takeover failed
  5   Timeout
  6   Rollback was triggered
EOF
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--primary-socket)   PRIMARY_SOCK="$2"; shift 2;;
        -s|--secondary-socket) SECONDARY_SOCK="$2"; shift 2;;
        -n|--new-binary)       NEW_BINARY="$2"; shift 2;;
        -t|--timeout)          TIMEOUT="$2"; shift 2;;
        -v|--verbose)          VERBOSE=1; shift;;
        -h|--help)             usage 0;;
        *)                     echo "Unknown option: $1"; usage 1;;
    esac
done

# ======================== RPC Helper ========================
_rpc_call() {
    local sock="$1"
    local method="$2"
    local timeout="${3:-5}"
    python3 -c "
import socket,json,sys,time
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
s.settimeout($timeout)
try:
    s.connect('$sock')
    s.sendall(json.dumps({'jsonrpc':'2.0','method':'$method','id':1}).encode())
    resp=json.loads(s.recv(4096))
    if 'result' in resp:
        print(json.dumps(resp['result']))
    elif 'error' in resp:
        print('ERROR:'+json.dumps(resp['error']),file=sys.stderr)
        sys.exit(1)
except socket.timeout:
    print('TIMEOUT',file=sys.stderr)
    sys.exit(124)
except ConnectionRefusedError:
    print('CONNECTION_REFUSED',file=sys.stderr)
    sys.exit(111)
except Exception as e:
    print('RPC_ERROR:'+str(e),file=sys.stderr)
    sys.exit(2)
" 2>/dev/null
}

_rpc_get_state() {
    local sock="$1"
    local state
    state=$(_rpc_call "$sock" "hot_upgrade_status" 3 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin)['state'])" 2>/dev/null || echo "UNKNOWN")
    echo "$state"
}

# ======================== Auto-Rollback ========================
hu_rollback() {
    local exit_code=$?
    local step="${CURRENT_STEP:-unknown}"
    echo ""
    echo "[ROLLBACK] Step '$step' failed (exit_code=$exit_code), rolling back..."

    # Kill Secondary if running
    if [[ -n "${SECONDARY_PID:-}" ]] && kill -0 "$SECONDARY_PID" 2>/dev/null; then
        echo "[ROLLBACK] Terminating Secondary (PID=$SECONDARY_PID)..."
        kill "$SECONDARY_PID" 2>/dev/null || true
        wait "$SECONDARY_PID" 2>/dev/null || true
    fi

    # Try to resume Primary
    local primary_state
    primary_state=$(_rpc_get_state "$PRIMARY_SOCK" 2>/dev/null || echo "UNKNOWN")
    echo "[ROLLBACK] Primary state: $primary_state"

    if [[ "$primary_state" == "PRIMARY_SUSPENDED" || "$primary_state" == "PRIMARY_DRAINING" ]]; then
        echo "[ROLLBACK] Attempting primary_resume..."
        _rpc_call "$PRIMARY_SOCK" "primary_resume" 10 >/dev/null 2>&1 || true
        sleep 1
        primary_state=$(_rpc_get_state "$PRIMARY_SOCK" 2>/dev/null || echo "UNKNOWN")
        echo "[ROLLBACK] Primary state after resume: $primary_state"
    fi

    # Print IO interruption if measured
    if [[ -n "$T_IO_START" ]]; then
        T_IO_END=$(date +%s.%N)
        local io_dur=$(echo "$T_IO_END - $T_IO_START" | bc 2>/dev/null || echo "?")
        echo "[METRIC] IO interruption (before failure): ${io_dur}s"
    fi

    echo "[ROLLBACK] Rollback complete. Manual intervention may be needed."
    exit 6
}
trap hu_rollback ERR

# ======================== Step Functions ========================

step1_read_primary_params() {
    CURRENT_STEP="read_primary_params"
    echo "=== Step 1: Read Primary parameters ==="

    # Find Primary PID from socket
    PRIMARY_PID=$(lsof -t "$PRIMARY_SOCK" 2>/dev/null | head -1 || true)
    if [[ -z "$PRIMARY_PID" ]]; then
        # Fallback: find by binary name
        PRIMARY_PID=$(pgrep -f "spdk_tgt.*$(basename "$PRIMARY_SOCK")" 2>/dev/null | head -1 || true)
    fi
    if [[ -z "$PRIMARY_PID" ]]; then
        echo "  ERROR: Cannot find Primary PID for socket $PRIMARY_SOCK"
        return 1
    fi
    echo "  Primary PID: $PRIMARY_PID"

    # Parse /proc/<pid>/cmdline (NUL-separated)
    local cmdline
    cmdline=$(tr '\0' '\n' < "/proc/$PRIMARY_PID/cmdline" 2>/dev/null || true)
    if [[ -z "$cmdline" ]]; then
        echo "  ERROR: Cannot read /proc/$PRIMARY_PID/cmdline"
        return 1
    fi

    # Extract --shm-id / -i
    SHM_ID=$(echo "$cmdline" | grep -A1 '^\(--shm-id\|-i\)$' | tail -1 || true)
    if [[ -z "$SHM_ID" ]]; then
        # Try --shm-id=VALUE format
        SHM_ID=$(echo "$cmdline" | grep -oP '^\-\-shm-id=\K\d+' || true)
    fi
    if [[ -z "$SHM_ID" ]]; then
        echo "  WARNING: --shm-id not found in Primary cmdline, using default 100"
        SHM_ID="100"
    fi
    echo "  shm_id:        $SHM_ID"

    # Extract --base-virtaddr
    BASE_VIRTADDR=$(echo "$cmdline" | grep -A1 '^--base-virtaddr$' | tail -1 || true)
    if [[ -z "$BASE_VIRTADDR" ]]; then
        BASE_VIRTADDR=$(echo "$cmdline" | grep -oP '^\-\-base-virtaddr=\K0x[0-9a-fA-F]+' || true)
    fi
    if [[ -n "$BASE_VIRTADDR" ]]; then
        echo "  base_virtaddr: $BASE_VIRTADDR"
    else
        # SPDK/DPDK default when not specified in cmdline
        BASE_VIRTADDR="0x200000000000"
        echo "  base_virtaddr: $BASE_VIRTADDR (SPDK default)"
    fi

    # Extract -m / --cpumask
    CORE_MASK=$(echo "$cmdline" | grep -A1 '^\(-m\|--cpumask\)$' | tail -1 || true)
    if [[ -z "$CORE_MASK" ]]; then
        CORE_MASK=$(echo "$cmdline" | grep -oP '^\-\-cpumask=\K[0-9a-fA-F]+' || true)
    fi
    if [[ -n "$CORE_MASK" ]]; then
        echo "  core_mask:     $CORE_MASK"
    else
        # SPDK default core mask when not specified (single core 0)
        CORE_MASK="0x1"
        echo "  core_mask:     $CORE_MASK (SPDK default)"
    fi

    # Set LD_LIBRARY_PATH from Primary's environment
    local primary_env
    primary_env=$(tr '\0' '\n' < "/proc/$PRIMARY_PID/environ" 2>/dev/null | grep '^LD_LIBRARY_PATH=' | head -1 || true)
    if [[ -n "$primary_env" ]]; then
        export "${primary_env}"
        echo "  LD_LIBRARY_PATH inherited from Primary"
    fi

    return 0
}

step2_start_secondary() {
    CURRENT_STEP="start_secondary"
    echo "=== Step 2: Start Secondary (Primary still RUNNING) ==="
    echo "  Binary:  $NEW_BINARY"
    echo "  Socket:  $SECONDARY_SOCK"
    echo "  Log:     $SECONDARY_LOG"

    # Clean up stale secondary socket
    rm -f "$SECONDARY_SOCK"

    # Build Secondary command line
    local -a sec_args
    sec_args=(
        "$NEW_BINARY"
        --proc-type=secondary
        -i "$SHM_ID"
        -r "$SECONDARY_SOCK"
    )
    [[ -n "$BASE_VIRTADDR" ]] && sec_args+=(--base-virtaddr="$BASE_VIRTADDR")
    [[ -n "$CORE_MASK" ]] && sec_args+=(-m "$CORE_MASK")

    echo "  Cmd: ${sec_args[*]}"

    # Start Secondary in background
    set +e
    "${sec_args[@]}" > "$SECONDARY_LOG" 2>&1 &
    SECONDARY_PID=$!
    set -e

    echo "  Secondary PID: $SECONDARY_PID"

    # Verify Secondary is alive
    if ! kill -0 "$SECONDARY_PID" 2>/dev/null; then
        echo "  ERROR: Secondary process died immediately"
        echo "  Check log: $SECONDARY_LOG"
        tail -20 "$SECONDARY_LOG" 2>/dev/null || true
        return 3
    fi

    # Wait for Secondary RPC socket to appear
    echo -n "  Waiting for Secondary RPC socket... "
    local deadline
    deadline=$(($(date +%s) + TIMEOUT))
    while [[ ! -S "$SECONDARY_SOCK" ]]; do
        if ! kill -0 "$SECONDARY_PID" 2>/dev/null; then
            echo "DEAD"
            echo "  ERROR: Secondary process died during startup"
            tail -20 "$SECONDARY_LOG" 2>/dev/null || true
            return 3
        fi
        if [[ $(date +%s) -gt $deadline ]]; then
            echo "TIMEOUT"
            echo "  Secondary socket not created within ${TIMEOUT}s"
            tail -20 "$SECONDARY_LOG" 2>/dev/null || true
            return 5
        fi
        sleep 0.5
    done
    echo "OK"

    # Wait for Secondary RPC to respond
    echo -n "  Waiting for Secondary RPC response... "
    deadline=$(($(date +%s) + TIMEOUT))
    while true; do
        if _rpc_call "$SECONDARY_SOCK" "spdk_get_version" 3 >/dev/null 2>&1; then
            echo "OK"
            break
        fi
        if ! kill -0 "$SECONDARY_PID" 2>/dev/null; then
            echo "DEAD"
            echo "  ERROR: Secondary process died during RPC wait"
            tail -20 "$SECONDARY_LOG" 2>/dev/null || true
            return 3
        fi
        if [[ $(date +%s) -gt $deadline ]]; then
            echo "TIMEOUT"
            return 5
        fi
        sleep 1
    done

    echo "  Secondary started successfully (Primary still running)"
    return 0
}

step3_wait_pre_init() {
    CURRENT_STEP="wait_pre_init"
    echo "=== Step 3: Wait for Secondary pre-init ==="
    echo -n "  Secondary hot upgrade state: "

    local deadline
    deadline=$(($(date +%s) + TIMEOUT))

    while true; do
        local state
        state=$(_rpc_get_state "$SECONDARY_SOCK")

        case "$state" in
            SECONDARY_PRE_INIT_DONE)
                echo "$state (ready for takeover)"
                return 0
                ;;
            SECONDARY_PRE_INIT)
                echo -n "."
                ;;
            FAILED)
                echo "FAILED"
                echo "  Secondary pre-init failed. Check log: $SECONDARY_LOG"
                tail -20 "$SECONDARY_LOG" 2>/dev/null || true
                return 4
                ;;
            UNKNOWN|CONNECTION_REFUSED)
                echo "ERROR (state=$state)"
                return 4
                ;;
            *)
                echo -n "[$state]"
                ;;
        esac

        if [[ $(date +%s) -gt $deadline ]]; then
            echo "TIMEOUT"
            echo "  Secondary did not complete pre-init within ${TIMEOUT}s"
            return 5
        fi
        sleep 1
    done
}

step4_primary_exit() {
    CURRENT_STEP="primary_exit"
    echo "=== Step 4: Trigger primary_exit (IO interruption STARTS) ==="

    # Record IO interruption start time
    T_IO_START=$(date +%s.%N)

    local result
    if result=$(_rpc_call "$PRIMARY_SOCK" "primary_exit" "$TIMEOUT"); then
        echo "  Primary exit response: $result"
        echo "  Primary now suspended (IO drained)"
        return 0
    else
        local rc=$?
        echo "  ERROR: primary_exit failed with code $rc"
        return 2
    fi
}

step5_secondary_takeover() {
    CURRENT_STEP="secondary_takeover"
    echo "=== Step 5: Trigger secondary_init (takeover) ==="

    local result
    if result=$(_rpc_call "$SECONDARY_SOCK" "secondary_init" "$TIMEOUT"); then
        echo "  Secondary takeover response: $result"
        return 0
    else
        echo "  ERROR: secondary_init failed"
        return 4
    fi
}

step6_verify() {
    CURRENT_STEP="verify"
    echo "=== Step 6: Verify Secondary health ==="

    # Check state is COMPLETE
    local state
    state=$(_rpc_get_state "$SECONDARY_SOCK")
    echo "  Secondary hot upgrade state: $state"

    if [[ "$state" != "COMPLETE" ]]; then
        echo "  WARNING: Expected COMPLETE state, got $state"
    fi

    # Verify RPC is fully functional
    echo -n "  Secondary RPC health check... "
    if _rpc_call "$SECONDARY_SOCK" "spdk_get_version" 3 >/dev/null 2>&1; then
        echo "OK"
    else
        echo "FAILED"
        return 4
    fi

    # Verify bdev list is inherited
    echo -n "  Secondary bdev list check... "
    local bdevs
    bdevs=$(_rpc_call "$SECONDARY_SOCK" "bdev_get_bdevs" 5 2>/dev/null || echo "[]")
    local bdev_count
    bdev_count=$(echo "$bdevs" | python3 -c "import sys,json;print(len(json.load(sys.stdin)))" 2>/dev/null || echo "0")
    echo "$bdev_count bdev(s)"

    # Record IO interruption end time
    T_IO_END=$(date +%s.%N)

    # Print IO interruption metric
    if [[ -n "$T_IO_START" && -n "$T_IO_END" ]]; then
        local io_dur
        io_dur=$(echo "$T_IO_END - $T_IO_START" | bc 2>/dev/null || echo "?")
        echo ""
        echo "[METRIC] IO interruption: ${io_dur}s (target: <0.2s)"
    fi

    return 0
}

# ======================== Main ========================
main() {
    echo "=============================================="
    echo "  SPDK Hot Upgrade (Parallel Flow)"
    echo "=============================================="
    echo "  Primary sock:   $PRIMARY_SOCK"
    echo "  Secondary sock: $SECONDARY_SOCK"
    echo "  Binary:         $NEW_BINARY"
    echo "  Timeout:        ${TIMEOUT}s"
    echo "=============================================="
    echo ""

    step1_read_primary_params
    step2_start_secondary
    step3_wait_pre_init
    step4_primary_exit
    step5_secondary_takeover
    step6_verify

    echo ""
    echo "=============================================="
    echo "  Hot Upgrade Complete!"
    echo "  Secondary PID:   $SECONDARY_PID"
    echo "  Secondary RPC:   $SECONDARY_SOCK"
    echo "  Primary status:  SUSPENDED"
    echo ""
    echo "  To commit:   spdk_hot_upgrade_commit.sh"
    echo "  To rollback: spdk_hot_upgrade_rollback.sh"
    echo "=============================================="

    exit 0
}

main
