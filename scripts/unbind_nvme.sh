#!/usr/bin/env bash

set -e

function configure_linux {
	rmmod nvme || true
}

function configure_freebsd {
	TMP=`mktemp`
	AWK_PROG="{if (count > 0) printf \",\"; printf \"%s:%s:%s\",\$2,\$3,\$4; count++}"
	echo $AWK_PROG > $TMP
	NVME_PCICONF=`pciconf -l | grep class=0x010802`
	BDFS=`echo $NVME_PCICONF | awk -F: -f $TMP`
	kldunload nic_uio.ko || true
	kenv hw.nic_uio.bdfs=$BDFS
	kldload nic_uio.ko
	rm $TMP
}

if [ `uname` = Linux ]; then
	configure_linux
else
	configure_freebsd
fi

