#!/usr/bin/env bash

fio_bdev_conf="$1"
/root/spdk/scripts/setup.sh
. /root/spdk/scripts/autotest_common.sh
fio_job_conf="--section=fio_job_guest_randwrite"
bdevs=$(discover_bdevs /root/spdk /root/bdev.conf | jq -r '.[] | select(.claimed == false)')
virtio_bdevs=""
for b in $(echo $bdevs | jq -r '.name') ; do
	virtio_bdevs+="$b:"
done

if [ "$2" == "--nigthly" ]; then
	fio_job_conf="--section=fio_job_guest_randwrite --section=fio_job_guest_randrw --section=fio_job_guest_write --section=fio_job_guest_rw"
fi

sed -i 's/\bfilename\b/&='$virtio_bdevs'/' /root/bdev.fio
LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio \
	--ioengine=spdk_bdev --runtime=10 /root/bdev.fio $fio_job_conf $fio_bdev_conf --spdk_mem=1024
