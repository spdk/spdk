#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

gather_supported_nvmf_pci_devs
TCP_INTERFACE_LIST=("${net_devs[@]}")
if ((${#TCP_INTERFACE_LIST[@]} == 0)); then
	echo "ERROR: Physical TCP interfaces are not ready"
	exit 1
fi

rpc_py="$rootdir/scripts/rpc.py"
perf="$SPDK_EXAMPLE_DIR/perf"

function adq_configure_driver() {
	# Enable adding flows to hardware
	"${NVMF_TARGET_NS_CMD[@]}" ethtool --offload $NVMF_TARGET_INTERFACE hw-tc-offload on
	# ADQ driver turns on this switch by default, we need to turn it off for SPDK testing
	"${NVMF_TARGET_NS_CMD[@]}" ethtool --set-priv-flags $NVMF_TARGET_INTERFACE channel-pkt-inspect-optimize off
	# Since sockets are non-blocking, a non-zero value of net.core.busy_read is sufficient
	sysctl -w net.core.busy_poll=1
	sysctl -w net.core.busy_read=1

	tc=/usr/sbin/tc
	# Create 2 traffic classes and 2 tc1 queues
	"${NVMF_TARGET_NS_CMD[@]}" $tc qdisc add dev $NVMF_TARGET_INTERFACE root \
		mqprio num_tc 2 map 0 1 queues 2@0 2@2 hw 1 mode channel
	"${NVMF_TARGET_NS_CMD[@]}" $tc qdisc add dev $NVMF_TARGET_INTERFACE ingress
	# TC filter is configured using target address (traddr) and port number (trsvcid) to steer packets
	"${NVMF_TARGET_NS_CMD[@]}" $tc filter add dev $NVMF_TARGET_INTERFACE protocol \
		ip parent ffff: prio 1 flower dst_ip $NVMF_FIRST_TARGET_IP/32 ip_proto tcp dst_port $NVMF_PORT skip_sw hw_tc 1
	# Setup mechanism for Tx queue selection based on Rx queue(s) map
	"${NVMF_TARGET_NS_CMD[@]}" $rootdir/scripts/perf/nvmf/set_xps_rxqs $NVMF_TARGET_INTERFACE
}

function adq_start_nvmf_target() {
	nvmfappstart -m $2 --wait-for-rpc
	trap 'process_shm --id $NVMF_APP_SHM_ID; clean_ints_files; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
	$rpc_py sock_impl_set_options --enable-placement-id $1 --enable-zerocopy-send-server -i posix
	$rpc_py framework_start_init
	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS --io-unit-size 8192 --sock-priority $1
	$rpc_py bdev_malloc_create 64 512 -b Malloc1
	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
}

function adq_reload_driver() {
	rmmod ice
	modprobe ice
	sleep 5
}

function clean_ints_files() {
	rm -f temp_ints1.log
	rm -f temp_ints2.log
}

function get_nvmf_poll_groups() {
	"$rpc_py" thread_get_pollers | jq -r '.threads[] | .active_pollers[] |
	select(.name == "nvmf_poll_group_poll").busy_count'
}

function num_busy_count() {
	get_pollers_busy_count0=($(get_nvmf_poll_groups))
	sleep 2
	get_pollers_busy_count1=($(get_nvmf_poll_groups))
	local num=0
	for i in "${!get_pollers_busy_count0[@]}"; do
		increment=$((get_pollers_busy_count1[i] - get_pollers_busy_count0[i]))
		if ((increment > 0)); then
			((++num))
		fi
	done
	echo $num
}

function compare_ints() {
	grep $2 < /proc/interrupts | awk '{
		for (i = 1; i <= NF; i++) {
				val[i]=$i;
				if (i>1)
					printf "%d\n", $i;
		}
	}' > temp_ints1.log
	sleep $1
	grep $2 < /proc/interrupts | awk '{
		for (i = 1; i <= NF; i++) {
				val[i]=$i;
				if (i>1)
					printf "%d\n", $i;
		}
	}' > temp_ints2.log

	if diff temp_ints1.log temp_ints2.log > /dev/null; then
		return 0
	fi
	return 1
}

function check_ints_result() {
	# We only test check_ints three times here, as long as there is no interruption once,
	# we consider it is pass. Of course, ideally, one time is enough.
	for ((i = 1; i <= 3; i++)); do
		if compare_ints 2 $NVMF_TARGET_INTERFACE; then
			return 0
		fi
	done
	return 1
}

# Clear the previous configuration that may have an impact.
# At present, ADQ configuration is only applicable to the ice driver.
adq_reload_driver

# Testcase 1 and Testcase 2 show the SPDK interacting with ADQ.
# The number of continuously increasing nvmf_poll_group_poll's busy_count, we define it as "num_busy_count".
# When ADQ enabled, num_busy_count will be equal to the number of tc1 queues of traffic classes.
# When ADQ disabled, num_busy_count will be equal to the smaller value of initiator connections and target cores.
# Testcase 1: Testing 2 traffic classes and 2 tc1 queues without ADQ
nvmftestinit
adq_start_nvmf_target 0 0xF
sleep 2
$perf -q 64 -o 4096 -w randread -t 10 -c 0x70 \
	-r "trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT} \
subnqn:nqn.2016-06.io.spdk:cnode1" &
perfpid=$!
sleep 2
if [[ $(num_busy_count) -ne 3 ]]; then
	echo "ERROR: num_busy_count != cores of initiators! Testcase 1 failed."
	exit 1
fi
wait $perfpid
clean_ints_files
nvmftestfini

adq_reload_driver

# Testcase 2: Testing 2 traffic classes and 2 tc1 queues with ADQ
nvmftestinit
sleep 2
adq_configure_driver
adq_start_nvmf_target 1 0xF
sleep 2
# The number of I/O connections from initiator is the core count * qpairs per ns, so here its 12.
# ADQ on target side will work if 12 connections are matched to two out of four cores on the target.
$perf -q 64 -o 4096 -w randread -t 15 -P 4 -c 0x70 \
	-r "trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT} \
subnqn:nqn.2016-06.io.spdk:cnode1" &
perfpid=$!
sleep 3
if ! check_ints_result; then
	echo "ERROR: check_ints failed! There is interruption in perf, this is not what we expected."
	exit 1
fi
if [[ $(num_busy_count) -ne 2 ]]; then
	echo "ERROR: num_busy_count != tc1 queues of traffic classes! Testcase 2 failed."
	exit 1
fi
wait $perfpid
clean_ints_files
nvmftestfini

trap - SIGINT SIGTERM EXIT
