#!/usr/bin/env bash

set -e

if [ "$1" = "" ]; then
	NRHUGE=1024
else
	NRHUGE="$1"
fi

if mount | grep -qv hugetlbfs; then
	mkdir -p /mnt/huge
	mount -t hugetlbfs nodev /mnt/huge
fi

echo $NRHUGE > /proc/sys/vm/nr_hugepages
