#!/usr/bin/env bash

set -xe
PLUGIN_DIR_NVME=$ROOTDIR/examples/nvme/fio_plugin
PLUGIN_DIR_BDEV=$ROOTDIR/examples/bdev/fio_plugin
. $ROOTDIR/scripts/common.sh || exit 1
. $ROOTDIR/test/common/autotest_common.sh
NVME_FIO_RESULTS=$BASE_DIR/result.json

function get_disks(){
	if [ "$1" = "nvme" ]; then
		for bdf in $(iter_pci_class_code 01 08 02); do
			driver=`grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}'`
			if [ "$driver" = "vfio-pci" ] || [ "$driver" = "uio_pci_generic" ]; then
				echo "$bdf"
			fi
		done
	elif [ "$1" = "bdev" ]; then
		bdevs=$(discover_bdevs $ROOTDIR $BASE_DIR/bdev.conf)
		echo $(jq -r '.[].name' <<< $bdevs)
	fi
}

function create_fio_filename(){
	local disk_no=$1
	local plugin=$2
	local devs=($3)
	local filename=""
	local i
	if [ "$plugin" = "nvme" ]; then
		for (( i=0; i < $disk_no; i++ ))
		do
			dev_name='trtype=PCIe traddr='${devs[i]//:/.}' ns=1'
			filename+=$(printf %s":" "$dev_name")
		done
	elif [ "$plugin" = "bdev" ]; then
		for (( i=0; i < $disk_no; i++ ))
		do
			filename+=$(printf %s":" "${devs[$i]}")
		done
	fi

	echo $filename
}

function preconditioning(){
	local dev_name=""
	local filename=""
	local i
	for (( i=0; i < $DISKNO; i++ ))
	do
		dev_name='trtype=PCIe traddr='${disks[i]//:/.}' ns=1'
		filename+=$(printf %s":" "$dev_name")
	done
	run_spdk_nvme_fio "nvme" --filename="$filename" --size=100% --loops=2 --bs=1M\
		--rw=write --iodepth=32
}

function get_results(){
	#If current workload is 70% READ and 30% WRITE, latencies are weighted average values of writes and reads
	case "$1" in
		iops)
		iops=$(cat $NVME_FIO_RESULTS | jq -r '.jobs[] | (.read.iops + .write.iops)')
		iops=${iops%.*}
		echo $iops
		;;
		mean_lat)
		if [ "$2" = "70" ]; then
			mean_lat=$(cat $NVME_FIO_RESULTS | jq -r '.jobs[] | (.read.clat_ns.mean*0.3 + .write.clat_ns.mean*0.7)')
		else
			mean_lat=$(cat $NVME_FIO_RESULTS | jq -r '.jobs[] | (.read.clat_ns.mean + .write.clat_ns.mean)')
		fi
		mean_lat=${mean_lat%.*}
		echo $(( $mean_lat/1000 ))
		;;
		p99_lat)
		if [ "$2" = "70" ]; then
			p99_lat=$(cat $NVME_FIO_RESULTS | jq -r '.jobs[] | (.read.clat_ns.percentile."99.000000"*0.3 + .write.clat_ns.percentile."99.000000"*0.7)')
		else
			p99_lat=$(cat $NVME_FIO_RESULTS | jq -r '.jobs[] | (.read.clat_ns.percentile."99.000000" + .write.clat_ns.percentile."99.000000")')
		fi
		p99_lat=${p99_lat%.*}
		echo $(( $p99_lat/1000 ))
		;;
		p99_99_lat)
		if [ "$2" = "70" ]; then
			p99_99_lat=$(cat $NVME_FIO_RESULTS | jq -r '.jobs[] | (.read.clat_ns.percentile."99.990000"*0.3 + .write.clat_ns.percentile."99.990000"*0.7)')
		else
			p99_99_lat=$(cat $NVME_FIO_RESULTS | jq -r '.jobs[] | (.read.clat_ns.percentile."99.990000" + .write.clat_ns.percentile."99.990000")')
		fi
		p99_99_lat=${p99_99_lat%.*}
		echo $(( $p99_99_lat/1000 ))
		;;
		stdev)
		if [ "$2" = "70" ]; then
			stdev=$(cat $NVME_FIO_RESULTS | jq -r '.jobs[] | (.read.clat_ns.stddev*0.3 + .write.clat_ns.stddev*0.7)')
		else
			stdev=$(cat $NVME_FIO_RESULTS | jq -r '.jobs[] | (.read.clat_ns.stddev + .write.clat_ns.stddev)')
		fi
		stdev=${stdev%.*}
		echo $(( $stdev/1000 ))
		;;
	esac
}

function run_spdk_nvme_fio(){
	local plugin=$1
	if [ "$plugin" = "nvme" ]; then
		LD_PRELOAD=$PLUGIN_DIR_NVME/fio_plugin $FIO_BIN $BASE_DIR/config.fio --output-format=json\
		 "${@:2}" --ioengine=spdk
	elif [ "$plugin" = "bdev" ]; then
		LD_PRELOAD=$PLUGIN_DIR_BDEV/fio_plugin $FIO_BIN $BASE_DIR/config.fio --output-format=json\
		 "${@:2}" --ioengine=spdk_bdev --spdk_conf=$BASE_DIR/bdev.conf --spdk_mem=1024
	fi

	sleep 1
}

mkdir -p $BASE_DIR/results
date="$(date +'%m_%d_%Y_%H%M%S')"
disks=($(get_disks nvme))
if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
	DISKNO=${#disks[@]}
elif [[ $DISKNO -gt ${#disks[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
	echo "error: Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#disks[@]})"
	exit 1
fi
