#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#

set -e

if [ $# -lt 2 ]; then
	echo "usage: $0 <pid> <script>"
	echo ""
	echo "Environment variable BPF_OUTFILE can be set to save results to a file"
	echo "rather than print to stdout."
	echo "If USE_CMDLINE_BPF_PROGRAM is provided via environment <script> is treated as a"
	echo "string holding already generated BPF program."
	exit 1
fi
SCRIPTS_DIR=$(readlink -f $(dirname $0))
BIN_PATH=$(readlink -f /proc/$1/exe)

BPF_PROGRAM=$2
if [[ -z $USE_CMDLINE_BPF_PROGRAM ]]; then
	BPF_PROGRAM=$("$SCRIPTS_DIR/gen_program.sh" "$@")
fi

[[ -n $BPF_PROGRAM ]]
[[ -n $ECHO_PROGRAM ]] && echo "$BPF_PROGRAM"

bpftrace -p $1 -e "$BPF_PROGRAM" ${BPF_OUTFILE:+-o "$BPF_OUTFILE"}
