#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0)../)
source "$rootdir/test/common/autotest_common.sh"

if [ $SPDK_TEST_BLOCKDEV -eq 1 ]; then
	if [ $(uname -s) = Linux ]; then
		./test/bdev/bdevjson/json_config.sh
		if modprobe -n nbd; then
			./test/bdev/nbdjson/json_config.sh
		fi
	fi
fi

if [ $SPDK_TEST_ISCSI -eq 1 ]; then
	./test/iscsi_tgt/iscsijson/json_config.sh
fi

if [ $SPDK_TEST_NVMF -eq 1 ]; then
	./test/nvmf/nvmfjson/json_config.sh
fi

if [ $SPDK_TEST_VHOST -eq 1 ]; then
	./test/vhost/json_config/json_config.sh
fi

if [ $SPDK_TEST_VHOST_INIT -eq 1 ]; then
	./test/vhost/initiator/json_config.sh
fi

if [ $SPDK_TEST_PMDK -eq 1 ]; then
	./test/pmem/json_config/json_config.sh
fi

if [ $SPDK_TEST_RBD -eq 1 ]; then
	./test/bdev/bdevjson/rbd_json_config.sh
fi
