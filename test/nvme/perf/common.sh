#!/usr/bin/env bash

set -xe
PLUGIN_DIR_NVME=$ROOTDIR/examples/nvme/fio_plugin
PLUGIN_DIR_BDEV=$ROOTDIR/examples/bdev/fio_plugin
BASE_DIR=$(readlink -f $(dirname $0))
ROOTDIR=$(readlink -f $BASE_DIR/../../..)
. $ROOTDIR/scripts/common.sh || exit 1
. $ROOTDIR/test/common/autotest_common.sh
NVME_FIO_RESULTS=$BASE_DIR/result.json

PRECONDITIONING=true
FIO_BIN="/usr/src/fio/fio"
RUNTIME=600
PLUGIN="nvme"
RAMP_TIME=30
BLK_SIZE=4096
RW=randrw
MIX=100
TYPE=("randread" "randrw" "randwrite")
IODEPTH=256
DISKNO=1
PREFER_NUMA=0

function get_numa_node(){
	local plugin=$1
	local disks=$2
	if [ "$plugin" = "nvme" ]; then
		for bdf in $disks; do
			local driver=`grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}'`
			if [ "$driver" = "vfio-pci" ] || [ "$driver" = "uio_pci_generic" ]; then
				echo $(cat /sys/bus/pci/devices/$bdf/numa_node)
			fi
		done
	elif [ "$plugin" = "bdev" ]; then
		local bdevs=$(discover_bdevs $ROOTDIR $BASE_DIR/bdev.conf)
		for name in $disks; do
			local bdev_bdf=$(jq -r ".[] | select(.name==\"$name\").driver_specific.nvme.pci_address" <<< $bdevs)
			echo $(cat /sys/bus/pci/devices/$bdev_bdf/numa_node)
		done
	fi
}

function get_disks(){
	local plugin=$1
	if [ "$plugin" = "nvme" ]; then
		for bdf in $(iter_pci_class_code 01 08 02); do
			driver=`grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}'`
			if [ "$driver" = "vfio-pci" ] || [ "$driver" = "uio_pci_generic" ]; then
				echo "$bdf"
			fi
		done
	elif [ "$plugin" = "bdev" ]; then
		local bdevs=$(discover_bdevs $ROOTDIR $BASE_DIR/bdev.conf)
		echo $(jq -r '.[].name' <<< $bdevs)
	fi
}

function get_disks_on_numa(){
	local devs=($1)
	local numas=($2)
	local numa_no=$3
	local disks_on_numa=""
	local i

	for (( i=0; i<${#devs[@]}; i++ ))
	do
		if [ ${numas[$i]} = $numa_no ]; then
			disks_on_numa=$(($disks_on_numa+1))
		fi
	done
	echo $disks_on_numa
}

function create_fio_filename(){
	local disk_no=$1
	local plugin=$2
	local devs=($3)
	local numas=($4)
	local total_disks=${#devs[@]}
	local filename=""
	local i
	local j=0
	local n=0

	n0=$(get_disks_on_numa "$3" "$4" "0")
	n1=$(get_disks_on_numa "$3" "$4" "1")
	case "$PREFER_NUMA" in
		0)
		if [ "$n0" -ge "$disk_no" ]; then
			disks_on_numa_0=$disk_no
			disks_on_numa_1=0
		else
			disks_on_numa_1=$(($disk_no-$n0))
			disks_on_numa_0=$n0
		fi
		;;
		1)
		if [ "$n1" -ge "$disk_no" ]; then
			disks_on_numa_1=$disk_no
			disks_on_numa_0=0
		else
			disks_on_numa_0=$(($disk_no-$n1))
			disks_on_numa_1=$n1
		fi
		;;
		both)
		if [ $disk_no = "1" ]; then
			disks_on_numa_0=1
			disks_on_numa_1=0
		elif [ "$n0" -lt "$(($disk_no/2))" ]; then
			disks_on_numa_0=$n0
			disks_on_numa_1=$(($disk_no-$n0))
		elif [ "$n1" -lt "$(($disk_no/2))" ]; then
			disks_on_numa_1=$n1
			disks_on_numa_0=$(($disk_no-$n1))
		else
			disks_on_numa_0=$(($disk_no/2))
			disks_on_numa_1=$(($disk_no/2))
		fi
		;;
	esac

	n=0
	j=0
	while [ "$n" -lt "$disks_on_numa_0" ]; do
		if [ ${numas[$j]} = "0" ]; then
			if [ "$plugin" = "nvme" ]; then
				dev_name='trtype=PCIe traddr='${devs[$j]//:/.}' ns=1'
				filename+=$(printf %s":" "$dev_name")
			elif [ "$plugin" = "bdev" ]; then
				filename+=$(printf %s":" "${devs[$j]}")
			fi
			n=$(($n+1))
		fi
		j=$(($j+1))
		if [ "$j" -gt "$total_disks" ]; then
			break
		fi
	done
	n=0
	j=0
	while [ "$n" -lt "$disks_on_numa_1" ]; do
		if [ ${numas[$j]} = "1" ]; then
			if [ "$plugin" = "nvme" ]; then
				dev_name='trtype=PCIe traddr='${devs[$j]//:/.}' ns=1'
				filename+=$(printf %s":" "$dev_name")
			elif [ "$plugin" = "bdev" ]; then
				filename+=$(printf %s":" "${devs[$j]}")
			fi
			n=$(($n+1))
		fi
		j=$(($j+1))
		if [ "$j" -gt "$total_disks" ]; then
			break
		fi
	done

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
	local reads_pct=$2
	local writes_pct=$((100-$2))

	case "$1" in
		iops)
		iops=$(cat $NVME_FIO_RESULTS | jq -r '.jobs[] | (.read.iops + .write.iops)')
		iops=${iops%.*}
		echo $iops
		;;
		mean_lat)
		mean_lat=$(cat $NVME_FIO_RESULTS | jq -r ".jobs[] | (.read.lat_ns.mean * $reads_pct + .write.lat_ns.mean * $writes_pct)")
		mean_lat=${mean_lat%.*}
		echo $(( $mean_lat/100000 ))
		;;
		p99_lat)
			p99_lat=$(cat $NVME_FIO_RESULTS | jq -r ".jobs[] | (.read.clat_ns.percentile.\"99.000000\" * $reads_pct + .write.clat_ns.percentile.\"99.000000\" * $writes_pct)")
		p99_lat=${p99_lat%.*}
		echo $(( $p99_lat/100000 ))
		;;
		p99_99_lat)
		p99_99_lat=$(cat $NVME_FIO_RESULTS | jq -r ".jobs[] | (.read.clat_ns.percentile.\"99.990000\" * $reads_pct + .write.clat_ns.percentile.\"99.990000\" * $writes_pct)")
		p99_99_lat=${p99_99_lat%.*}
		echo $(( $p99_99_lat/100000 ))
		;;
		stdev)
		stdev=$(cat $NVME_FIO_RESULTS | jq -r ".jobs[] | (.read.clat_ns.stddev * $reads_pct + .write.clat_ns.stddev * $writes_pct)")
		stdev=${stdev%.*}
		echo $(( $stdev/100000 ))
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
		 "${@:2}" --ioengine=spdk_bdev --spdk_conf=$BASE_DIR/bdev.conf --spdk_mem=4096
	fi

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
	echo "    --rwmixread=INT       Percentage of a mixed workload that should be reads. [default=$MIX]"
	echo "    --iodepth=INT         Number of I/O units to keep in flight against the file. [default=$IODEPTH]"
	echo "    --prefer-numa=OPT     Prefer numa node 0, 1 or both. [default=$PREFER_NUMA]"
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
			rwmixread=*) MIX="${OPTARG#*=}" ;;
			iodepth=*) IODEPTH="${OPTARG#*=}" ;;
			no-preconditioning) PRECONDITIONING=false ;;
			prefer-numa=*) PREFER_NUMA="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

trap 'rm -f *.state $BASE_DIR/bdev.conf; print_backtrace' ERR SIGTERM SIGABRT
mkdir -p $BASE_DIR/results
date="$(date +'%m_%d_%Y_%H%M%S')"
disks=($(get_disks nvme))
if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
	DISKNO=${#disks[@]}
elif [[ $DISKNO -gt ${#disks[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
	echo "error: Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#disks[@]})"
	exit 1
fi
