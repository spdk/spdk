#!/usr/bin/env bash

set -e

function configure_linux {
	if mount | grep -qv hugetlbfs; then
		mkdir -p /mnt/huge
		mount -t hugetlbfs nodev /mnt/huge
	fi

	echo $NRHUGE > /proc/sys/vm/nr_hugepages
}

function configure_freebsd {
	kldunload contigmem.ko || true
	kenv hw.contigmem.num_buffers=$((NRHUGE * 2 / 32))
	kenv hw.contigmem.buffer_size=$((32 * 1024 * 1024))
	kldload contigmem.ko
}

if [ "$1" = "" ]; then
	NRHUGE=1024
else
	NRHUGE="$1"
fi

if [ `uname` = Linux ]; then
	configure_linux
else
	configure_freebsd
fi

