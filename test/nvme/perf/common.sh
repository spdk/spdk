#!/usr/bin/env bash

function discover_bdevs() {
	local rootdir=$1
	local config_file=$2
	local wait_for_spdk_bdev=30
	local rpc_server=/var/tmp/spdk-discover-bdevs.sock

	if [ ! -e $config_file ]; then
		echo "Invalid Configuration File: $config_file"
		return 1
	fi

	# Start the bdev service to query for the list of available
	# bdevs.
	$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 \
		--json $config_file &> /dev/null &
	stubpid=$!
	while ! [ -e /var/run/spdk_bdev0 ]; do
		# If this counter drops to zero, errexit will be caught to abort the test
		((wait_for_spdk_bdev--))
		sleep 1
	done

	# Get all of the bdevs
	$rootdir/scripts/rpc.py -s "$rpc_server" bdev_get_bdevs

	# Shut down the bdev service
	kill $stubpid
	wait $stubpid
	rm -f /var/run/spdk_bdev0
}

function create_spdk_bdev_conf() {
	local output
	local disk_cfg
	local bdev_io_cache_size=$1
	local bdev_io_pool_size=$2
	local bdev_json_cfg=()
	local bdev_opts=()

	disk_cfg=($(grep -vP "^\s*#" "$DISKCFG"))

	if [[ -n "$bdev_io_cache_size" ]]; then
		bdev_opts+=("\"bdev_io_cache_size\": $bdev_io_cache_size")
	fi

	if [[ -n "$bdev_io_pool_size" ]]; then
		bdev_opts+=("\"bdev_io_pool_size\": $bdev_io_pool_size")
	fi

	local IFS=","
	if [[ ${#bdev_opts[@]} -gt 0 ]]; then
		bdev_json_cfg+=("$(
			cat <<- JSON
				{
					"method": "bdev_set_options",
					"params": {
						${bdev_opts[*]}
					}
				}
			JSON
		)")
	fi

	for i in "${!disk_cfg[@]}"; do
		bdev_json_cfg+=("$(
			cat <<- JSON
				{
					"method": "bdev_nvme_attach_controller",
					"params": {
						"trtype": "PCIe",
						"name":"Nvme${i}",
						"traddr":"${disk_cfg[i]}"
					}
				}
			JSON
		)")
	done

	local IFS=","
	jq -r '.' <<- JSON > $testdir/bdev.conf
		{
			"subsystems": [
				{
					"subsystem": "bdev",
					"config": [
						${bdev_json_cfg[*]},
					        {
					                "method": "bdev_wait_for_examine"
					        }
					]
				}
			]
		}
	JSON
}

function is_bdf_not_mounted() {
	local bdf=$1
	local blkname
	local mountpoints
	blkname=$(ls -l /sys/block/ | grep $bdf | awk '{print $9}')
	mountpoints=$(lsblk /dev/$blkname --output MOUNTPOINT -n | wc -w)
	return $mountpoints
}

function get_cores() {
	local cpu_list="$1"
	for cpu in ${cpu_list//,/ }; do
		echo $cpu
	done
}

function get_cores_numa_node() {
	local cores=$1
	for core in $cores; do
		lscpu -p=cpu,node | grep "^$core\b" | awk -F ',' '{print $2}'
	done
}

function get_numa_node() {
	local plugin=$1
	local disks=$2
	if [[ "$plugin" =~ "nvme" ]]; then
		for bdf in $disks; do
			local driver
			driver=$(grep DRIVER /sys/bus/pci/devices/$bdf/uevent | awk -F"=" '{print $2}')
			# Use this check to omit blocked devices ( not bound to driver with setup.sh script )
			if [ "$driver" = "vfio-pci" ] || [ "$driver" = "uio_pci_generic" ]; then
				cat /sys/bus/pci/devices/$bdf/numa_node
			fi
		done
	elif [[ "$plugin" =~ "bdev" ]]; then
		local bdevs
		bdevs=$(discover_bdevs $rootdir $testdir/bdev.conf)
		for name in $disks; do
			local bdev_bdf
			bdev_bdf=$(jq -r ".[] | select(.name==\"$name\").driver_specific.nvme.pci_address" <<< $bdevs)
			cat /sys/bus/pci/devices/$bdev_bdf/numa_node
		done
	else
		for name in $disks; do
			local bdf
			# Not reading directly from /sys/block/nvme* because of a kernel bug
			# which results in NUMA 0 always getting reported.
			bdf=$(cat /sys/block/$name/device/address)
			cat /sys/bus/pci/devices/$bdf/numa_node
		done
	fi
}

function get_disks() {
	local plugin=$1
	local disk_cfg

	disk_cfg=($(grep -vP "^\s*#" "$DISKCFG"))
	if [[ "$plugin" =~ "nvme" ]]; then
		# PCI BDF address is enough for nvme-perf and nvme-fio-plugin,
		# so just print them from configuration file
		echo "${disk_cfg[*]}"
	elif [[ "$plugin" =~ "bdev" ]]; then
		# Generate NvmeXn1 bdev name configuration file for bdev-perf
		# and bdev-fio-plugin
		local bdevs
		local disk_no
		disk_no=${#disk_cfg[@]}
		eval echo "Nvme{0..$((disk_no - 1))}n1"
	else
		# Find nvme block devices and only use the ones which
		# are not mounted
		for bdf in "${disk_cfg[@]}"; do
			if is_bdf_not_mounted $bdf; then
				local blkname
				blkname=$(ls -l /sys/block/ | grep $bdf | awk '{print $9}')
				echo $blkname
			fi
		done
	fi
}

function get_disks_on_numa() {
	local devs=($1)
	local numas=($2)
	local numa_no=$3
	local disks_on_numa=""
	local i

	for ((i = 0; i < ${#devs[@]}; i++)); do
		if [ ${numas[$i]} = $numa_no ]; then
			disks_on_numa=$((disks_on_numa + 1))
		fi
	done
	echo $disks_on_numa
}

function create_fio_config() {
	local disk_no=$1
	local plugin=$2
	local disks=($3)
	local disks_numa=($4)
	local cores=($5)
	local total_disks=${#disks[@]}
	local fio_job_section=()
	local num_cores=${#cores[@]}
	local disks_per_core=$((disk_no / num_cores))
	local disks_per_core_mod=$((disk_no % num_cores))
	local cores_numa
	cores_numa=($(get_cores_numa_node "${cores[*]}"))

	# Following part of this function still leverages global variables a lot.
	# It's a mix of local variables passed as arguments to function with global variables. This is messy.
	# TODO: Modify this to be consistent with how variables are used here. Aim for using only
	# local variables to get rid of globals as much as possible.
	desc="\"Test io_plugin=$PLUGIN Blocksize=${BLK_SIZE} Workload=$RW MIX=${MIX} qd=${IODEPTH}\""
	cp "$testdir/config.fio.tmp" "$testdir/config.fio"
	cat <<- EOF >> $testdir/config.fio
		description=$desc

		rw=$RW
		rwmixread=$MIX
		bs=$BLK_SIZE
		runtime=$RUNTIME
		ramp_time=$RAMP_TIME
		numjobs=$NUMJOBS
		log_avg_msec=$SAMPLING_INT
	EOF

	if $GTOD_REDUCE; then
		echo "gtod_reduce=1" >> $testdir/config.fio
	fi

	if [[ $PLUGIN =~ "uring" ]]; then
		cat <<- EOF >> $testdir/config.fio
			fixedbufs=1
			hipri=1
			registerfiles=1
			sqthread_poll=1
		EOF
	fi

	if [[ "$IO_BATCH_SUBMIT" -gt 0 ]]; then
		echo "iodepth_batch_submit=$IO_BATCH_SUBMIT" >> $testdir/config.fio
	fi

	if [[ "$IO_BATCH_COMPLETE" -gt 0 ]]; then
		echo "iodepth_batch_complete=$IO_BATCH_COMPLETE" >> $testdir/config.fio
	fi

	for i in "${!cores[@]}"; do
		local m=0 #Counter of disks per NUMA node
		local n=0 #Counter of all disks in test
		core_numa=${cores_numa[$i]}

		total_disks_per_core=$disks_per_core
		# Check how many "stray" disks are unassigned to CPU cores
		# Assign one disk to current CPU core and substract it from the total of
		# unassigned disks
		if [[ "$disks_per_core_mod" -gt "0" ]]; then
			total_disks_per_core=$((disks_per_core + 1))
			disks_per_core_mod=$((disks_per_core_mod - 1))
		fi
		# SPDK fio plugin supports submitting/completing I/Os to multiple SSDs from a single thread.
		# Therefore, the per thread queue depth is set to the desired IODEPTH/device X the number of devices per thread.
		QD=$IODEPTH
		if [[ "$NOIOSCALING" == false ]]; then
			QD=$((IODEPTH * total_disks_per_core))
		fi

		if [[ "$FIO_FNAME_STRATEGY" == "group" ]]; then
			fio_job_section+=("")
			fio_job_section+=("[filename${i}]")
			fio_job_section+=("iodepth=$QD")
			fio_job_section+=("cpus_allowed=${cores[$i]} #CPU NUMA Node ${cores_numa[$i]}")
		fi

		while [[ "$m" -lt "$total_disks_per_core" ]]; do
			# Try to add disks to job section if it's NUMA node matches NUMA
			# for currently selected CPU
			if [[ "${disks_numa[$n]}" == "$core_numa" ]]; then
				if [[ "$FIO_FNAME_STRATEGY" == "split" ]]; then
					fio_job_section+=("")
					fio_job_section+=("[filename${m}-${cores[$i]}]")
					fio_job_section+=("iodepth=$QD")
					fio_job_section+=("cpus_allowed=${cores[$i]} #CPU NUMA Node ${cores_numa[$i]}")
				fi

				if [[ "$plugin" == "spdk-plugin-nvme" ]]; then
					fio_job_section+=("filename=trtype=PCIe traddr=${disks[$n]//:/.} ns=1 #NVMe NUMA Node ${disks_numa[$n]}")
				elif [[ "$plugin" == "spdk-plugin-bdev" ]]; then
					fio_job_section+=("filename=${disks[$n]} #NVMe NUMA Node ${disks_numa[$n]}")
				elif [[ "$plugin" =~ "kernel" ]]; then
					fio_job_section+=("filename=/dev/${disks[$n]} #NVMe NUMA Node ${disks_numa[$n]}")
				fi
				m=$((m + 1))

				#Mark numa of n'th disk as "x" to mark it as claimed for next loop iterations
				disks_numa[$n]="x"
			fi
			n=$((n + 1))

			# If there is no more disks with numa node same as cpu numa node, switch to
			# other numa node, go back to start of loop and try again.
			if [[ $n -ge $total_disks ]]; then
				echo "WARNING! Cannot assign any more NVMes for CPU ${cores[$i]}"
				echo "NVMe assignment for this CPU will be cross-NUMA."
				if [[ "$core_numa" == "1" ]]; then
					core_numa=0
				else
					core_numa=1
				fi
				n=0
			fi
		done
	done

	printf "%s\n" "${fio_job_section[@]}" >> $testdir/config.fio
	echo "INFO: Generated fio configuration file:"
	cat $testdir/config.fio
}

function preconditioning() {
	local dev_name=""
	local filename=""
	local nvme_list

	HUGEMEM=8192 $rootdir/scripts/setup.sh
	cp $testdir/config.fio.tmp $testdir/config.fio
	echo "[Preconditioning]" >> $testdir/config.fio

	# Generate filename argument for FIO.
	# We only want to target NVMes not bound to nvme driver.
	# If they're still bound to nvme that means they were skipped by
	# setup.sh on purpose.
	nvme_list=$(get_disks nvme)
	for nvme in $nvme_list; do
		dev_name='trtype=PCIe traddr='${nvme//:/.}' ns=1'
		filename+=$(printf %s":" "$dev_name")
	done
	echo "** Preconditioning disks, this can take a while, depending on the size of disks."
	run_spdk_nvme_fio "spdk-plugin-nvme" --filename="$filename" --size=100% --loops=2 --bs=1M \
		--rw=write --iodepth=32 --output-format=normal
	rm -f $testdir/config.fio
}

function bc() {
	$(type -P bc) -l <<< "scale=3; $1"
}

function get_results() {
	local iops bw stdev
	local p90_lat p99_lat p99_99_lat
	local mean_slat mean_clat
	local reads_pct
	local writes_pct

	reads_pct=$(bc "$1 / 100")
	writes_pct=$(bc "1 - $reads_pct")

	iops=$(jq -r '.jobs[] | .read.iops + .write.iops' $TMP_RESULT_FILE)
	bw=$(jq -r ".jobs[] | (.read.bw + .write.bw)" $TMP_RESULT_FILE)
	mean_lat=$(jq -r ".jobs[] | (.read.lat_ns.mean * $reads_pct + .write.lat_ns.mean * $writes_pct)/1000" $TMP_RESULT_FILE)
	p90_lat=$(jq -r ".jobs[] | (.read.clat_ns.percentile.\"90.000000\"  // 0 * $reads_pct + .write.clat_ns.percentile.\"90.000000\" // 0 * $writes_pct)/1000" $TMP_RESULT_FILE)
	p99_lat=$(jq -r ".jobs[] | (.read.clat_ns.percentile.\"99.000000\"  // 0 * $reads_pct + .write.clat_ns.percentile.\"99.000000\" // 0 * $writes_pct)/1000" $TMP_RESULT_FILE)
	p99_99_lat=$(jq -r ".jobs[] | (.read.clat_ns.percentile.\"99.990000\" // 0 * $reads_pct + .write.clat_ns.percentile.\"99.990000\" // 0 * $writes_pct)/1000" $TMP_RESULT_FILE)
	stdev=$(jq -r ".jobs[] | (.read.clat_ns.stddev * $reads_pct + .write.clat_ns.stddev * $writes_pct)/1000" $TMP_RESULT_FILE)
	mean_slat=$(jq -r ".jobs[] | (.read.slat_ns.mean * $reads_pct + .write.slat_ns.mean * $writes_pct)/1000" $TMP_RESULT_FILE)
	mean_clat=$(jq -r ".jobs[] | (.read.clat_ns.mean * $reads_pct + .write.clat_ns.mean * $writes_pct)/1000" $TMP_RESULT_FILE)

	echo "$iops $bw $mean_lat $p90_lat $p99_lat $p99_99_lat $stdev $mean_slat $mean_clat"
}

function get_bdevperf_results() {
	local iops
	local bw_MBs
	read -r iops bw_MBs <<< $(grep Total $TMP_RESULT_FILE | tr -s " " | awk -F ":| " '{print $5" "$7}')
	echo "$iops $(bc "$bw_MBs * 1024")"
}

function get_nvmeperf_results() {
	local iops
	local bw_MBs
	local mean_lat_usec
	local max_lat_usec
	local min_lat_usec

	read -r iops bw_MBs mean_lat_usec min_lat_usec max_lat_usec <<< $(tr -s " " < $TMP_RESULT_FILE | grep -oP "(?<=Total : )(.*+)")
	echo "$iops $(bc "$bw_MBs * 1024") $mean_lat_usec $min_lat_usec $max_lat_usec"
}

function run_spdk_nvme_fio() {
	local plugin=$1
	echo "** Running fio test, this can take a while, depending on the run-time and ramp-time setting."
	if [[ "$plugin" = "spdk-plugin-nvme" ]]; then
		LD_PRELOAD=$plugin_dir/spdk_nvme $FIO_BIN $testdir/config.fio --output-format=json "${@:2}" --ioengine=spdk
	elif [[ "$plugin" = "spdk-plugin-bdev" ]]; then
		LD_PRELOAD=$plugin_dir/spdk_bdev $FIO_BIN $testdir/config.fio --output-format=json "${@:2}" --ioengine=spdk_bdev --spdk_json_conf=$testdir/bdev.conf --spdk_mem=4096
	fi

	sleep 1
}

function run_nvme_fio() {
	echo "** Running fio test, this can take a while, depending on the run-time and ramp-time setting."
	$FIO_BIN $testdir/config.fio --output-format=json "$@"
	sleep 1
}

function run_bdevperf() {
	local bdevperf_rpc
	local bdevperf_pid
	local rpc_socket
	local bpf_script_cmd
	local bpf_script_pid
	local bpf_app_pid
	local main_core_param=""

	bdevperf_rpc="$rootdir/test/bdev/bdevperf/bdevperf.py"
	rpc_socket="/var/tmp/spdk.sock"

	if [[ -n $MAIN_CORE ]]; then
		main_core_param="-p ${MAIN_CORE}"
	fi

	echo "** Running bdevperf test, this can take a while, depending on the run-time setting."
	$bdevperf_dir/bdevperf --json $testdir/bdev.conf -q $IODEPTH -o $BLK_SIZE -w $RW -M $MIX -t $RUNTIME -m "[$CPUS_ALLOWED]" -r "$rpc_socket" $main_core_param -z &
	bdevperf_pid=$!
	waitforlisten $bdevperf_pid

	if [[ ${#BPFTRACES[@]} -gt 0 ]]; then
		echo "INFO: Enabling BPF Traces ${BPFTRACES[*]}"
		bpf_script_cmd=("$rootdir/scripts/bpftrace.sh")
		bpf_script_cmd+=("$bdevperf_pid")
		for trace in "${BPFTRACES[@]}"; do
			bpf_script_cmd+=("$rootdir/scripts/bpf/$trace")
		done

		BPF_OUTFILE=$TMP_BPF_FILE "${bpf_script_cmd[@]}" &
		bpf_script_pid=$!
		sleep 3
	fi

	PYTHONPATH=$PYTHONPATH:$rootdir/scripts $bdevperf_rpc -s "$rpc_socket" -t $((RUNTIME + 10)) perform_tests

	# Using "-z" option causes bdevperf to NOT exit automatically after running the test,
	# so we need to stop it ourselves.
	kill -s SIGINT $bdevperf_pid
	wait $bdevperf_pid

	if ((bpf_script_pid)); then
		wait $bpf_script_pid
	fi
	sleep 1
}

function run_nvmeperf() {
	# Prepare -r argument string for nvme perf command
	local r_opt
	local disks

	# Limit the number of disks to $1 if needed
	disks=($(get_disks nvme))
	disks=("${disks[@]:0:$1}")
	r_opt=$(printf -- ' -r "trtype:PCIe traddr:%s"' "${disks[@]}")

	echo "** Running nvme perf test, this can take a while, depending on the run-time setting."

	# Run command in separate shell as this solves quoting issues related to r_opt var
	$SHELL -c "$nvmeperf_dir/perf $r_opt -q $IODEPTH -o $BLK_SIZE -w $RW -M $MIX -t $RUNTIME -c [$CPUS_ALLOWED]"
	sleep 1
}

function wait_for_nvme_reload() {
	local nvmes=$1

	shopt -s extglob
	for disk in $nvmes; do
		cmd="ls /sys/block/$disk/queue/*@(iostats|rq_affinity|nomerges|io_poll_delay)*"
		until $cmd 2> /dev/null; do
			echo "Waiting for full nvme driver reload..."
			sleep 0.5
		done
	done
	shopt -q extglob
}

function verify_disk_number() {
	# Check if we have appropriate number of disks to carry out the test
	disks=($(get_disks $PLUGIN))
	if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
		DISKNO=${#disks[@]}
	elif [[ $DISKNO -gt ${#disks[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
		echo "error: Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#disks[@]})"
		false
	fi
}
