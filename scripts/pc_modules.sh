#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
cat << EOF
Description: SPDK $1 modules
Name: spdk_$1_modules
Version: 1.0
Requires: $2
EOF
