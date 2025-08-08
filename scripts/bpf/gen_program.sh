#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2025 Nutanix Inc.
#  All rights reserved.
#
set -e
curdir=$(readlink -f "$(dirname "$0")")

(($# > 1))
"$curdir/gen.py" -p "$@"
"$curdir/gen_enums.sh"
