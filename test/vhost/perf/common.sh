set -e

BLK_SIZE=4096
RW=randrw
MIX=100
IODEPTH=128
RUNTIME=60
RAMP_TIME=10
FIO_BIN="/usr/src/fio/fio"
DISKNO="ALL"
CPUMASK=0x02

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help            Print help and exit"
	echo "    --bs=INT          The block size in bytes used for I/O units. [default=$BLK_SIZE]"
	echo "    --rw=STR          Type of I/O pattern. [default=$RW]"
	echo "    --rwmixread=INT   Percentage of a mixed workload that should be reads. [default=$MIX]"
	echo "    --iodepth=INT     Number of I/O units to keep in flight against the file. [default=$IODEPTH]"
	echo "    --runtime=TIME    Tell fio to terminate processing after the specified period of time. [default=$RUNTIME]"
	echo "    --ramp_time=TIME  Fio will run the specified workload for this amount of time before logging any performance numbers. [default=$RAMP_TIME]"
	echo "    --fiobin=PATH     Path to fio binary. [default=$FIO_BIN]"
	echo "    --disk_no=INT,ALL Number of disks to test on, if =ALL then test on all found disk. [default=$DISKNO]"
	echo "    --cpumask=HEX     The parameter given is a bit mask of allowed CPUs the job may run on. [default=$CPUMASK]"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0; exit 0 ;;
			bs=*) BLK_SIZE="${OPTARG#*=}" ;;
			rw=*) RW="${OPTARG#*=}" ;;
			rwmixread=*) MIX="${OPTARG#*=}" ;;
			iodepth=*) IODEPTH="${OPTARG#*=}" ;;
			runtime=*) RUNTIME="${OPTARG#*=}" ;;
			ramp_time=*) RAMP_TIME="${OPTARG#*=}" ;;
			fiobin=*) FIO_BIN="${OPTARG#*=}" ;;
			disk_no=*) DISKNO="${OPTARG#*=}" ;;
			cpumask=*) CPUMASK="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

. $(readlink -e "$(dirname $0)/../common/common.sh") || exit 1
mkdir -p $BASE_DIR/perf_output
virtio_fio_results="$BASE_DIR/perf_output/virtio_fio.json"
nvme_fio_results="$BASE_DIR/perf_output/nvme_fio.json"

if [ ! -x $FIO_BIN ]; then
	error "Invalid path of fio binary"
fi

if [[ $EUID -ne 0 ]]; then
	echo "INFO: Go away user come back as root"
	exit 1
fi
