fio_bdev_conf="$1"
fio_conf_option="$2"
/root/spdk/scripts/setup.sh
. /root/spdk/scripts/autotest_common.sh
bdevs=$(discover_bdevs /root/spdk /root/bdev.conf | jq -r '.[] | select(.claimed == false)')
virtio_bdevs=""
if [ $fio_conf_option == 1 ]; then
        for b in $(echo $bdevs | jq -r '.name') ; do
                if [ $b == "VirtioScsi0t0" ]; then
                        virtio_bdevs+="$b:"
                fi
        done
elif [ $fio_conf_option == 2 ]; then
        for b in $(echo $bdevs | jq -r '.name') ; do
                if [ $b == "VirtioScsi1t0" ] || [ $b == "VirtioScsi1t1" ]; then
                        virtio_bdevs+="$b:"
                fi
        done
fi
sed -i "s|filename=|filename=$virtio_bdevs|g" /root/bdev.fio
cat /root/bdev.fio
LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio \
	--ioengine=spdk_bdev --runtime=10 /root/bdev.fio $fio_bdev_conf --spdk_mem=1024
