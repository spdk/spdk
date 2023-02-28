#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#

# Default set of apps used in functional testing

_root=$(readlink -f "$(dirname "${BASH_SOURCE[0]}")")
_root=${_root%/test/common}
_app_dir=$_root/build/bin
_test_app_dir=$_root/test/app
_examples_dir=$_root/build/examples

VHOST_FUZZ_APP=("$_test_app_dir/fuzz/vhost_fuzz/vhost_fuzz")
ISCSI_APP=("$_app_dir/iscsi_tgt")
NVMF_APP=("$_app_dir/nvmf_tgt")
VHOST_APP=("$_app_dir/vhost")
DD_APP=("$_app_dir/spdk_dd")
SPDK_APP=("$_app_dir/spdk_tgt")

# Check if apps should execute under debug flags
if [[ -e $_root/include/spdk/config.h ]]; then
	if [[ $(< "$_root/include/spdk/config.h") == *"#define SPDK_CONFIG_DEBUG"* ]] \
		&& ((SPDK_AUTOTEST_DEBUG_APPS)); then
		VHOST_FUZZ_APP+=("--logflag=all")
		ISCSI_APP+=("--logflag=all")
		NVMF_APP+=("--logflag=all")
		VHOST_APP+=("--logflag=all")
		DD_APP+=("--logflag=all")
		SPDK_APP+=("--logflag=all")
	fi
fi
