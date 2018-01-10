
virtio_bdevs=""
virtio_with_unmap=""
/root/spdk/scripts/setup.sh
. /root/spdk/scripts/autotest_common.sh

vbdevs=$(discover_bdevs /root/spdk /root/spdk/test/vhost/initiator/bdev_pci.conf)
virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)
virtio_with_unmap=$(jq -r '[.[] | select(.supported_io_types.unmap==true).name]
 | join(":")' <<< $vbdevs)

timing_enter run_spdk_fio
LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio --ioengine=spdk_bdev \
 /root/spdk/test/vhost/initiator/bdev.fio --filename=$virtio_bdevs --section=job_randwrite \
 --section=job_randrw --section=job_write --section=job_rw \
 --spdk_conf=/root/spdk/test/vhost/initiator/bdev_pci.conf --spdk_mem=1024
timing_exit run_spdk_fio

timing_enter run_spdk_fio_unmap
LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio --ioengine=spdk_bdev \
 /root/spdk/test/vhost/initiator/bdev.fio --filename=$virtio_with_unmap --section=job_randwrite \
 --section=job_randrw --section=job_unmap_trim_sequential --section=job_unmap_trim_random \
 --section=job_unmap_write --spdk_conf=/root/spdk/test/vhost/initiator/bdev_pci.conf --spdk_mem=1024
timing_exit run_spdk_fio_unmap
