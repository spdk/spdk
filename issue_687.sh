#!/bin/bash -xe

rootdir=$(readlink -f $(dirname $0))

shopt -s expand_aliases

function on_exit() {
	trap - EXIT
}

trap "on_exit" EXIT ERR

: ${socket=$rootdir/../sandbox/vhost0/vhost.sock}
alias rpc='$rootdir/scripts/rpc.py -t 3600 -s ${socket}'

rpc construct_vhost_scsi_controller vhost.0 --cpumask 1 || true
sleep 1


for i in {0..7} ; do
	  rpc construct_malloc_bdev 64 512 -b Malloc$i || true
	  rpc remove_vhost_scsi_target vhost.0 $i || true
done

while [ 1 -eq 1 ] ; do
	for i in {0..7} ; do
	  rpc add_vhost_scsi_lun vhost.0 $i Malloc$i
	done
	for i in {0..7} ; do
	  rpc remove_vhost_scsi_target vhost.0 $i
	done
done

trap - EXIT ERR
echo "DONE"
