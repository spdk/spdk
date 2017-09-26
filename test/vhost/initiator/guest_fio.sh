#!/usr/bin/env bash

fio_bdev_conf="$1"
fio_job_conf="$2"
virtio_bdevs=""
/root/spdk/scripts/setup.sh
. /root/spdk/scripts/autotest_common.sh

bdevs=$(discover_bdevs /root/spdk /root/bdev.conf | jq -r '.[] | select(.claimed == false)')
for b in $(echo $bdevs | jq -r '.name') ; do
	virtio_bdevs+="$b:"
done

sed -i 's/\bfilename\b/&='$virtio_bdevs'/' /root/bdev.fio
LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio \
	--ioengine=spdk_bdev --runtime=10 /root/bdev.fio $fio_job_conf $fio_bdev_conf --spdk_mem=1024
