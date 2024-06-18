#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

bdevperf=$rootdir/build/examples/bdevperf

source "$curdir/mallocs.conf"
$bdevperf --json <(gen_malloc_ocf_json) -q 128 -o 4096 -t 4 -w flush
$bdevperf --json <(gen_malloc_ocf_json) -q 128 -o 4096 -t 4 -w unmap
$bdevperf --json <(gen_malloc_ocf_json) -q 128 -o 4096 -t 4 -w write
