#!/usr/bin/env bash

if [ $SPDK_TEST_BLOCKDEV -eq 1 ]; then
	if [ $(uname -s) = Linux ]; then
		run_test suite test/bdev/bdevjson/json_config.sh
		if modprobe -n nbd; then
			run_test suite test/bdev/nbdjson/json_config.sh
		fi
	fi
fi

if [ $SPDK_TEST_ISCSI -eq 1 ]; then
	run_test suite ./test/iscsi_tgt/iscsijson/json_config.sh
fi

if [ $SPDK_TEST_NVMF -eq 1 ]; then
	run_test suite ./test/nvmf/nvmfjson/json_config.sh
fi

if [ $SPDK_TEST_VHOST -eq 1 ]; then
	run_test suite ./test/vhost/json_config/json_config.sh
fi

if [ $SPDK_TEST_VHOST_INIT -eq 1 ]; then
	run_test suite ./test/vhost/initiator/json_config.sh
fi

if [ $SPDK_TEST_PMDK -eq 1 ]; then
	run_test suite ./test/pmem/json_config/json_config.sh
fi

if [ $SPDK_TEST_RBD -eq 1 ]; then
	run_test suite ./test/bdev/bdevjson/rbd_json_config.sh
fi
