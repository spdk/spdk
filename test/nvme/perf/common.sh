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
CPUS=(1 2 4 6 8 10)

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

function create_fio_filename(){
	local disk_no=$1
	local plugin=$2
	local devs=($3)
	local numas=($4)
	local total_disks=${#devs[@]}
	local filename=""
	local disks_per_numa=$(( $disk_no/2 ))
	local i
	local j=0
	local n=0

	#if only one disk is used, don't care which numa is used
	if [ $disk_no = "1" ]; then
		if [ "$plugin" = "nvme" ]; then
			dev_name='trtype=PCIe traddr='${devs[0]//:/.}' ns=1'
			filename=$(printf %s":" "$dev_name")
		elif [ "$plugin" = "bdev" ]; then
			filename+=$(printf %s":" "${devs[0]}")
		fi
		echo $filename
		return
	fi

	#use equal amount of disks on numa node 0 and 1
	for (( i=0; i<2; i++ ))
	do
		n=0
		j=0
		while [ "$n" -lt "$disks_per_numa" ]; do
			if [ ${numas[$j]} = "$i" ]; then
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
				echo "error: There is no sufficient disks on numa node $i"
				exit 1
				break
			fi
		done
	done
	echo $filename
}

function create_cpu_allowed(){
	local no_cpu=$1
	local cpus_per_numa=$(( $no_cpu/2 ))
	local i
	local j
	local total_cpu=$(nproc)

	#core 0 is used by fio_plugin
	#if only one core is used, core 1
	if [ $no_cpu = "1" ]; then
		echo 1
		return
	fi

	#use equal amount of cores on numa node 0 and 1
	for (( i=0; i<2; i++ ))
	do
		n=0
		j=1
		while [ "$n" -lt "$cpus_per_numa" ]; do
			if [ "$(lscpu -p=cpu,node | grep "^$j\b" | awk -F ',' '{print $2}')" = "$i" ]; then
				cpu_allowed+=$(printf %s"," "$j")
				n=$(($n+1))
			fi
			j=$(($j+1))
			if [ "$j" -gt "$total_cpu" ]; then
				echo "error: There is no sufficient cores on numa node $i"
				exit 1
				break
			fi
		done
	done
	echo "$cpu_allowed"
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
