#!/usr/bin/env bash

if [ "$(uname -s)" = "FreeBSD" ]; then
	echo "Not testing for shared object dependencies on FreeBSD."
	exit 0
fi

rootdir=$(readlink -f $(dirname $0)/../..)

if [[ ! -f $1 ]]; then
	echo "ERROR: SPDK test configuration not specified"
	exit 1
fi

source $1
source "$rootdir/test/common/autotest_common.sh"

libdir="$rootdir/build/lib"
libdeps_file="$rootdir/mk/spdk.lib_deps.mk"
source_abi_dir="$HOME/spdk_20_04/build/lib"
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

	cat << EOF > ${suppression_file}
[suppress_variable]
	name = SPDK_LOG_IDXD
[suppress_variable]
	name = SPDK_LOG_IOAT
[suppress_variable]
	name = SPDK_LOG_JSON_UTIL
[suppress_variable]
	name = SPDK_LOG_RPC
[suppress_variable]
	name = SPDK_LOG_RPC_CLIENT
[suppress_function]
	name = spdk_jsonrpc_server_handle_request
[suppress_function]
	name = spdk_jsonrpc_server_handle_error
[suppress_function]
	name = spdk_jsonrpc_server_send_response
[suppress_function]
	name = spdk_jsonrpc_parse_request
[suppress_function]
	name = spdk_jsonrpc_free_request
[suppress_function]
	name = spdk_jsonrpc_parse_response
[suppress_variable]
	name = SPDK_LOG_LOG_RPC
[suppress_variable]
	name = SPDK_LOG_LOG
[suppress_variable]
	name = SPDK_LOG_LVOL
[suppress_variable]
	name = SPDK_LOG_NBD
[suppress_function]
	name = spdk_nbd_disk_find_by_nbd_path
[suppress_function]
	name = spdk_nbd_disk_first
[suppress_function]
	name = spdk_nbd_disk_next
[suppress_function]
	name = spdk_nbd_disk_get_nbd_path
[suppress_function]
	name = spdk_nbd_disk_get_bdev_name
[suppress_variable]
	name = SPDK_LOG_NET
[suppress_function]
	name = spdk_interface_net_interface_add_ip_address
[suppress_function]
	name = spdk_interface_net_interface_delete_ip_address
[suppress_function]
	name = spdk_interface_get_list
[suppress_function]
	name = spdk_get_uevent
[suppress_function]
	name = spdk_uevent_connect
[suppress_function]
	name = spdk_nvme_ctrlr_get_current_process
[suppress_function]
	name = spdk_nvme_ctrlr_get_process
[suppress_function]
	name = spdk_nvme_get_ctrlr_by_trid_unsafe
[suppress_function]
	name = spdk_nvme_io_msg_process
[suppress_function]
	name = spdk_nvme_wait_for_completion
[suppress_function]
	name = spdk_nvme_wait_for_completion_robust_lock
[suppress_function]
	name = spdk_nvme_wait_for_completion_timeout
[suppress_variable]
	name = SPDK_LOG_NVME
[suppress_variable]
	name = SPDK_LOG_OPAL
[suppress_variable]
	name = spdk_opal_method
[suppress_variable]
	name = spdk_opal_uid
[suppress_variable]
	name = SPDK_LOG_REDUCE
[suppress_variable]
	name = SPDK_LOG_THREAD
[suppress_variable]
	name = SPDK_LOG_TRACE
[suppress_function]
	name = spdk_crc32_table_init
[suppress_function]
	name = spdk_crc32_update
[suppress_variable]
	name = SPDK_LOG_VIRTIO_DEV
[suppress_variable]
	name = SPDK_LOG_VIRTIO_PCI
[suppress_variable]
	name = SPDK_LOG_VIRTIO_USER
[suppress_variable]
	name = SPDK_LOG_VMD
[suppress_variable]
	name = SPDK_LOG_ACCEL_IDXD
[suppress_variable]
	name = SPDK_LOG_ACCEL_IOAT
[suppress_variable]
	name = SPDK_LOG_AIO
[suppress_variable]
	name = SPDK_LOG_VBDEV_COMPRESS
[suppress_variable]
	name = SPDK_LOG_CRYPTO
[suppress_variable]
	name = SPDK_LOG_VBDEV_DELAY
[suppress_function]
	name = spdk_vbdev_error_create
[suppress_function]
	name = spdk_vbdev_error_delete
[suppress_function]
	name = spdk_vbdev_error_inject_error
[suppress_variable]
	name = SPDK_LOG_BDEV_FTL
[suppress_variable]
	name = SPDK_LOG_GPT_PARSE
[suppress_variable]
	name = SPDK_LOG_VBDEV_GPT
[suppress_function]
	name = spdk_gpt_parse_mbr
[suppress_function]
	name = spdk_gpt_parse_partition_table
[suppress_variable]
	name = SPDK_LOG_ISCSI_INIT
[suppress_variable]
	name = SPDK_LOG_LVOL_RPC
[suppress_variable]
	name = SPDK_LOG_VBDEV_LVOL
[suppress_variable]
	name = SPDK_LOG_BDEV_MALLOC
[suppress_variable]
	name = SPDK_LOG_BDEV_NULL
[suppress_variable]
	name = SPDK_LOG_BDEV_NVME
[suppress_function]
	name = spdk_bdev_nvme_create
[suppress_function]
	name = spdk_bdev_nvme_delete
[suppress_function]
	name = spdk_bdev_nvme_get_ctrlr
[suppress_function]
	name = spdk_bdev_nvme_get_io_qpair
[suppress_function]
	name = spdk_bdev_nvme_get_opts
[suppress_function]
	name = spdk_bdev_nvme_set_hotplug
[suppress_function]
	name = spdk_bdev_nvme_set_opts
[suppress_function]
	name = spdk_vbdev_opal_create
[suppress_function]
	name = spdk_vbdev_opal_destruct
[suppress_function]
	name = spdk_vbdev_opal_enable_new_user
[suppress_function]
	name = spdk_vbdev_opal_get_info_from_bdev
[suppress_function]
	name = spdk_vbdev_opal_set_lock_state
[suppress_variable]
	name = SPDK_LOG_BDEV_OCSSD
[suppress_variable]
	name = SPDK_LOG_VBDEV_OPAL
[suppress_variable]
	name = SPDK_LOG_OCFCTX
[suppress_variable]
	name = SPDK_LOG_VBDEV_PASSTHRU
[suppress_variable]
	name = SPDK_LOG_BDEV_PMEM
[suppress_function]
	name = spdk_create_pmem_disk
[suppress_function]
	name = spdk_delete_pmem_disk
[suppress_variable]
	name = SPDK_LOG_BDEV_RAID
[suppress_variable]
	name = SPDK_LOG_BDEV_RAID0
[suppress_variable]
	name = SPDK_LOG_BDEV_RAID5
[suppress_variable]
	name = SPDK_LOG_RAID_RPC
[suppress_variable]
	name = SPDK_LOG_BDEV_RBD
[suppress_function]
	name = spdk_bdev_rbd_create
[suppress_function]
	name = spdk_bdev_rbd_delete
[suppress_function]
	name = spdk_bdev_rbd_dup_config
[suppress_function]
	name = spdk_bdev_rbd_free_config
[suppress_function]
	name = spdk_bdev_rbd_resize
[suppress_variable]
	name = SPDK_LOG_VBDEV_SPLIT
[suppress_function]
	name = spdk_vbdev_split_destruct
[suppress_function]
	name = spdk_vbdev_split_get_part_base
[suppress_variable]
	name = SPDK_LOG_URING
[suppress_variable]
	name = SPDK_LOG_VIRTIO
[suppress_variable]
	name = SPDK_LOG_VIRTIO_BLK
[suppress_variable]
	name = SPDK_LOG_VBDEV_ZONE_BLOCK
[suppress_function]
	name = spdk_vbdev_zone_block_create
[suppress_function]
	name = spdk_vbdev_zone_block_delete
[suppress_variable]
	name = SPDK_LOG_BLOBFS_BDEV
[suppress_variable]
	name = SPDK_LOG_BLOBFS_BDEV_RPC
[suppress_function]
	name = spdk_blobfs_fuse_send_request
[suppress_function]
	name = spdk_blobfs_fuse_start
[suppress_function]
	name = spdk_blobfs_fuse_stop
[suppress_variable]
	name = SPDK_LOG_APP_RPC
[suppress_function]
	name = spdk_nvmf_parse_conf
[suppress_variable]
	name = SPDK_LOG_VHOST
[suppress_variable]
	name = SPDK_LOG_VHOST_BLK
[suppress_variable]
	name = SPDK_LOG_VHOST_BLK_DATA
[suppress_variable]
	name = SPDK_LOG_VHOST_RING
[suppress_variable]
	name = SPDK_LOG_VHOST_RPC
[suppress_variable]
	name = SPDK_LOG_VHOST_SCSI
[suppress_variable]
	name = SPDK_LOG_VHOST_SCSI_DATA
[suppress_variable]
	name = SPDK_LOG_VHOST_SCSI_QUEUE
[suppress_variable]
	name = spdk_vhost_scsi_device_backend
[suppress_type]
	name = spdk_net_impl
[suppress_type]
	name = spdk_lvol
[suppress_type]
	name = spdk_pci_device
EOF

	for object in "$libdir"/libspdk_*.so; do
		so_file=$(basename $object)
		if [ ! -f "$source_abi_dir/$so_file" ]; then
			echo "No corresponding object for $so_file in canonical directory. Skipping."
			continue
		fi

		if ! output=$(abidiff "$source_abi_dir/$so_file" "$libdir/$so_file" --headers-dir1 "$source_abi_dir/../../include/" --headers-dir2 "$rootdir/include" --leaf-changes-only --suppressions $suppression_file --stat); then
			# remove any filtered out variables.
			output=$(sed "s/ [()][^)]*[)]//g" <<< "$output")

			IFS="." read -r _ _ new_so_maj new_so_min < <(readlink "$libdir/$so_file")
			IFS="." read -r _ _ old_so_maj old_so_min < <(readlink "$source_abi_dir/$so_file")

			found_abi_change=false
			so_name_changed=no

			if [[ $output == *"ELF SONAME changed"* ]]; then
				so_name_changed=yes
			fi

			changed_leaf_types=0
			if [[ $output =~ "leaf types summary: "([0-9]+) ]]; then
				changed_leaf_types=${BASH_REMATCH[1]}
			fi

			removed_functions=0 changed_functions=0 added_functions=0
			if [[ $output =~ "functions summary: "([0-9]+)" Removed, "([0-9]+)" Changed, "([0-9]+)" Added" ]]; then
				removed_functions=${BASH_REMATCH[1]} changed_functions=${BASH_REMATCH[2]} added_functions=${BASH_REMATCH[3]}
			fi

			removed_vars=0 changed_vars=0 added_vars=0
			if [[ $output =~ "variables summary: "([0-9]+)" Removed, "([0-9]+)" Changed, "([0-9]+)" Added" ]]; then
				removed_vars=${BASH_REMATCH[1]} changed_vars=${BASH_REMATCH[2]} added_vars=${BASH_REMATCH[3]}
			fi

			if ((changed_leaf_types != 0)); then
				if ((new_so_maj == old_so_maj)); then
					touch $fail_file
					echo "Please update the major SO version for $so_file. A header accesible type has been modified since last release."
				fi
				found_abi_change=true
			fi

			if ((removed_functions != 0)) || ((removed_vars != 0)); then
				if ((new_so_maj == old_so_maj)); then
					touch $fail_file
					echo "Please update the major SO version for $so_file. API functions or variables have been removed since last release."
				fi
				found_abi_change=true
			fi

			if ((changed_functions != 0)) || ((changed_vars != 0)); then
				if ((new_so_maj == old_so_maj)); then
					touch $fail_file
					echo "Please update the major SO version for $so_file. API functions or variables have been changed since last release."
				fi
				found_abi_change=true
			fi

			if ((added_functions != 0)) || ((added_vars != 0)); then
				if ((new_so_min == old_so_min && new_so_maj == old_so_maj)) && ! $found_abi_change; then
					touch $fail_file
					echo "Please update the minor SO version for $so_file. API functions or variables have been added since last release."
				fi
				found_abi_change=true
			fi

			if [[ $so_name_changed == yes ]]; then
				if ! $found_abi_change; then
					# Unfortunately, libspdk_idxd made it into 20.04 without an SO suffix. TODO:: remove after 20.07
					if [ "$so_file" != "libspdk_idxd.so" ] && [ "$so_file" != "libspdk_accel_idxd.so" ]; then
						echo "SO name for $so_file changed without a change to abi. please revert that change."
						touch $fail_file
					fi
				fi

				if ((new_so_maj != old_so_maj && new_so_min != 0)); then
					echo "SO major version for $so_file was bumped. Please reset the minor version to 0."
					touch $fail_file
				fi

				expected_new_so_min=$((old_so_min + 1))
				if ((new_so_min > old_so_min && expected_new_so_min != new_so_min)); then
					echo "SO minor version for $so_file was incremented more than once. Please revert minor version to $expected_new_so_min."
					touch $fail_file
				fi
			fi

			continue
		fi
		processed_so=$((processed_so + 1))
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
		good_values=("${good_values[@]}" "${new_values[@]}")
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
	lib_make_deps=($(printf "%s\n" "${lib_make_deps[@]}" | sort | grep -v "event_"))
	# Ignore the env_dpdk readelf dependency. We don't want people explicitly linking against it.
	dep_names=($(printf "%s\n" "${dep_names[@]}" | sort | uniq | grep -v "env_dpdk"))
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

source ~/autorun-spdk.conf
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

(
	for lib in $SPDK_LIBS; do confirm_deps $lib & done
	wait
)

$MAKE $MAKEFLAGS clean
git checkout "$rootdir/mk/spdk.lib.mk"

if [ -f $fail_file ]; then
	rm -f $fail_file
	echo "shared object test failed"
	exit 1
fi

xtrace_restore
