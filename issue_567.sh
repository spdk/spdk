#!/bin/bash -xe

shopt -s expand_aliases

PATH=$PWD/scripts:$PATH

aio_bdev="aio0"
lvs_name="lvs_${aio_bdev}"
aio_file="$PWD/$aio_bdev"

rm -f $aio_file
truncate -s 1G $aio_file

#alias spdkcli="spdkcli.py -s <socket_path>"
alias spdkcli="spdkcli.py $socket_path"

spdkcli /bdevs/aio create $aio_bdev $aio_file 4096

spdkcli /lvol_stores/ create $lvs_name $aio_bdev

for ((i=0; i<3; i++)); do
	spdkcli /bdevs/logical_volume create "$lvol_name$i" 128 $lvs_name
done

spdkcli /bdevs/logical_volume ls
spdkcli /bdevs/logical_volume delete "$lvs_name/${lvol_name}2"

spdkcli /bdevs/logical_volume ls
spdkcli /bdevs/aio delete $aio_bdev
