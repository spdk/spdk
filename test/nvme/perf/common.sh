#!/usr/bin/env bash

set -xe
PLUGIN_DIR_NVME=$ROOTDIR/examples/nvme/fio_plugin
PLUGIN_DIR_BDEV=$ROOTDIR/examples/bdev/fio_plugin
. $ROOTDIR/scripts/common.sh || exit 1
. $ROOTDIR/test/common/autotest_common.sh
NVME_FIO_RESULTS=$BASE_DIR/result.json
BASE_DIR=$(readlink -f $(dirname $0))
ROOTDIR=$(readlink -f $BASE_DIR/../../..)

PRECONDITIONING=true
FIO_BIN="/usr/src/fio/fio"
RUNTIME=600
PLUGIN="nvme"
RAMP_TIME=30
BLK_SIZE=4096
RW=randrw
MIX=(100 70 0)
TYPE=("randread" "randrw" "randwrite")
IODEPTH=(256 256 32)
DISKNO=1
CPU_MASKS=(0x02 0x06 0x1E 0x7E 0x1FE 0x7FE)
CPUs=(1 2 4 6 8 10)

function get_disks(){
	if [ "$1" = "nvme" ]; then
		for bdf in $(iter_pci_class_code 01 08 02); do
			driver=`grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}'`
			if [ "$driver" = "vfio-pci" ] || [ "$driver" = "uio_pci_generic" ]; then
				echo "$bdf"
			fi
		done
	elif [ "$1" = "bdev" ]; then
		local bdevs=$(discover_bdevs $ROOTDIR $BASE_DIR/bdev.conf)
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
	elif [ "$plugin" = "kernel" ]; then
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
	local i
	for (( i=0; i < $DISKNO; i++ ))
	do
		if [ -b "/dev/nvme0n1" ]; then
			filename+=$(printf /dev/nvme%sn1: $i)
		else
			dev_name='trtype=PCIe traddr='${disks[i]//:/.}' ns=1'
			filename+=$(printf %s":" "$dev_name")
		fi
	done
	if [ -b "/dev/nvme0n1" ]; then
		run_nvme_fio "nvme" --filename="$filename" --size=100% --loops=2 --bs=1M\
		--rw=write --iodepth=32 -ioengine=libaio
	else
		run_spdk_nvme_fio "nvme" --filename="$filename" --size=100% --loops=2 --bs=1M\
		--rw=write --iodepth=32
	fi
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

function run_nvme_fio(){
	$FIO_BIN $BASE_DIR/config.fio --output-format=json --cpus_allowed=28 "$@"
	sleep 1
}

function usage()
{
	set +x
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Run Nvme PMD/BDEV performance test. Change options for easier debug and setup configuration"
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help                Print help and exit"
	echo "    --run-time=TIME[s]    Tell fio to terminate processing after the specified period of time. [default=$RUNTIME]"
	echo "    --ramp-time=TIME[s]   Fio will run the specified workload for this amount of time before logging any performance numbers. [default=$RAMP_TIME]"
	echo "    --fio-bin=PATH        Path to fio binary. [default=$FIO_BIN]"
	echo "    --fio-plugin=STR      Use bdev or nvme fio_plugin. [default=$PLUGIN]"
	echo "    --disk-no=INT,ALL     Number of disks to test on, if =ALL then test on all found disk. [default=$DISKNO]"
	echo "    --no-preconditioning  Skip preconditioning"
	set -x
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0; exit 0 ;;
			run-time=*) RUNTIME="${OPTARG#*=}" ;;
			ramp-time=*) RAMP_TIME="${OPTARG#*=}" ;;
			fio-bin=*) FIO_BIN="${OPTARG#*=}" ;;
			disk-no=*) DISKNO="${OPTARG#*=}" ;;
			fio-plugin=*) PLUGIN="${OPTARG#*=}" ;;
			no-preconditioning) PRECONDITIONING=false ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

trap 'rm -f *.state $NVME_FIO_RESULTS $BASE_DIR/bdev.conf; print_backtrace' ERR SIGTERM SIGABRT
mkdir -p $BASE_DIR/results
date="$(date +'%m_%d_%Y_%H%M%S')"
disks=($(get_disks nvme))
if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
	DISKNO=${#disks[@]}
elif [[ $DISKNO -gt ${#disks[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
	echo "error: Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#disks[@]})"
	exit 1
fi
