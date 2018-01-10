
set -xe
virtio_bdevs=""
virtio_with_unmap=""
/root/spdk/scripts/setup.sh
. /root/spdk/scripts/autotest_common.sh

vbdevs=$(discover_bdevs /root/spdk /root/spdk/test/vhost/initiator/bdev_pci.conf)
virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)
virtio_with_unmap=$(jq -r '[.[] | select(.supported_io_types.unmap==true).name]
 | join(":")' <<< $vbdevs)

timing_enter run_spdk_fio_pci
LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio --ioengine=spdk_bdev \
 /root/spdk/test/vhost/initiator/bdev.fio --filename=$virtio_bdevs --section=job_randwrite \
 --section=job_randrw --section=job_write --section=job_rw \
 --spdk_conf=/root/spdk/test/vhost/initiator/bdev_pci.conf --spdk_mem=1024
timing_exit run_spdk_fio_pci

timing_enter run_spdk_fio_pci_unmap
LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio --ioengine=spdk_bdev \
 /root/spdk/test/vhost/initiator/bdev.fio --filename=$virtio_with_unmap \
 --spdk_conf=/root/spdk/test/vhost/initiator/bdev_pci.conf --spdk_mem=1024
timing_exit run_spdk_fio_pci_unmap
