#!/bin/bash
rootdir=$(readlink -f $(dirname $0))/../..

rm -rf /tmp/lsblk
mkdir -p /tmp/lsblk
cd /tmp/lsblk
mkdir proc
cp $rootdir/lib/sysfs/sysfs.so .

function create_dev_node()
{
	mkdir -p sys/block/$1
	mkdir -p sys/dev/block/$2:$3
	echo $4 > sys/dev/block/$2:$3/size
	echo $4 > sys/block/$1/size
	echo "$2:$3" > sys/block/$1/dev
	echo "$2 $3 $1 100 0 800 50000 50 0 400 125000 3 175000 350000" >> proc/diskstats
}

create_dev_node nvme3n1 10 1 1048576
create_dev_node lvs1 10 2 512000
create_dev_node comp_lvs1 10 3 512000
