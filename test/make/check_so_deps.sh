#!/usr/bin/env bash

if [ "$(uname -s)" = "FreeBSD" ]; then
	echo "Not testing for shared object dependencies on FreeBSD."
	exit 0
fi

rootdir=$(readlink -f $(dirname $0)/../..)
source "$rootdir/test/common/autotest_common.sh"

libdir="$rootdir/build/lib"
libdeps_file="$rootdir/mk/spdk.lib_deps.mk"
source_abi_dir="$HOME/spdk_20_01/build/lib"
suppression_file="$HOME/abigail_suppressions.ini"

function confirm_abi_deps() {
	local processed_so=0

	if ! hash abidiff; then
		echo "Unable to check ABI compatibility. Please install abidiff."
		return 1
	fi

	if [ ! -d $source_abi_dir ]; then
		echo "No source ABI available, failing this test."
		return 1
	fi

	cat <<EOF > ${suppression_file}
[suppress_variable]
	name = SPDK_LOG_BDEV
[suppress_variable]
	name = SPDK_LOG_BLOBFS_BDEV
[suppress_variable]
	name = SPDK_LOG_BLOBFS_BDEV_RPC
[suppress_variable]
	name = SPDK_LOG_APP_RPC
[suppress_variable]
	name = SPDK_LOG_FTL_CORE
[suppress_variable]
	name = SPDK_LOG_FTL_INIT
[suppress_variable]
	name = SPDK_LOG_ISCSI
[suppress_variable]
	name = SPDK_LOG_SCSI

EOF

	for object in "$libdir"/libspdk_*.so; do
		so_file=$(basename $object)
		if [ ! -f "$source_abi_dir/$so_file" ]; then
			echo "No corresponding object for $so_file in canonical directory. Skipping."
			continue
		fi

		if ! abidiff $source_abi_dir/$so_file $libdir/$so_file --leaf-changes-only --suppressions $suppression_file --stat > /dev/null; then
			found_abi_change=false
			output=$(abidiff $source_abi_dir/$so_file $libdir/$so_file --leaf-changes-only --suppressions $suppression_file --stat) || true
			new_so_maj=$(readlink $libdir/$so_file | awk -F'\\.so\\.' '{print $2}' | cut -d '.' -f1)
			new_so_min=$(readlink $libdir/$so_file | awk -F'\\.so\\.' '{print $2}' | cut -d '.' -f2)
			old_so_maj=$(readlink $source_abi_dir/$so_file | awk -F'\\.so\\.' '{print $2}' | cut -d '.' -f1)
			old_so_min=$(readlink $source_abi_dir/$so_file | awk -F'\\.so\\.' '{print $2}' | cut -d '.' -f2)
			so_name_changed=$(grep "ELF SONAME changed" <<< "$output") || so_name_changed="No"
			function_summary=$(grep "functions summary" <<< "$output")
			variable_summary=$(grep "variables summary" <<< "$output")
			# remove any filtered out variables.
			variable_summary=$(sed "s/ [()][^)]*[)]//g" <<< "$variable_summary")

			read -r _ _ _ removed_functions _ changed_functions _ added_functions _ <<< "$function_summary"
			read -r _ _ _ removed_vars _ changed_vars _ added_vars _ <<< "$variable_summary"

			if [ $removed_functions -ne 0 ] || [ $removed_vars -ne 0 ]; then
				if [ "$new_so_maj" == "$old_so_maj" ]; then
					touch $fail_file
					echo "Please update the major SO version for $so_file. API functions or variables have been removed since last release."
				fi
				found_abi_change=true
			fi

			if [ $changed_functions -ne 0 ] || [ $changed_vars -ne 0 ]; then
				if [ "$new_so_maj" == "$old_so_maj" ]; then
					touch $fail_file
					echo "Please update the major SO version for $so_file. API functions or variables have been changed since last release."
				fi
				found_abi_change=true
			fi

			if [ $added_functions -ne 0 ] || [ $added_vars -ne 0 ]; then
				if [ "$new_so_min" == "$old_so_min" ] && [ "$new_so_maj" == "$old_so_maj" ] && ! $found_abi_change; then
					touch $fail_file
					echo "Please update the minor SO version for $so_file. API functions or variables have been added since last release."
				fi
				found_abi_change=true
			fi

			if [ "$so_name_changed" != "No" ]; then
				if ! $found_abi_change; then
					echo "SO name for $so_file changed without a change to abi. please revert that change."
					touch $fail_file
				fi

				if [ "$new_so_maj" != "$old_so_maj" ] && [ "$new_so_min"  != "0" ]; then
					echo "SO major version for $so_file was bumped. Please reset the minor version to 0."
					touch $fail_file
				fi
			fi

			continue
		fi
		processed_so=$((processed_so+1))
	done
	rm -f $suppression_file
	echo "Processed $processed_so objects."
}

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
		dep_def_arr=($(grep -v "#" $libdeps_file | grep "${dep}" | cut -d "=" -f 2 | xargs))
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
	lib_make_deps=($(grep "DEPDIRS-${lib_shortname}" $libdeps_file | cut -d "=" -f 2 | xargs))
	lib_make_deps=($(replace_defined_variables "${lib_make_deps[@]}"))

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
	lib_make_deps=( $(printf "%s\n" "${lib_make_deps[@]}" | sort | grep -v "event_") )
	# Ignore the env_dpdk readelf dependency. We don't want people explicitly linking against it.
	dep_names=( $(printf "%s\n" "${dep_names[@]}" | sort | uniq | grep -v "env_dpdk") )
	unset IFS
	diff=$(echo "${dep_names[@]}" "${lib_make_deps[@]}" | tr ' ' '\n' | sort | uniq -u)
	if [ "$diff" != "" ]; then
		touch $fail_file
		echo "there was a dependency mismatch in the library $lib_shortname"
		echo "The makefile lists: '${lib_make_deps[*]}'"
		echo "readelf outputs   : '${dep_names[*]}'"
		echo "---------------------------------------------------------------------"
	fi
}

# By removing the spdk.lib_deps.mk file from spdk.lib.mk, we ensure that we won't
# create any link dependencies. Then we can be sure we get a valid accounting of the
# symbol dependencies we have.
sed -i -e 's,include $(SPDK_ROOT_DIR)/mk/spdk.lib_deps.mk,,g' "$rootdir/mk/spdk.lib.mk"

config_params=$(get_config_params)
if [ "$SPDK_TEST_OCF" -eq 1 ]; then
	config_params="$config_params --with-ocf=$rootdir/build/ocf.a"
fi

$MAKE $MAKEFLAGS clean
./configure $config_params --with-shared
$MAKE $MAKEFLAGS

xtrace_disable

fail_file=$output_dir/check_so_deps_fail

rm -f $fail_file

run_test "confirm_abi_deps" confirm_abi_deps

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

( for lib in $SPDK_LIBS; do confirm_deps $lib & done; wait )

$MAKE $MAKEFLAGS clean
git checkout "$rootdir/mk/spdk.lib.mk"

if [ -f $fail_file ]; then
	rm -f $fail_file
	echo "shared object test failed"
	exit 1
fi

xtrace_restore
