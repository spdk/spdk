#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#

cat << EOF
Description: $3 libraries used by SPDK
Name: $4
Version: 1.0
Libs: $1
Libs.private: $2
EOF
