
source $rootdir/test/common/autotest_common.sh

rpc_py="$rootdir/scripts/rpc.py"

function kill_spdk()
{
	killprocess $1 2>/dev/null
}

function with_spdk()
{
	$rootdir/app/iscsi_tgt/iscsi_tgt -c $curdir/environment.conf &
	local spdk_pid=$!

	trap "kill_spdk $spdk_pid|| true;" SIGINT SIGTERM EXIT

	eval $@
	local rc=$?

	kill_spdk $spdk_pid

	return $rc
}
