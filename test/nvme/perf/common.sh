#!/usr/bin/env bash

set -xe
PLUGIN_DIR_NVME=$rootdir/examples/nvme/fio_plugin
PLUGIN_DIR_BDEV=$rootdir/examples/bdev/fio_plugin
. $rootdir/scripts/common.sh || exit 1
. $rootdir/test/common/autotest_common.sh
nvme_fio_results=$BASE_DIR/result.json

mkdir -p $BASE_DIR/results
date="$(date +'%m_%d_%Y_%H%M%S')"
disks=($(iter_pci_class_code 01 08 02))
if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
	DISKNO=${#disks[@]}
elif [[ $DISKNO -gt ${#disks[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
	echo "error: Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#disks[@]})"
	exit 1
fi

function get_disks(){
	if [ "$1" = "nvme" ]; then
		echo $(iter_pci_class_code 01 08 02)
	elif [ "$1" = "bdev" ]; then
		local bdevs=$(discover_bdevs $rootdir $BASE_DIR/bdev.conf)
		echo $(jq -r '.[].name' <<< $bdevs)
	fi
}

function get_filename(){
	local disk_no=$1
	local devs=($3)
	local filename=""
	if [ "$2" = "nvme" ]; then
		for (( i=0; i < $disk_no; i++ ))
		do
			dev_name='trtype=PCIe traddr='${devs[i]//:/.}' ns=1'
			filename+=$(printf %s":" "$dev_name")
		done
	elif [ "$2" = "bdev" ]; then
		for (( i=0; i < $disk_no; i++ ))
		do
			filename+=$(printf %s":" "${devs[$i]}")
		done
	elif [ "$2" = "kernel" ]; then
		for (( i=0; i < $disk_no; i++ ))
		do
			filename+=$(printf /dev/nvme%sn1: $i)
		done
	fi

	echo $filename
}

function preconditioning(){
	local dev_name=""
	local filename=""
	if [ -b "/dev/nvme0n1" ]; then
		fio_func=run_nvme_fio
	else
		fio_func=run_spdk_nvme_fio
	fi

	for (( i=0; i < $DISKNO; i++ ))
	do
		if [ -b "/dev/nvme0n1" ]; then
			filename+=$(printf /dev/nvme%sn1: $i)
		else
			dev_name='trtype=PCIe traddr='${disks[i]//:/.}' ns=1'
			filename+=$(printf %s":" "$dev_name")
		fi
	done
	if [ "$1" = "--write" ]; then
		$fio_func --filename="$filename" "--runtime=5400" "--bs=4096"\
		 "--rw=randwrite" "--iodepth=32"
	else
		$fio_func --filename="$filename" "--runtime=1200" "--bs=131072"\
		 "--rw=write" "--iodepth=32"
	fi
}

function get_value(){
	#If current workload is 70% READ and 30% WRITE, latencies are average values of writes and reads
	case "$1" in
		iops) local iops=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.iops + .write.iops)')
		iops=${iops%.*}
		echo $iops
		;;
		mean_lat) local mean_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.mean + .write.clat_ns.mean)')
		mean_lat=${mean_lat%.*}
		if [ "$2" = "70" ]; then
			echo $(($mean_lat/2))
		else
			echo $mean_lat
		fi
		;;
		p99_lat) local p99_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.percentile."99.000000" + .write.clat_ns.percentile."99.000000")')
		p99_lat=${p99_lat%.*}
		if [ "$2" = "70" ]; then
			echo $(($p99_lat/2))
		else
			echo $p99_lat
		fi
		;;
		p99_99_lat) local p99_99_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.percentile."99.990000" + .write.clat_ns.percentile."99.990000")')
		p99_99_lat=${p99_99_lat%.*}
		if [ "$2" = "70" ]; then
			echo $(($p99_99_lat/2))
		else
			echo $p99_99_lat
		fi
		;;
		stdev) local stdev=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.stddev + .write.clat_ns.stddev)')
		stdev=${stdev%.*}
		if [ "$2" = "70" ]; then
			echo $(($stdev/2))
		else
			echo $stdev
		fi
		;;
	esac
}

function run_spdk_nvme_fio(){
	if [ "$PLUGIN" = "nvme" ]; then
		LD_PRELOAD=$PLUGIN_DIR_NVME/fio_plugin $FIO_BIN $BASE_DIR/config.fio --output-format=json\
		 "$@" --ioengine=$PLUGIN_DIR_NVME/fio_plugin
	elif [ "$PLUGIN" = "bdev" ]; then
		LD_PRELOAD=$PLUGIN_DIR_BDEV/fio_plugin $FIO_BIN $BASE_DIR/config.fio --output-format=json\
		 "$@" --ioengine=spdk_bdev --spdk_conf=$BASE_DIR/bdev.conf --spdk_mem=1024
	fi

	sleep 1
}

function run_nvme_fio(){
	$FIO_BIN $BASE_DIR/config.fio --output-format=json --ioengine=pvsync2 --hipri=100 --cpus_allowed=28 "$@"
	sleep 1
}
