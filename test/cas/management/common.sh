
source $rootdir/test/common/autotest_common.sh

rpc_py="$rootdir/scripts/rpc.py"

spdk_pid=0

function kill_spdk()
{
	killprocess $spdk_pid 2>/dev/null
}

function with_spdk()
{
	$rootdir/app/iscsi_tgt/iscsi_tgt -c $curdir/environment.conf &
	spdk_pid=$!

	trap "kill_spdk || true;" SIGINT SIGTERM EXIT

	eval $@
	local rc=$?

	kill_spdk

	return $rc
}
