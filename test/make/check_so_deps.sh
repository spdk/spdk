#!/usr/bin/env bash

rootdir=$(readlink -f $(dirname $0)/../..)
source "$rootdir/test/common/autotest_common.sh"

xtrace_disable

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
		dep_def_arr=($(cat $libdeps_file | grep -v "#" | grep "${dep}" | cut -d "=" -f 2 | xargs))
		new_values=($(replace_defined_variables "${dep_def_arr[@]}"))
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
	lib_make_deps=($(cat $libdeps_file | grep "DEPDIRS-${lib_shortname}" | cut -d "=" -f 2 | xargs))
	lib_make_deps=($(replace_defined_variables "${lib_make_deps[@]}"))

	for dep in ${lib_make_deps[@]}; do
		if [[ $dep == *'$'* ]]; then
			dep_no_dollar=$(echo $dep | sed 's,$(,,g' | sed 's,),,g')
		fi
	done

	symbols=$(readelf -s $lib | grep -E "NOTYPE.*GLOBAL.*UND" | awk '{print $8}' | sort | uniq)
	for symbol in $symbols; do
		for deplib in $SPDK_LIBS; do
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
	#Ignore any event_* dependencies. Those are based on the subsystem configuration and not readelf.
	lib_make_deps=( $(printf "%s\n" ${lib_make_deps[@]} | sort | grep -v "event_") )
	dep_names=( $(printf "%s\n" ${dep_names[@]} | sort | uniq) )
	unset IFS
	diff=$(echo ${dep_names[@]} ${lib_make_deps[@]} | tr ' ' '\n' | sort | uniq -u)
	if [ "$diff" != "" ]; then
		mismatch=true
		echo "there was a dependency mismatch in the library $lib_shortname"
		echo "The makefile lists: '${lib_make_deps[*]}'"
		echo "readelf outputs   : '${dep_names[*]}'"
		echo "---------------------------------------------------------------------"
	elif [ ${#missing_sims[@]} -ne 0 ]; then
		echo "There are still undefined files in the library $lib_shortname"
		printf "%s\n" ${missing_sims[@]}
		echo "---------------------------------------------------------------------"
	fi
}

# By removing the spdk.lib_deps.mk file from spdk.lib.mk, we ensure that we won't
# create any link dependencies. Then we can be sure we get a valid accounting of the
# symbol dependencies we have.
sed -i -e's,include $(SPDK_ROOT_DIR)/mk/spdk.lib_deps.mk,,g' "$rootdir/mk/spdk.lib.mk"

./configure $config_params --with-shared
$MAKE $MAKEFLAGS

echo "---------------------------------------------------------------------"
# Exclude libspdk_env_dpdk.so. We don't link against this one so that users can define
# their own environment abstraction.
SPDK_LIBS=$(ls -1 $libdir/libspdk_*.so | grep -v libspdk_env_dpdk.so)

mismatch=false

for lib in $SPDK_LIBS; do
	confirm_deps $lib&
done

wait

if [ "$mismatch" == "true" ]; then
	exit 1
fi

$MAKE $MAKEFLAGS clean
git checkout "$rootdir/mk/spdk.lib.mk"

xtrace_restore
