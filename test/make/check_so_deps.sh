#!/usr/bin/env bash

if [ "$(uname -s)" = "FreeBSD" ]; then
	echo "Not testing for shared object dependencies on FreeBSD."
	exit 0
fi

rootdir=$(readlink -f $(dirname $0)/../..)
source "$rootdir/test/common/autotest_common.sh"

libdir="$rootdir/build/lib"
libdeps_file="$rootdir/mk/spdk.lib_deps.mk"

# This function is needed to properly evaluate the Make variables into actual dependencies.
function replace_defined_variables() {
	local arr=("$@")
	local bad_values=()
	local good_values=()
	local new_values
	for dep in "${arr[@]}"; do
		if [[ $dep == *'$'* ]]; then
			raw_dep=${dep/$\(/}
			raw_dep=${raw_dep/\)/ }
			bad_values+=("$raw_dep")
		else
			good_values+=("$dep")
		fi
	done
	for dep in "${bad_values[@]}"; do
		mapfile -t dep_def_arr < <(cat $libdeps_file | grep -v "#" | grep "${dep}" | cut -d "=" -f 2 | xargs)
		mapfile -t new_values < <(replace_defined_variables "${dep_def_arr[@]}")
		good_values=( "${good_values[@]}" "${new_values[@]}" )
	done
	echo ${good_values[*]}
}

function confirm_deps() {
	lib=$1
	missing_syms=()
	dep_names=()
	found_symbol_lib=""

	#keep the space here to differentiate bdev and bdev_*
	lib_shortname=$(basename $lib | sed 's,libspdk_,,g' | sed 's,\.so, ,g')
	mapfile -t lib_make_deps < <(cat $libdeps_file | grep "DEPDIRS-${lib_shortname}" | cut -d "=" -f 2 | xargs)
	mapfile -t lib_make_deps < <(replace_defined_variables "${lib_make_deps[@]}")

	for ign_dep in "${IGNORED_LIBS[@]}"; do
		for i in "${!lib_make_deps[@]}"; do
			if [[ ${lib_make_deps[i]} == "$ign_dep" ]]; then
				unset 'lib_make_deps[i]'
			fi
		done
	done

	symbols=$(readelf -s $lib | grep -E "NOTYPE.*GLOBAL.*UND" | awk '{print $8}' | sort | uniq)
	for symbol in $symbols; do
		for deplib in $DEP_LIBS; do
			if [ "$deplib" == "$lib" ]; then
				continue
			fi
			found_symbol=$(readelf -s $deplib | grep -E "DEFAULT\s+[0-9]+\s$symbol$") || true
			if [ "$found_symbol" != "" ]; then
				found_symbol_lib=$(basename $deplib | sed 's,libspdk_,,g' | sed 's,\.so,,g')
				break
			fi
		done
		if [ "$found_symbol" == "" ]; then
			missing_syms+=("$symbol")
		else
			dep_names+=("$found_symbol_lib")
		fi
	done
	IFS=$'\n'
	# Ignore any event_* dependencies. Those are based on the subsystem configuration and not readelf.
	mapfile -t lib_make_deps < <(printf "%s\n" ${lib_make_deps[@]} | sort | grep -v "event_")
	# Ignore the env_dpdk readelf dependency. We don't want people explicitly linking against it.
	mapfile -t dep_names < <(printf "%s\n" ${dep_names[@]} | sort | uniq | grep -v "env_dpdk")
	unset IFS
	diff=$(echo ${dep_names[@]} ${lib_make_deps[@]} | tr ' ' '\n' | sort | uniq -u)
	if [ "$diff" != "" ]; then
		touch $fail_file
		echo "there was a dependency mismatch in the library $lib_shortname"
		echo "The makefile lists: '${lib_make_deps[*]}'"
		echo "readelf outputs   : '${dep_names[*]}'"
		echo "---------------------------------------------------------------------"
	elif [ ${#missing_syms[@]} -ne 0 ]; then
		echo "There are still undefined symbols in the library $lib_shortname"
		printf "%s\n" ${missing_syms[@]}
		echo "---------------------------------------------------------------------"
	fi
}

# By removing the spdk.lib_deps.mk file from spdk.lib.mk, we ensure that we won't
# create any link dependencies. Then we can be sure we get a valid accounting of the
# symbol dependencies we have.
sed -i -e 's,include $(SPDK_ROOT_DIR)/mk/spdk.lib_deps.mk,,g' "$rootdir/mk/spdk.lib.mk"

if [ "$SPDK_TEST_OCF" -eq 1 ]; then
	config_params="$config_params --with-ocf=$rootdir/build/ocf.a"
fi

./configure $config_params --with-shared
$MAKE $MAKEFLAGS

xtrace_disable

echo "---------------------------------------------------------------------"
# Exclude libspdk_env_dpdk.so from the library list. We don't link against this one so that
# users can define their own environment abstraction. However we do want to still check it
# for dependencies to avoid printing out a bunch of confusing symbols under the missing
# symbols section.
SPDK_LIBS=$(ls -1 $libdir/libspdk_*.so | grep -v libspdk_env_dpdk.so)
DEP_LIBS=$(ls -1 $libdir/libspdk_*.so)

IGNORED_LIBS=()
if grep -q 'CONFIG_VHOST_INTERNAL_LIB?=n' $rootdir/mk/config.mk; then
	IGNORED_LIBS+=("rte_vhost")
fi

fail_file=$output_dir/check_so_deps_fail

rm -f $fail_file

for lib in $SPDK_LIBS; do
	confirm_deps $lib&
done

wait

$MAKE $MAKEFLAGS clean
git checkout "$rootdir/mk/spdk.lib.mk"

if [ -f $fail_file ]; then
	rm -f $fail_file
	echo "shared object test failed"
	exit 1
fi

xtrace_restore
