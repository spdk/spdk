#!/usr/bin/env bash
# SPDK Hot Upgrade - Single Entry Point
# Usage: spdk_hot_upgrade.sh <artifact> [options]
#
# Artifact can be:
#   - tar.gz/.tgz package: extracted to /var/tmp/spdk_hu_new/, spdk_tgt located inside
#   - directory: searched for build/bin/spdk_tgt or bin/spdk_tgt
#   - executable file: used directly
#
# The script orchestrates: precheck → do (parallel) → commit
# On any failure, rollback is triggered automatically.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ======================== Defaults ========================
PRIMARY_SOCK="/var/tmp/spdk_hot_upgrade.sock"
SECONDARY_SOCK="/var/tmp/spdk_secondary.sock"
TIMEOUT=30
SKIP_PRECHECK=0
SKIP_COMMIT=0
DRY_RUN=0
VERBOSE=0
ARTIFACT=""
NEW_BINARY=""
EXTRACT_DIR="/var/tmp/spdk_hu_new"

# ======================== Argument Parsing ========================
usage() {
    cat <<EOF
spdk_hot_upgrade.sh - Single-entry SPDK hot upgrade

Usage: $0 <artifact> [options]

Arguments:
  artifact                  Path to new SPDK artifact:
                            - .tar.gz/.tgz package (extracted)
                            - directory (searched for spdk_tgt)
                            - executable file (used directly)

Options:
  --primary-sock <path>     Primary RPC socket (default: $PRIMARY_SOCK)
  --secondary-sock <path>   Secondary RPC socket (default: $SECONDARY_SOCK)
  --timeout <sec>           Per-step timeout (default: $TIMEOUT)
  --skip-precheck           Skip precheck step
  --skip-commit             Skip commit step (leave Primary suspended)
  --dry-run                 Print steps without executing
  -v, --verbose             Verbose output
  -h, --help                Show this help

Exit codes:
  0   Hot upgrade completed successfully
  1   Artifact resolution or precheck failed
  2   Hot upgrade execution failed (rollback attempted)
  3   Commit failed
  6   Rollback was triggered
EOF
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --primary-sock)     PRIMARY_SOCK="$2"; shift 2;;
        --secondary-sock)   SECONDARY_SOCK="$2"; shift 2;;
        --timeout)          TIMEOUT="$2"; shift 2;;
        --skip-precheck)    SKIP_PRECHECK=1; shift;;
        --skip-commit)      SKIP_COMMIT=1; shift;;
        --dry-run)          DRY_RUN=1; shift;;
        -v|--verbose)       VERBOSE=1; shift;;
        -h|--help)          usage 0;;
        -*)
            echo "Unknown option: $1" >&2
            usage 1;;
        *)
            if [[ -z "$ARTIFACT" ]]; then
                ARTIFACT="$1"
            else
                echo "Unexpected argument: $1" >&2
                usage 1
            fi
            shift;;
    esac
done

if [[ -z "$ARTIFACT" ]]; then
    echo "ERROR: artifact is required" >&2
    usage 1
fi

# ======================== Artifact Resolution ========================
resolve_artifact() {
    local art="$1"
    local binary=""

    echo "[ARTIFACT] Resolving artifact: $art"

    if [[ -f "$art" ]]; then
        case "$art" in
            *.tar.gz|*.tgz)
                echo "[ARTIFACT] Type: tar.gz package"
                rm -rf "$EXTRACT_DIR"
                mkdir -p "$EXTRACT_DIR"
                tar xzf "$art" -C "$EXTRACT_DIR"
                # Search for spdk_tgt in common locations
                binary=$(find "$EXTRACT_DIR" -name "spdk_tgt" -type f -executable 2>/dev/null | head -1)
                if [[ -z "$binary" ]]; then
                    echo "[ARTIFACT] ERROR: spdk_tgt not found in package" >&2
                    return 1
                fi
                echo "[ARTIFACT] Found binary: $binary"
                ;;
            *)
                # Regular executable file
                if [[ -x "$art" ]]; then
                    binary="$art"
                    echo "[ARTIFACT] Type: executable file"
                else
                    echo "[ARTIFACT] ERROR: file exists but not executable: $art" >&2
                    return 1
                fi
                ;;
        esac
    elif [[ -d "$art" ]]; then
        echo "[ARTIFACT] Type: directory"
        # Search for spdk_tgt in common locations
        for candidate in \
            "$art/build/bin/spdk_tgt" \
            "$art/bin/spdk_tgt" \
            "$art/spdk_tgt"; do
            if [[ -x "$candidate" ]]; then
                binary="$candidate"
                break
            fi
        done
        if [[ -z "$binary" ]]; then
            binary=$(find "$art" -name "spdk_tgt" -type f -executable 2>/dev/null | head -1)
        fi
        if [[ -z "$binary" ]]; then
            echo "[ARTIFACT] ERROR: spdk_tgt not found in directory: $art" >&2
            return 1
        fi
        echo "[ARTIFACT] Found binary: $binary"
    else
        echo "[ARTIFACT] ERROR: artifact not found: $art" >&2
        return 1
    fi

    # Verify binary is executable
    if ! "$binary" --version >/dev/null 2>&1; then
        echo "[ARTIFACT] WARNING: binary --version check failed (may still work)"
    fi

    NEW_BINARY="$binary"
    echo "[ARTIFACT] Resolved binary: $NEW_BINARY"
    return 0
}

# ======================== Main Orchestration ========================
main() {
    echo "=============================================="
    echo "  SPDK Hot Upgrade - Single Entry Point"
    echo "=============================================="
    echo "  Artifact:       $ARTIFACT"
    echo "  Primary sock:   $PRIMARY_SOCK"
    echo "  Secondary sock: $SECONDARY_SOCK"
    echo "  Timeout:        ${TIMEOUT}s"
    echo "=============================================="
    echo ""

    # Step 0: Resolve artifact
    if ! resolve_artifact "$ARTIFACT"; then
        exit 1
    fi

    if [[ $DRY_RUN -eq 1 ]]; then
        echo "[DRY-RUN] Would execute:"
        echo "  1. precheck: $SCRIPT_DIR/spdk_hot_upgrade_precheck.sh -n $NEW_BINARY -p $PRIMARY_SOCK -s $SECONDARY_SOCK"
        echo "  2. do:       $SCRIPT_DIR/spdk_hot_upgrade_do.sh -n $NEW_BINARY -p $PRIMARY_SOCK -s $SECONDARY_SOCK -t $TIMEOUT"
        echo "  3. commit:   $SCRIPT_DIR/spdk_hot_upgrade_commit.sh -p $PRIMARY_SOCK -s $SECONDARY_SOCK"
        exit 0
    fi

    # Step 1: Precheck
    if [[ $SKIP_PRECHECK -eq 0 ]]; then
        echo "=== Step 1: Precheck ==="
        if ! "$SCRIPT_DIR/spdk_hot_upgrade_precheck.sh" \
                -n "$NEW_BINARY" \
                -p "$PRIMARY_SOCK" \
                -s "$SECONDARY_SOCK"; then
            echo "[FAILED] Precheck failed"
            exit 1
        fi
        echo ""
    else
        echo "[SKIP] Precheck skipped"
    fi

    # Step 2: Execute hot upgrade (parallel flow)
    echo "=== Step 2: Execute hot upgrade ==="
    local do_rc=0
    "$SCRIPT_DIR/spdk_hot_upgrade_do.sh" \
        -n "$NEW_BINARY" \
        -p "$PRIMARY_SOCK" \
        -s "$SECONDARY_SOCK" \
        -t "$TIMEOUT" \
        $([[ $VERBOSE -eq 1 ]] && echo "-v") || do_rc=$?

    if [[ $do_rc -ne 0 ]]; then
        echo "[FAILED] Hot upgrade execution failed (rc=$do_rc)"
        echo "=== Triggering automatic rollback ==="
        "$SCRIPT_DIR/spdk_hot_upgrade_rollback.sh" \
            -p "$PRIMARY_SOCK" \
            -s "$SECONDARY_SOCK" || true
        exit 6
    fi
    echo ""

    # Step 3: Commit
    if [[ $SKIP_COMMIT -eq 0 ]]; then
        echo "=== Step 3: Commit ==="
        local commit_rc=0
        "$SCRIPT_DIR/spdk_hot_upgrade_commit.sh" \
            -p "$PRIMARY_SOCK" \
            -s "$SECONDARY_SOCK" || commit_rc=$?

        if [[ $commit_rc -ne 0 ]]; then
            echo "[WARNING] Commit failed (rc=$commit_rc), Secondary should still be running"
            exit 3
        fi
    else
        echo "[SKIP] Commit skipped (Primary remains suspended)"
    fi

    echo ""
    echo "=============================================="
    echo "  Hot upgrade completed successfully!"
    echo "=============================================="
    exit 0
}

main "$@"
