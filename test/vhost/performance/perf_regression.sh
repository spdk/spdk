BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../ && pwd)"
[[ -z "$WORKSPACE" ]] && WORKSPACE="$(cd $BASE_DIR/../../../../ && pwd)"
QUEUE_DEPTH=128
BLOCKSIZE=4096

function usage() {
    [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
    echo "Shortcut script for doing automated hotattach/hotdetach test"
    echo "Usage: $(basename $1) [OPTIONS]"
    echo
    echo "-h, --help                print help and exit"
    echo "    --queue-depth         specify queue_depth in fio tests"
    echo "    --blocksize           specify blocksize in fio tests"
    exit 0
}

while getopts 'xh-:' optchar; do
    case "$optchar" in
        -)
        case "$OPTARG" in
            help) usage $0 ;;
            queue-depth=*) QUEUE_DEPTH="${OPTARG#*=}" ;;
            blocksize=*) BLOCKSIZE="${OPTARG#*=}" ;;
            *) usage $0 "Invalid argument '$OPTARG'" ;;
        esac
        ;;
    h) usage $0 ;;
    x) set -x
        x="-x" ;;
    *) usage $0 "Invalid argument '$OPTARG'"
    esac
done
shift $(( OPTIND - 1 ))


. $BASE_DIR/../common/common.sh
vm_img="vhost_vm_image.qcow2"
DRIVES=2
VMS=$(($DRIVES))

# Prepare config files

cp vhost.conf $BASE_DIR/../fiotest/vhost.conf.in
echo "[Split]" >> $BASE_DIR/../fiotest/vhost.conf.in

DRIVES=6
for ((i=0 ; i<$DRIVES ; i++)); do
	echo "Split Nvme${i}n1 2" >> $BASE_DIR/../fiotest/vhost.conf.in
done
cat $BASE_DIR/../fiotest/vhost.conf.in

# cp $BASE_DIR/configs/smp2.config $BASE_DIR/../common/autotest.config
cat $BASE_DIR/../common/autotest.config

mkdir $WORKSPACE/fio_jobs || true


function create_fio_job() {
    local fio_job=$1
    local rw_method=$2
    rm $WORKSPACE/fio_jobs/$fio_job || true
    touch $WORKSPACE/fio_jobs/$fio_job
    cat << END_FIO_JOB >> $WORKSPACE/fio_jobs/$fio_job
[global]
blocksize=$BLOCKSIZE
iodepth=$QUEUE_DEPTH
ioengine=libaio
runtime=30s
time_based=1
filename=
ramp_time=10
group_reporting
thread
numjobs=1
direct=1
rw=$rw
[nvme-host]
END_FIO_JOB
}


# Randomize write pattern for write jobs
#for i in `ls $BASE_DIR/fio_jobs/ | egrep "*_(128).job"`
for rw in "read" "write" "randread" "randwrite" "rw" "randrw"
do
    fio_job="$BLOCKSIZE"_"$rw"_"$QUEUE_DEPTH".job
    create_fio_job "$fio_job" "$rw"

    echo "egrep"
    if egrep "rw=.*(write|rw)" $WORKSPACE/fio_jobs/$fio_job > /dev/null; then
        echo "echo_"
        echo -e "do_verify=0\nverify=meta\nverify_pattern=`hexdump -n 4 -e '\"0x%08X\"' /dev/urandom`" >> $WORKSPACE/fio_jobs/$fio_job
        echo "_echo"
    fi
    echo "1"
    sed -i "s#numjobs=1#numjobs=2#g" $WORKSPACE/fio_jobs/$fio_job
    sed -i "s#nvme-host#$fio_job#g" $WORKSPACE/fio_jobs/$fio_job
    sed -i "s#runtime=30s#runtime=120#g" $WORKSPACE/fio_jobs/$fio_job
    echo "2"
    ramptime=600
    if [[ "$i" =~ "randwrite" ]]; then
        ramptime=2400
    elif [[ "$i" =~ "randrw" ]]; then
        ramptime=900
    else
        ramptime=300
    fi
    echo "3"
    sed -i "s#ramp_time=10#ramp_time=$ramptime#g" $WORKSPACE/fio_jobs/$fio_job
done

echo "sed"
# Temporary, need to patch that as commit:
# sed -i "s#sleep 2#sleep 10#g" $BASE_DIR/../common/common.sh

out_cmd=''
for ((i=0 ; i<$VMS ; i++))
do
	out_cmd="$out_cmd --vm=$i,/home/sys_sgsw/$vm_img,Nvme$(($i/2))n1p$(($i%2)) "
done
echo VM launch parameter: $out_cmd

for i in `ls $WORKSPACE/fio_jobs/`; do
	echo Running performance test for $i
	echo $i
    sudo $BASE_DIR/../fiotest/autotest.sh --fio-bin=/home/sys_sgsw/fio_ubuntu \
    $out_cmd \
    --test-type=spdk_vhost_scsi \
    --fio-job=$WORKSPACE/fio_jobs/$i
done
