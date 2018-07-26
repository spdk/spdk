#!/usr/bin/env bash
CONVERTER_DIR=$(readlink -f $(dirname $0))
source $CONVERTER_DIR/common.sh
source $CONVERTER_DIR/../nvmf/common.sh

trap 'on_error_exit' ERR

create_aio_bdevs
cp $CONVERTER_DIR/config_iscsi.ini $CONVERTER_DIR/config_tmp.ini
run_tests
test_cleanup
