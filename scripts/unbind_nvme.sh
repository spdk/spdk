#!/usr/bin/env bash

set -e

function configure_linux {
	lsmod | grep nvme && rmmod nvme
}

function configure_freebsd {
	TMP=`mktemp`
	AWK_PROG="{if (count > 0) printf \",\"; printf \"%s:%s:%s\",\$2,\$3,\$4; count++}"
	echo $AWK_PROG > $TMP
	NVME_PCICONF=`pciconf -l | grep class=0x010802`
	BDFS=`echo $NVME_PCICONF | awk -F: -f $TMP`
	kenv hw.nic_uio.bdfs=$BDFS
	kldload `find . -name nic_uio.ko | head -1`
	rm $TMP
}

if [ `uname` = Linux ]; then
	configure_linux
else
	configure_freebsd
fi

