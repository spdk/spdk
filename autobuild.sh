#!/usr/bin/env bash

set -e

# If the configuration of tests is not provided, no tests will be carried out.
if [[ ! -f $1 ]]; then
	echo "ERROR: SPDK test configuration not specified"
	exit 1
fi

rootdir=$(readlink -f $(dirname $0))

source "$1"
source "$rootdir/test/common/autotest_common.sh"

out=$output_dir
scanbuild="scan-build -o $output_dir/scan-build-tmp --status-bugs"
config_params=$(get_config_params)

rm -rf /tmp/spdk
mkdir /tmp/spdk
umask 022
cd $rootdir

# Print some test system info out for the log
date -u
git describe --tags
./configure $config_params
echo "** START ** Info for Hostname: $HOSTNAME"
uname -a
$MAKE cc_version
$MAKE cxx_version
echo "** END ** Info for Hostname: $HOSTNAME"

function ocf_precompile {
	# We compile OCF sources ourselves
	# They don't need to be checked with scanbuild and code coverage is not applicable
	# So we precompile OCF now for further use as standalone static library
	./configure $(echo $config_params | sed 's/--enable-coverage//g')
	$MAKE $MAKEFLAGS include/spdk/config.h
	CC=gcc CCAR=ar $MAKE $MAKEFLAGS -C lib/env_ocf exportlib O=$rootdir/build/ocf.a
	# Set config to use precompiled library
	config_params="$config_params --with-ocf=/$rootdir/build/ocf.a"
	# need to reconfigure to avoid clearing ocf related files on future make clean.
	./configure $config_params
}

function make_fail_cleanup {
	if [ -d $out/scan-build-tmp ]; then
		scanoutput=$(ls -1 $out/scan-build-tmp/)
		mv $out/scan-build-tmp/$scanoutput $out/scan-build
		rm -rf $out/scan-build-tmp
		chmod -R a+rX $out/scan-build
	fi
	false
}

function scanbuild_make {
	pass=true
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
		echo "missing files"
		cat $out/unbuilt_c_files.txt
		pass=false
	fi

	$pass
}

function porcelain_check {
	if [ $(git status --porcelain --ignore-submodules | wc -l) -ne 0 ]; then
		echo "Generated files missing from .gitignore:"
		git status --porcelain --ignore-submodules
		exit 1
	fi
}

# Check that header file dependencies are working correctly by
#  capturing a binary's stat data before and after touching a
#  header file and re-making.
function header_dependency_check {
	STAT1=$(stat app/spdk_tgt/spdk_tgt)
	sleep 1
	touch lib/nvme/nvme_internal.h
	$MAKE $MAKEFLAGS
	STAT2=$(stat app/spdk_tgt/spdk_tgt)

	if [ "$STAT1" == "$STAT2" ]; then
		echo "Header dependency check failed"
		false
	fi
}

function test_make_uninstall {
	# Create empty file to check if it is not deleted by target uninstall
	touch /tmp/spdk/usr/lib/sample_xyz.a
	$MAKE $MAKEFLAGS uninstall DESTDIR=/tmp/spdk prefix=/usr
	if [[ $(find /tmp/spdk/usr -maxdepth 1 -mindepth 1 | wc -l) -ne 2 ]] || [[ $(find /tmp/spdk/usr/lib/ -maxdepth 1 -mindepth 1 | wc -l) -ne 1 ]]; then
		ls -lR /tmp/spdk
		rm -rf /tmp/spdk
		echo "Make uninstall failed"
		exit 1
	else
		rm -rf /tmp/spdk
	fi
}

function build_doc {
	$MAKE -C "$rootdir"/doc --no-print-directory $MAKEFLAGS &> "$out"/doxygen.log
	if [ -s "$out"/doxygen.log ]; then
		cat "$out"/doxygen.log
		echo "Doxygen errors found!"
		exit 1
	fi
	if hash pdflatex 2>/dev/null; then
		$MAKE -C "$rootdir"/doc/output/latex --no-print-directory $MAKEFLAGS &>> "$out"/doxygen.log
	fi
	mkdir -p "$out"/doc
	mv "$rootdir"/doc/output/html "$out"/doc
	if [ -f "$rootdir"/doc/output/latex/refman.pdf ]; then
		mv "$rootdir"/doc/output/latex/refman.pdf "$out"/doc/spdk.pdf
	fi
	$MAKE -C "$rootdir"/doc --no-print-directory $MAKEFLAGS clean &>> "$out"/doxygen.log
	if [ -s "$out"/doxygen.log ]; then
		rm "$out"/doxygen.log
	fi
	rm -rf "$rootdir"/doc/output
}

function autobuild_test_suite {
	run_test "autobuild_check_format" ./scripts/check_format.sh
	run_test "autobuild_external_code" sudo -E $rootdir/test/external_code/test_make.sh $rootdir
	if [ "$SPDK_TEST_OCF" -eq 1 ]; then
		run_test "autobuild_ocf_precompile" ocf_precompile
	fi
	run_test "autobuild_check_so_deps" $rootdir/test/make/check_so_deps.sh
	run_test "scanbuild_make" scanbuild_make
	run_test "autobuild_generated_files_check" porcelain_check
	run_test "autobuild_header_dependency_check" header_dependency_check
	run_test "autobuild_make_install" $MAKE $MAKEFLAGS install DESTDIR=/tmp/spdk prefix=/usr
	run_test "autobuild_make_uninstall" test_make_uninstall
	run_test "autobuild_build_doc" build_doc
}

if [ $SPDK_RUN_VALGRIND -eq 1 ]; then
	run_test "valgrind" echo "using valgrind"
fi

if [ $SPDK_RUN_ASAN -eq 1 ]; then
	run_test "asan" echo "using asan"
fi

if [ $SPDK_RUN_UBSAN -eq 1 ]; then
	run_test "ubsan" echo "using ubsan"
fi

if [ "$SPDK_TEST_AUTOBUILD" -eq 1 ]; then
	run_test "autobuild" autobuild_test_suite
else
	if [ "$SPDK_TEST_OCF" -eq 1 ]; then
		run_test "autobuild_ocf_precompile" ocf_precompile
	fi
	run_test "make" $MAKE $MAKEFLAGS
fi
