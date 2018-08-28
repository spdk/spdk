#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $BASE_DIR/../../..)

PLUGIN_DIR_NVME=$rootdir/examples/nvme/fio_plugin
. $rootdir/scripts/common.sh || exit 1
. $rootdir/test/common/autotest_common.sh
preconditioning=true
FIO_BIN="/usr/src/fio/fio"
nvme_fio_results=$BASE_DIR/result.json

RUNTIME=600
RAMP_TIME=30
BLK_SIZE=4096
RW=randrw
MIX=(100 0 70)
TYPE=("read" "write" "mix")
IODEPTH=(256 32 256)
DISKNO=1
CPU_MASKS=(0x02 0x06 0x1E 0x7E 0x1FE 0x7FE)
CPUs=(1 2 4 6 8 10)

function usage()
{
	set +x
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Run Nvme PMD performance test. Change options for easier debug and setup configuration"
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help                Print help and exit"
	echo "    --runtime=TIME        Tell fio to terminate processing after the specified period of time. [default=$RUNTIME]"
	echo "    --ramp_time=TIME      Fio will run the specified workload for this amount of time before logging any performance numbers. [default=$RAMP_TIME]"
	echo "    --fiobin=PATH         Path to fio binary. [default=$FIO_BIN]"
	echo "    --disk_no=INT,ALL     Number of disks to test on, if =ALL then test on all found disk. [default=$DISKNO]"
	echo "    --no-preconditioning  Skip preconditioning. [deault=Disabled]"
	set -x
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0; exit 0 ;;
			runtime=*) RUNTIME="${OPTARG#*=}" ;;
			ramp_time=*) RAMP_TIME="${OPTARG#*=}" ;;
			fiobin=*) FIO_BIN="${OPTARG#*=}" ;;
			disk_no=*) DISKNO="${OPTARG#*=}" ;;
			no-preconditioning) preconditioning=false ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

function run_spdk_nvme_fio(){
LD_PRELOAD=$PLUGIN_DIR_NVME/fio_plugin $FIO_BIN $BASE_DIR/config.fio --output-format=json\
	 "$@" --ioengine=$PLUGIN_DIR_NVME/fio_plugin
# Sleep 1 resolves some issues with binding/unbinding nvme
sleep 1
}

date="$(date +'%m_%d_%Y_%H%M%S')"
mkdir -p $BASE_DIR/results
trap 'rm -f *.state $nvme_fio_results; print_backtrace' ERR SIGTERM SIGABRT

disks=($(iter_pci_class_code 01 08 02))
if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
	DISKNO=${#disks[@]}
elif [[ $DISKNO -gt ${#disks[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
	error "Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#disks[@]})"
fi


function preconditioning(){
	dev_name=""
	filename=""
	for (( i=0; i < $DISKNO; i++ ))
	do
		dev_name='trtype=PCIe traddr='${disks[i]//:/.}' ns=1'
		filename+=$(printf %s":" "$dev_name")
	done

	if [ "$1" = "--write" ]; then
		run_spdk_nvme_fio --filename="$filename" "--runtime=5400" "--bs=4096"\
		 "--rw=randwrite" "--iodepth=32"
	else
		run_spdk_nvme_fio --filename="$filename" "--runtime=1200" "--bs=131072"\
		 "--rw=write" "--iodepth=32"
	fi
}


#100% READ, 100% WRITE, 70% READ and 30% WRITE
for (( i=0; i < 3; i++ ))
do
	if [ "${MIX[$i]}" = "0" ] && $preconditioning; then
		preconditioning --write
	elif $preconditioning; then
		preconditioning
	fi
	echo "CPUs;iops;avg_lat[u];p99[u];p99.99[u];stdev[u]" >> $BASE_DIR/results/tc2_perf_results_${TYPE[$i]}_${date}.csv
	# Run tests for 1, 2, 4, 6 ,8 ,10 cpu cores
	for (( j=0; j < 6; j++ ))
	do
		CPUMASK=${CPU_MASKS[$j]}
		max_iops=0
		max_disk=0
		count=0
		avg_mean_iops=0
		avg_mean_lat=0
		#Each Workload repeat 3 times
		for (( k=0; k < 3; k++ ))
		do
			# Run test with multiple disk configuration once and find disk number where cpus are saturated, next do runs do with
			# only that disk number.
			if [ "$max_iops" = "0" ]; then
				#Start with $DISKNO disks and remove 2 disks for each run
				for (( l=$DISKNO; l >= 1; l-=2 ))
				do
					dev_name=""
					filename=""
					for (( m=0; m < $l; m++ ))
					do
						dev_name='trtype=PCIe traddr='${disks[m]//:/.}' ns=1'
						filename+=$(printf %s":" "$dev_name")
					done

					run_spdk_nvme_fio --filename="$filename" "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
					 "--rw=$RW" "--rwmixread=${MIX[$i]}" "--iodepth=${IODEPTH[$i]}" "--cpumask=$CPUMASK" "--output=$nvme_fio_results"
					iops=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.iops + .write.iops)')
					iops=${iops%.*}
					#search the number of disks where cpus are saturated
					if [[ $max_ops < $iops ]]; then
						max_iops=$iops
						max_disk=$l
						mean_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.mean + .write.clat_ns.mean)')
						mean_lat=${mean_lat%.*}
						p99_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.percentile."99.000000" + .write.clat_ns.percentile."99.000000")')
						p99_lat=${p99_lat%.*}
						p99_99_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.percentile."99.990000" + .write.clat_ns.percentile."99.990000")')
						p99_99_lat=${p99_99_lat%.*}
						stdev=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.stddev + .write.clat_ns.stddev)')
						stdev=${stdev%.*}
					fi
				done
				iops=$max_iops
			else
				dev_name=""
				filename=""
				for (( m=0; m < $max_disk; m++ ))
				do
					dev_name='trtype=PCIe traddr='${disks[m]//:/.}' ns=1'
					filename+=$(printf %s":" "$dev_name")
				done
				run_spdk_nvme_fio --filename="$filename" "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
				 "--rw=$RW" "--rwmixread=${MIX[$i]}" "--iodepth=${IODEPTH[$i]}" "--cpumask=$CPUMASK" "--output=$nvme_fio_results"
				iops=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.iops + .write.iops)')
				iops=${iops%.*}
				mean_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.mean + .write.clat_ns.mean)')
				mean_lat=${mean_lat%.*}
				p99_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.percentile."99.000000" + .write.clat_ns.percentile."99.000000")')
				p99_lat=${p99_lat%.*}
				p99_99_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.percentile."99.990000" + .write.clat_ns.percentile."99.990000")')
				p99_99_lat=${p99_99_lat%.*}
				stdev=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.stddev + .write.clat_ns.stddev)')
				stdev=${stdev%.*}
			fi

			# In 70/30 case latencies are means of read and write latencies
			if [ "${MIX[$i]}" = "70" ]; then
				mean_lat=$(($mean_lat/2))
				p99_lat=$(($p99_lat/2))
				p99_99_lat=$(($p99_99_lat/2))
				stdev=$(($stdev/2))
			fi

			count=$(($count + 1))
			avg_mean_iops=$(($avg_mean_iops + $iops))
			avg_mean_lat=$(($avg_mean_lat + $mean_lat))
			avg_p99_lat=$(($avg_p99_lat + $p99_lat))
			avg_p99_99_lat=$(($avg_p99_99_lat + $p99_99_lat))
			avg_stdev=$(($avg_stdev + $stdev))
		done
		avg_mean_iops=$(($avg_mean_iops / $count))
		avg_mean_lat=$(($avg_mean_lat / $count))
		avg_p99_lat=$(($avg_p99_lat / $count))
		avg_p99_99_lat=$(($avg_p99_99_lat / $count))
		avg_stdev=$(($avg_stdev / $count))
		printf "%s,%s,%s,%s,%s,%s\n" ${CPUs[j]} $avg_mean_iops $(($avg_mean_lat / 1000)) $(($avg_p99_lat / 1000))\
		 $(($avg_p99_99_lat / 1000)) $(($avg_stdev / 1000)) >> $BASE_DIR/results/tc2_perf_results_${TYPE[$i]}_${date}.csv
	done
done
