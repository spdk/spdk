#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
cat << EOF
Description: SPDK $3 library
Name: spdk_$3
Version: $4
Libs: -L${2:-"$1/lib"}  -lspdk_$3
Requires: $5
Libs.private: $6
Cflags: -I$1/include
EOF
