
source $rootdir/test/common/autotest_common.sh
plugindir=$rootdir/examples/bdev/fio_plugin

function bdevperf(){
	$rootdir/test/bdev/bdevperf/bdevperf $@
}

function fio_verify(){
	sudo LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio $curdir/test.fio --ioengine=spdk_bdev $@
}
