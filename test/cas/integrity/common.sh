
plugindir=$rootdir/examples/bdev/fio_plugin

function bdevperf_verify(){
	sudo $rootdir/test/bdev/bdevperf/bdevperf -w verify $@
}

function fio_verify(){
	sudo LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio $curdir/test.fio --ioengine=spdk_bdev $@
}
