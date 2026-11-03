#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Sandisk and/or its affiliates.
#  All rights reserved.

set -euo pipefail

SIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="$SIT_DIR/sit_runner"
DO_BUILD=1
# Auto-disable build when running from an installed location (no Makefile present)
[[ -f "$SIT_DIR/Makefile" ]] || DO_BUILD=0

usage() {
	echo "Usage: $0 [--no-build] [sit_runner args]"
	echo ""
	echo "Examples:"
	echo "  $0 --bdf=0000:01:00.0"
	echo "  $0 --no-build --bdf=0000:01:00.0"
	echo ""
	echo "Notes:"
	echo "  - Runs every registered SIT test one-by-one."
	echo "  - Stops immediately on the first test failure; exit 77 means skipped."
	echo "  - Any extra args are passed to sit_runner for each test run."
}

RUNNER_ARGS=()
while [[ $# -gt 0 ]]; do
	case "$1" in
		--no-build)
			DO_BUILD=0
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			RUNNER_ARGS+=("$1")
			shift
			;;
	esac
done

if [[ $DO_BUILD -eq 1 ]]; then
	echo "[SIT] Building sit_runner..."
	make -C "$SIT_DIR" -j "$(nproc)" sit_runner
fi

if [[ ! -x "$RUNNER" ]]; then
	echo "[SIT] ERROR: sit_runner not found at: $RUNNER" >&2
	exit 1
fi

mapfile -t TESTS < <(
	"$RUNNER" -l | awk '
		/^[[:space:]]*[0-9]+\./ {
			sub(/^[[:space:]]*[0-9]+\.[[:space:]]*/, "", $0);
			print $0;
		}
	'
)

if [[ ${#TESTS[@]} -eq 0 ]]; then
	echo "[SIT] ERROR: no tests discovered from sit_runner -l" >&2
	exit 1
fi

echo "[SIT] Found ${#TESTS[@]} tests"

skipped=0
for test_name in "${TESTS[@]}"; do
	echo "--------------------------------------------------"
	echo "[SIT] Running: $test_name"

	if "$RUNNER" "${RUNNER_ARGS[@]}" --test="$test_name"; then
		echo "[SIT] PASS: $test_name"
	else
		rc=$?
		if [[ $rc -ne 77 ]]; then
			exit "$rc"
		fi
		echo "[SIT] SKIP: $test_name"
		skipped=$((skipped + 1))
	fi
done

echo "--------------------------------------------------"
echo "[SIT] Completed: ${#TESTS[@]} tests, $skipped skipped"
if [[ $skipped -gt 0 ]]; then
	exit 77
fi
