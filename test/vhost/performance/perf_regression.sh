BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../ && pwd)"
[[ -z "$WORKSPACE" ]] && WORKSPACE="$(cd $BASE_DIR/../../../../ && pwd)"

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

cp $BASE_DIR/configs/smp2.config $BASE_DIR/../common/autotest.config
cat $BASE_DIR/../common/autotest.config

echo "mkdir"
mkdir $WORKSPACE/fio_jobs || true
# Randomize write pattern for write jobs
for i in `ls $BASE_DIR/fio_jobs/ | egrep "*_(128).job"`
do
    echo "i: $i"
    cp $BASE_DIR/fio_jobs/$i $WORKSPACE/fio_jobs/
    echo "egrep"
    if egrep "rw=.*(write|rw)" $WORKSPACE/fio_jobs/$i > /dev/null; then
        echo "echo_"
        echo -e "do_verify=0\nverify=meta\nverify_pattern=`hexdump -n 4 -e '0x%08X' /dev/random`" >> $WORKSPACE/fio_jobs/$i
        echo "_echo"
    fi
    echo "1"
    #echo -e "per_job_logs=0" >> $WORKSPACE/fio_jobs/$i
    #echo -e "write_iops_log" >> $WORKSPACE/fio_jobs/$i
    #echo -e "log_avg_msec=20000" >> $WORKSPACE/fio_jobs/$i
    sed -i "s#numjobs=1#numjobs=2#g" $WORKSPACE/fio_jobs/$i
    sed -i "s#nvme-host#$i#g" $WORKSPACE/fio_jobs/$i
    sed -i "s#runtime=30s#runtime=120#g" $WORKSPACE/fio_jobs/$i
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
    sed -i "s#ramp_time=10#ramp_time=$ramptime#g" $WORKSPACE/fio_jobs/$i
done

echo "sed"
# Temporary, need to patch that as commit:
#sed -i "s#cpu<[$](nproc --all)#task_mask >= 1<<\$cpu#g" $WORKSPACE/spdk/test/vhost/common/common.sh
sed -i "s#sleep 2#sleep 10#g" $BASE_DIR/../common/common.sh

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
