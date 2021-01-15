testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

rpc_py="$rootdir/scripts/rpc.py"

r0_mask=0x1
r1_mask=0x2
r2_mask=0x4

cpu_server_mask=0x07
rpc_server_addr="/var/tmp/spdk.sock"

function cleanup() {
	rm -f "$SPDK_TEST_STORAGE/aiofile"
}

function start_intr_tgt() {
	local rpc_addr="${1:-$rpc_server_addr}"
	local cpu_mask="${2:-$cpu_server_mask}"

	"$SPDK_EXAMPLE_DIR/interrupt_tgt" -m $cpu_mask -r $rpc_addr -E -g &
	intr_tgt_pid=$!
	trap 'killprocess "$intr_tgt_pid"; cleanup; exit 1' SIGINT SIGTERM EXIT
	waitforlisten "$intr_tgt_pid" $rpc_addr
}

function reactor_is_busy_or_idle() {
	local pid=$1
	local idx=$2
	local state=$3

	if [[ $state != "busy" ]] && [[ $state != "idle" ]]; then
		return 1
	fi

	if ! hash top; then
		# Fail this test if top is missing from system.
		return 1
	fi

	for ((j = 10; j != 0; j--)); do
		top_reactor=$(top -bHn 1 -p $pid -w 256 | grep reactor_$idx)
		cpu_rate=$(echo $top_reactor | sed -e 's/^\s*//g' | awk '{print $9}')
		cpu_rate=${cpu_rate%.*}

		if [[ $state = "busy" ]] && [[ $cpu_rate -lt 70 ]]; then
			sleep 1
		elif [[ $state = "idle" ]] && [[ $cpu_rate -gt 30 ]]; then
			sleep 1
		else
			return 0
		fi
	done

	if [[ $state = "busy" ]]; then
		echo "cpu rate ${cpu_rate} of reactor $i probably is not busy polling"
	else
		echo "cpu rate ${cpu_rate} of reactor $i probably is not idle interrupt"
	fi

	return 1
}

function reactor_is_busy() {
	reactor_is_busy_or_idle $1 $2 "busy"
}

function reactor_is_idle() {
	reactor_is_busy_or_idle $1 $2 "idle"
}

function reactor_get_thread_ids() {
	local reactor_cpumask=$1
	local grep_str

	reactor_cpumask=$((reactor_cpumask))
	jq_str='.threads|.[]|select(.cpumask == $reactor_cpumask)|.id'

	# shellcheck disable=SC2005
	echo "$($rpc_py thread_get_stats | jq --arg reactor_cpumask "$reactor_cpumask" "$jq_str")"

}

function setup_bdev_mem() {
	"$rpc_py" <<- RPC
		bdev_malloc_create -b Malloc0 32 512
		bdev_malloc_create -b Malloc1 32 512
		bdev_malloc_create -b Malloc2 32 512
	RPC
}

function setup_bdev_aio() {
	if [[ $(uname -s) != "FreeBSD" ]]; then
		dd if=/dev/zero of="$SPDK_TEST_STORAGE/aiofile" bs=2048 count=5000
		"$rpc_py" bdev_aio_create "$SPDK_TEST_STORAGE/aiofile" AIO0 2048
	fi
}
