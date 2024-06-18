#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation
#  All rights reserved.
#

testdir="$(readlink -f $(dirname $0))"
rootdir="$(readlink -f $testdir/../../..)"

source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"
rpc_py="$rootdir/scripts/rpc.py"

cleanup() {
	process_shm --id $NVMF_APP_SHM_ID || true
	killprocess $bdevperf_pid
	nvmftestfini || true
	rm -f $key_path
}

setup_nvmf_tgt_conf() {
	local key=$1

	$rpc_py <<- EOF
		nvmf_create_transport $NVMF_TRANSPORT_OPTS
		nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -s SPDK00000000000001 -m 10
		nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT \
		-a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -k
		bdev_malloc_create 32 4096 -b malloc0
		nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 malloc0 -n 1
		nvmf_subsystem_add_host nqn.2016-06.io.spdk:cnode1 nqn.2016-06.io.spdk:host1 \
		--psk $key
	EOF
}

build_openssl_config() {
	cat <<- NO_DEFAULT
		openssl_conf = openssl_spdk

		[openssl_spdk]
		providers = provider_sect_spdk
		alg_section = algorithm_sect_spdk

		[provider_sect_spdk]
		fips = fips_sect_spdk
		base = base_sect_spdk

		[base_sect_spdk]
		activate = 1

		[fips_sect_spdk]
		activate = 1

		[algorithm_sect_spdk]
		default_properties = fips=yes
	NO_DEFAULT
	if [[ ! -t 0 ]]; then
		cat -
	fi
}

build_openssl_config_fallback() {
	build_openssl_config <<- FIPS
		$(openssl fipsinstall -module "$(openssl info -modulesdir)/fips.so" 2>/dev/null)

		[openssl_spdk]
		providers = provider_sect_spdk
		alg_section = algorithm_sect_spdk

		[provider_sect_spdk]
		fips = fips_sect
		base = base_sect_spdk

		[base_sect_spdk]
		activate = 1

		[algorithm_sect_spdk]
		default_properties = fips=yes
	FIPS
}

check_openssl_version() {
	local target=${1:-3.0.0}

	ge "$(openssl version | awk '{print $2}')" "$target"
}

# Ensure environment is prepared for running this test.
if ! check_openssl_version; then
	echo "Unsupported OpenSSL version"
	exit 1
fi

# Absence of this library means that OpenSSL was configured and built without FIPS support.
if [[ ! -f "$(openssl info -modulesdir)/fips.so" ]]; then
	echo "FIPS library not found"
	exit 1
fi

if ! warn=$(openssl fipsinstall -help 2>&1); then
	if [[ $warn == "This command is not enabled"* ]]; then
		# Rhel-based openssl >=3.0.9 builds no longer support fipsinstall command.
		# Enforce proper patches.
		export callback=build_openssl_config
	else
		exit 1
	fi
else
	# We need to explicitly enable FIPS via proper config.
	export callback=build_openssl_config_fallback
fi

"$callback" > spdk_fips.conf
export OPENSSL_CONF=spdk_fips.conf

mapfile -t providers < <(openssl list -providers | grep "name")
# We expect OpenSSL to present the providers we requested. If OpenSSL loaded other providers
# (e.g. "default") or was unable to load "base" and "fips", the following line will fail,
# indicating that OPENSSL_CONF is invalid or OpenSSL itself is malconfigured.
if ((${#providers[@]} != 2)) || [[ ${providers[0],,} != *base* || ${providers[1],,} != *fips* ]]; then
	printf 'We expected Base and FIPS providers, got:\n'
	printf '  %s\n' "${providers[@]:-no providers}"
	exit 1
fi

# MD5 is not FIPS compliant, so below command should fail in FIPS-only environment.
NOT openssl md5 <(:)

# Start NVMf TLS test.
nvmftestinit
nvmfappstart -m 0x2

trap 'cleanup' EXIT

# Key taken from NVM Express TCP Transport Specification 1.0c.
key="NVMeTLSkey-1:01:VRLbtnN9AQb2WXW3c9+wEf/DRLz0QuLdbYvEhwtdWwNf9LrZ:"
key_path="$testdir/key.txt"
echo -n "$key" > $key_path
chmod 0600 $key_path

setup_nvmf_tgt_conf $key_path

# Use bdevperf as initiator.
bdevperf_rpc_sock="/var/tmp/bdevperf.sock"
"$rootdir/build/examples/bdevperf" -m 0x4 -z -r $bdevperf_rpc_sock \
	-q 128 -o 4096 -w verify -t 10 &
bdevperf_pid=$!
waitforlisten $bdevperf_pid $bdevperf_rpc_sock

$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b TLSTEST -t $TEST_TRANSPORT \
	-a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1 \
	-q nqn.2016-06.io.spdk:host1 --psk "$key_path"

"$rootdir/examples/bdev/bdevperf/bdevperf.py" -s $bdevperf_rpc_sock perform_tests
