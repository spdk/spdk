#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation
#  All rights reserved.
#

set -e

# If the configuration of tests is not provided, no tests will be carried out.
if [[ ! -f $1 ]]; then
	echo "ERROR: SPDK test configuration not specified"
	exit 1
fi

rootdir=$(readlink -f $(dirname $0))

source "$1"
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/scripts/common.sh"

if [[ -n $EXTERNAL_MAKE_HUGEMEM ]]; then
	export EXTERNAL_MAKE_HUGEMEM
fi

out=$output_dir
if [ -n "$SPDK_TEST_NATIVE_DPDK" ]; then
	scanbuild_exclude=" --exclude $(dirname $SPDK_RUN_EXTERNAL_DPDK)"
else
	scanbuild_exclude="--exclude $rootdir/dpdk/"
fi
# We exclude /tmp as it's used by xnvme's liburing subproject for storing
# temporary .c files which are picked up as buggy by the scanbuild.
scanbuild_exclude+=" --exclude $rootdir/xnvme --exclude /tmp"

scanbuild="scan-build -o $output_dir/scan-build-tmp $scanbuild_exclude --status-bugs"
config_params=$(get_config_params)

trap '[[ -d $SPDK_WORKSPACE ]] && rm -rf "$SPDK_WORKSPACE"' 0

SPDK_WORKSPACE=$(mktemp -dt "spdk_$(date +%s).XXXXXX")
export SPDK_WORKSPACE

umask 022
cd $rootdir

# Print some test system info out for the log
date -u
git describe --tags

function ocf_precompile() {
	# We compile OCF sources ourselves
	# They don't need to be checked with scanbuild and code coverage is not applicable
	# So we precompile OCF now for further use as standalone static library
	./configure $(echo $config_params | sed 's/--enable-coverage//g')
	$MAKE $MAKEFLAGS include/spdk/config.h
	CC=gcc CCAR=ar $MAKE $MAKEFLAGS -C lib/env_ocf exportlib O=$rootdir/ocf.a
	# Set config to use precompiled library
	config_params="$config_params --with-ocf=/$rootdir/ocf.a"
	# need to reconfigure to avoid clearing ocf related files on future make clean.
	./configure $config_params
}

# Find matching llvm fuzzer library and clang compiler version
function llvm_precompile() {
	[[ $(clang --version) =~ "version "(([0-9]+).([0-9]+).([0-9]+)) ]]
	clang_version=${BASH_REMATCH[1]}
	clang_num=${BASH_REMATCH[2]}

	export CC=clang-$clang_num
	export CXX=clang++-$clang_num

	fuzzer_libs=(/usr/lib*/clang/"$clang_version"/lib/linux/libclang_rt.fuzzer_no_main-x86_64.a)
	fuzzer_lib=${fuzzer_libs[0]}
	[[ -e $fuzzer_lib ]]

	config_params="$config_params --with-fuzzer=$fuzzer_lib"
	# Need to reconfigure to avoid clearing llvm related files on future make clean.
	./configure $config_params
}

function build_native_dpdk() {
	local external_dpdk_dir
	local external_dpdk_base_dir
	local compiler_version
	local compiler
	local dpdk_kmods

	compiler=${CC:-gcc}

	# Export CC to be absolutely sure it's set.
	# If CC was not set and we defaulted to "gcc" then we need to do the export
	# so that "meson build" command a few lines below is aware of which compiler
	# to use.
	export CC="$compiler"

	if [[ $compiler != *clang* && $compiler != *gcc* ]]; then
		echo "Unsupported compiler detected ($compiler), failing the test" >&2
		return 1
	fi

	compiler_version=$("$compiler" -dumpversion)
	compiler_version=${compiler_version%%.*}
	external_dpdk_dir="$SPDK_RUN_EXTERNAL_DPDK"
	external_dpdk_base_dir="$(dirname $external_dpdk_dir)"

	if [[ ! -d "$external_dpdk_base_dir" ]]; then
		sudo mkdir -p "$external_dpdk_base_dir"
		sudo chown -R $(whoami) "$external_dpdk_base_dir"/..
	fi
	orgdir=$PWD

	rm -rf "$external_dpdk_base_dir"
	git clone --branch $SPDK_TEST_NATIVE_DPDK --depth 1 http://dpdk.org/git/dpdk "$external_dpdk_base_dir"
	git -C "$external_dpdk_base_dir" log --oneline -n 5

	dpdk_cflags="-fPIC -g -fcommon"
	dpdk_ldflags=""
	dpdk_ver=$(< "$external_dpdk_base_dir/VERSION")

	if [[ $compiler == *gcc* && $compiler_version -ge 5 ]]; then
		dpdk_cflags+=" -Werror"
	fi

	if [[ $compiler == *gcc* && $compiler_version -ge 10 ]]; then
		dpdk_cflags+=" -Wno-stringop-overflow"
	fi

	# the drivers we use
	# net/i40e driver is not really needed by us, but it's built as a workaround
	# for DPDK issue: https://bugs.dpdk.org/show_bug.cgi?id=576
	DPDK_DRIVERS=("bus" "bus/pci" "bus/vdev" "mempool/ring" "net/i40e" "net/i40e/base")

	local mlx5_libs_added="n"
	if [[ "$SPDK_TEST_CRYPTO" -eq 1 || "$SPDK_TEST_SMA" -eq 1 ]]; then
		intel_ipsec_mb_ver=v0.54
		intel_ipsec_mb_drv=crypto/aesni_mb
		intel_ipsec_lib=""
		if ge "$dpdk_ver" 21.11.0; then
			# Minimum supported version of intel-ipsec-mb, for DPDK >= 21.11, is 1.0.
			# Source of the aesni_mb driver was moved to ipsec_mb. .{h,so,a} were moved
			# to ./lib.
			# https://github.com/dpdk/dpdk/commit/918fd2f1466b0e3b21a033df7012a77a83665582.
			intel_ipsec_mb_ver=v1.0
			intel_ipsec_mb_drv=crypto/ipsec_mb
			intel_ipsec_lib=lib
		fi
		git clone --branch "$intel_ipsec_mb_ver" --depth 1 https://github.com/intel/intel-ipsec-mb.git "$external_dpdk_base_dir/intel-ipsec-mb"
		cd "$external_dpdk_base_dir/intel-ipsec-mb"
		$MAKE $MAKEFLAGS all SHARED=y EXTRA_CFLAGS=-fPIC
		DPDK_DRIVERS+=("crypto")
		DPDK_DRIVERS+=("$intel_ipsec_mb_drv")
		DPDK_DRIVERS+=("crypto/qat")
		DPDK_DRIVERS+=("compress/qat")
		DPDK_DRIVERS+=("common/qat")
		# 21.11.0 is version of DPDK with stable support for mlx5 crypto.
		if ge "$dpdk_ver" 21.11.0; then
			# SPDK enables CRYPTO_MLX in case supported version of DPDK is detected
			# so make sure proper libs are built.
			DPDK_DRIVERS+=("bus/auxiliary")
			DPDK_DRIVERS+=("common/mlx5")
			DPDK_DRIVERS+=("common/mlx5/linux")
			DPDK_DRIVERS+=("crypto/mlx5")
			mlx5_libs_added="y"
		fi
		dpdk_cflags+=" -I$external_dpdk_base_dir/intel-ipsec-mb/$intel_ipsec_lib"
		dpdk_ldflags+=" -L$external_dpdk_base_dir/intel-ipsec-mb/$intel_ipsec_lib"
		export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$external_dpdk_base_dir/intel-ipsec-mb/$intel_ipsec_lib"
	fi

	if [[ "$SPDK_TEST_VBDEV_COMPRESS" -eq 1 ]]; then
		isal_dir="$external_dpdk_base_dir/isa-l"
		git clone --branch v2.29.0 --depth 1 https://github.com/intel/isa-l.git "$isal_dir"

		cd $isal_dir
		./autogen.sh
		./configure CFLAGS="-fPIC -g -O2" --enable-shared=yes --prefix="$isal_dir/build"
		ln -s $PWD/include $PWD/isa-l
		$MAKE $MAKEFLAGS all
		$MAKE install
		DPDK_DRIVERS+=("compress")
		DPDK_DRIVERS+=("compress/isal")
		DPDK_DRIVERS+=("compress/qat")
		DPDK_DRIVERS+=("common/qat")
		if ge "$dpdk_ver" 21.02.0; then
			# SPDK enables REDUCE_MLX in case supported version of DPDK is detected
			# so make sure proper libs are built.
			if test $mlx5_libs_added = "n"; then
				DPDK_DRIVERS+=("bus/auxiliary")
				DPDK_DRIVERS+=("common/mlx5")
				DPDK_DRIVERS+=("common/mlx5/linux")
			fi
			DPDK_DRIVERS+=("compress/mlx5")
		fi
		export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$isal_dir/build/lib/pkgconfig"
		export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$isal_dir/build/lib"
	fi

	cd $external_dpdk_base_dir
	if [ "$(uname -s)" = "Linux" ]; then
		if lt $dpdk_ver 21.11.0; then
			patch -p1 < "$rootdir/test/common/config/pkgdep/patches/dpdk/20.11/dpdk_pci.patch"
			patch -p1 < "$rootdir/test/common/config/pkgdep/patches/dpdk/20.11/dpdk_qat.patch"
		else
			patch -p1 < "$rootdir/test/common/config/pkgdep/patches/dpdk/21.11+/dpdk_qat.patch"

			if lt $dpdk_ver 23.03.0; then
				# Commit https://review.spdk.io/gerrit/c/spdk/dpdk/+/16828 is required for DPDK <23.03.0
				patch -p1 < "$rootdir/test/common/config/pkgdep/patches/dpdk/21.11+/dpdk_rte_thash_gfni.patch"
			fi

			# Commit https://review.spdk.io/gerrit/c/spdk/dpdk/+/16134 is required for DPDK 22.11+
			if ge $dpdk_ver 22.11.0 && lt $dpdk_ver 23.03.0; then
				patch -p1 < "$rootdir/test/common/config/pkgdep/patches/dpdk/22.11+/dpdk_ipsec_mb.patch"
			fi
		fi
	fi

	dpdk_kmods="false"
	if [ "$(uname -s)" = "FreeBSD" ]; then
		dpdk_kmods="true"
	fi

	meson build-tmp --prefix="$external_dpdk_dir" --libdir lib \
		-Denable_docs=false -Denable_kmods="$dpdk_kmods" -Dtests=false \
		-Dc_link_args="$dpdk_ldflags" -Dc_args="$dpdk_cflags" \
		-Dmachine=native -Denable_drivers=$(printf "%s," "${DPDK_DRIVERS[@]}")
	ninja -C "$external_dpdk_base_dir/build-tmp" $MAKEFLAGS
	ninja -C "$external_dpdk_base_dir/build-tmp" $MAKEFLAGS install

	# Save this path. In tests are run using autorun.sh then autotest.sh
	# script will be unaware of LD_LIBRARY_PATH and will fail tests.
	echo "export LD_LIBRARY_PATH=$LD_LIBRARY_PATH" > /tmp/spdk-ld-path

	cd "$orgdir"
}

function check_dpdk_pci_api() {
	local dpdk_dir

	if [[ -n "$SPDK_TEST_NATIVE_DPDK" ]]; then
		dpdk_dir=$(dirname "$SPDK_RUN_EXTERNAL_DPDK")
	fi

	"$rootdir/scripts/env_dpdk/check_dpdk_pci_api.sh" check "$dpdk_dir"
}

function make_fail_cleanup() {
	if [ -d $out/scan-build-tmp ]; then
		scanoutput=$(ls -1 $out/scan-build-tmp/)
		mv $out/scan-build-tmp/$scanoutput $out/scan-build
		rm -rf $out/scan-build-tmp
		chmod -R a+rX $out/scan-build
	fi
	false
}

function scanbuild_make() {
	pass=true
	"$rootdir/configure" $config_params --without-shared
	$scanbuild $MAKE $MAKEFLAGS > $out/build_output.txt && rm -rf $out/scan-build-tmp || make_fail_cleanup
	xtrace_disable

	rm -f $out/*files.txt
	for ent in $(find app examples lib module test -type f | grep -vF ".h"); do
		if [[ $ent == lib/env_ocf* ]]; then continue; fi
		if file -bi $ent | grep -q 'text/x-c'; then
			echo $ent | sed 's/\.cp\{0,2\}$//g' >> $out/all_c_files.txt
		fi
	done
	xtrace_restore

	grep -E "CC|CXX" $out/build_output.txt | sed 's/\s\s\(CC\|CXX\)\s//g' | sed 's/\.o//g' > $out/built_c_files.txt
	cat $rootdir/test/common/skipped_build_files.txt >> $out/built_c_files.txt

	sort -o $out/all_c_files.txt $out/all_c_files.txt
	sort -o $out/built_c_files.txt $out/built_c_files.txt
	# from comm manual:
	#   -2 suppress column 2 (lines unique to FILE2)
	#   -3 suppress column 3 (lines that appear in both files)
	# comm may exit 1 if no lines were printed (undocumented, unreliable)
	comm -2 -3 $out/all_c_files.txt $out/built_c_files.txt > $out/unbuilt_c_files.txt || true

	if [ $(wc -l < $out/unbuilt_c_files.txt) -ge 1 ]; then
		cat <<- ERROR
			The following C files were not built.  Either scanbuild CI job needs to
			be updated with proper flags to build these files, or exceptions need
			to be added to test/common/skipped_build_files.txt

			$(<"$out/unbuilt_c_files.txt")
		ERROR
		pass=false
	fi

	$pass
}

function porcelain_check() {
	if [ $(git status --porcelain --ignore-submodules | wc -l) -ne 0 ]; then
		echo "Generated files missing from .gitignore:"
		git status --porcelain --ignore-submodules
		exit 1
	fi
}

# Check that header file dependencies are working correctly by
#  capturing a binary's stat data before and after touching a
#  header file and re-making.
function header_dependency_check() {
	STAT1=$(stat $SPDK_BIN_DIR/spdk_tgt)
	sleep 1
	touch lib/nvme/nvme_internal.h
	$MAKE $MAKEFLAGS
	STAT2=$(stat $SPDK_BIN_DIR/spdk_tgt)

	if [ "$STAT1" == "$STAT2" ]; then
		echo "Header dependency check failed"
		false
	fi
}

function test_make_uninstall() {
	# Create empty file to check if it is not deleted by target uninstall
	touch "$SPDK_WORKSPACE/usr/lib/sample_xyz.a"
	$MAKE $MAKEFLAGS uninstall DESTDIR="$SPDK_WORKSPACE" prefix=/usr
	if [[ $(find "$SPDK_WORKSPACE/usr" -maxdepth 1 -mindepth 1 | wc -l) -ne 2 ]] || [[ $(find "$SPDK_WORKSPACE/usr/lib/" -maxdepth 1 -mindepth 1 | wc -l) -ne 1 ]]; then
		ls -lR "$SPDK_WORKSPACE"
		echo "Make uninstall failed"
		exit 1
	fi
}

function build_doc() {
	local doxygenv
	doxygenv=$(doxygen --version)

	$MAKE -C "$rootdir"/doc --no-print-directory $MAKEFLAGS &> "$out"/doxygen.log
	if [ -s "$out"/doxygen.log ]; then
		if [[ "$doxygenv" == "1.8.20" ]]; then
			# Doxygen 1.8.20 produces false positives, see:
			# https://github.com/doxygen/doxygen/issues/7948
			if grep -vE '\\ilinebr' "$out"/doxygen.log; then
				echo "Doxygen errors found!"
				exit 1
			fi
		elif [[ "$doxygenv" == "1.9.5" ]]; then
			# Doxygen 1.9.5 produces false positives, see:
			# https://github.com/doxygen/doxygen/issues/9552 and
			# https://github.com/doxygen/doxygen/issues/9678
			if grep -vE '\\ifile|@param' "$out"/doxygen.log; then
				echo "Doxygen errors found!"
				exit 1
			fi
		else
			cat "$out"/doxygen.log
			echo "Doxygen errors found!"
			exit 1
		fi

		echo "Doxygen $doxygenv detected. No warnings except false positives, continuing the test"
	fi
	if hash pdflatex 2> /dev/null; then
		$MAKE -C "$rootdir"/doc/output/latex --no-print-directory $MAKEFLAGS &>> "$out"/doxygen.log
	fi
	mkdir -p "$out"/doc
	# Copy and remove files to avoid mv: failed to preserve ownership error
	cp -r --preserve=mode "$rootdir"/doc/output/html "$out"/doc
	rm -rf "$rootdir"/doc/output/html
	if [ -f "$rootdir"/doc/output/latex/refman.pdf ]; then
		mv "$rootdir"/doc/output/latex/refman.pdf "$out"/doc/spdk.pdf
	fi
	$MAKE -C "$rootdir"/doc --no-print-directory $MAKEFLAGS clean &>> "$out"/doxygen.log
	if [ -s "$out"/doxygen.log ]; then
		# Save the log as an artifact in case we are working with potentially broken version
		eq "$doxygenv" 1.8.20 || rm "$out"/doxygen.log
	fi
	rm -rf "$rootdir"/doc/output
}

function autobuild_test_suite() {
	run_test "autobuild_check_format" ./scripts/check_format.sh
	run_test "autobuild_check_so_deps" $rootdir/test/make/check_so_deps.sh $1
	run_test "autobuild_check_dpdk_pci_api" check_dpdk_pci_api
	if [[ $SPDK_TEST_AUTOBUILD == 'full' ]]; then
		run_test "autobuild_external_code" $rootdir/test/external_code/test_make.sh $rootdir
		./configure $config_params --without-shared
		$MAKE $MAKEFLAGS
		run_test "autobuild_generated_files_check" porcelain_check
		run_test "autobuild_header_dependency_check" header_dependency_check
		run_test "autobuild_make_install" $MAKE $MAKEFLAGS install DESTDIR="$SPDK_WORKSPACE" prefix=/usr
		run_test "autobuild_make_uninstall" test_make_uninstall
		run_test "autobuild_build_doc" build_doc
	fi
}

function unittest_build() {
	"$rootdir/configure" $config_params --without-shared
	$MAKE $MAKEFLAGS
}

if [ $SPDK_RUN_ASAN -eq 1 ]; then
	run_test "asan" echo "using asan"
fi

if [ $SPDK_RUN_UBSAN -eq 1 ]; then
	run_test "ubsan" echo "using ubsan"
fi

if [ -n "$SPDK_TEST_NATIVE_DPDK" ]; then
	run_test "build_native_dpdk" build_native_dpdk
fi

if [[ -z $SPDK_TEST_AUTOBUILD ]] || [[ $SPDK_TEST_AUTOBUILD == 'full' ]]; then
	./configure $config_params
	echo "** START ** Info for Hostname: $HOSTNAME"
	uname -a
	$MAKE cc_version
	$MAKE cxx_version
	echo "** END ** Info for Hostname: $HOSTNAME"
elif [[ $SPDK_TEST_AUTOBUILD != 'tiny' ]]; then
	echo "ERROR: supported values for SPDK_TEST_AUTOBUILD are 'full' and 'tiny'"
	exit 1
fi

if [[ $SPDK_TEST_OCF -eq 1 ]]; then
	run_test "autobuild_ocf_precompile" ocf_precompile
fi

if [[ $SPDK_TEST_FUZZER -eq 1 ]]; then
	run_test "autobuild_llvm_precompile" llvm_precompile
fi

if [[ -n $SPDK_TEST_AUTOBUILD ]]; then
	run_test "autobuild" autobuild_test_suite $1
elif [[ $SPDK_TEST_UNITTEST -eq 1 ]]; then
	run_test "unittest_build" unittest_build
elif [[ $SPDK_TEST_SCANBUILD -eq 1 ]]; then
	run_test "scanbuild_make" scanbuild_make
else
	if [[ $SPDK_TEST_FUZZER -eq 1 ]]; then
		# if we are testing nvmf fuzz with llvm lib, --with-shared will cause lib link fail
		./configure $config_params
	else
		# if we aren't testing the unittests, build with shared objects.
		./configure $config_params --with-shared
	fi
	run_test "make" $MAKE $MAKEFLAGS
fi
