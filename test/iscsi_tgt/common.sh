# Network configuration
TARGET_INTERFACE="spdk_tgt_int"
INITIATOR_INTERFACE="spdk_init_int"
TARGET_NAMESPACE="spdk_iscsi_ns"
TARGET_NS_CMD="ip netns exec $TARGET_NAMESPACE"

# iSCSI target configuration
TARGET_IP=10.0.0.1
INITIATOR_IP=10.0.0.2
ISCSI_PORT=3260
NETMASK=$INITIATOR_IP/32
INITIATOR_TAG=2
INITIATOR_NAME=ANY
PORTAL_TAG=1
ISCSI_APP="$TARGET_NS_CMD ./app/iscsi_tgt/iscsi_tgt -i 0"
ISCSI_TEST_CORE_MASK=0xFF

function create_veth_interfaces() {
	# $1 = test type (posix/vpp)
	ip netns del $TARGET_NAMESPACE || true
	ip link delete $INITIATOR_INTERFACE || true

	trap "cleanup_veth_interfaces $1; exit 1" SIGINT SIGTERM EXIT

	# Create veth (Virtual ethernet) interface pair
	ip link add $TARGET_INTERFACE type veth peer name $INITIATOR_INTERFACE
	ip link set $INITIATOR_INTERFACE up
	ip addr add $INITIATOR_IP/24 dev $INITIATOR_INTERFACE

	# Create and add interface for target to network namespace
	ip netns add $TARGET_NAMESPACE
	ip link set $TARGET_INTERFACE netns $TARGET_NAMESPACE
	ip netns exec $TARGET_NAMESPACE ip link set $TARGET_INTERFACE up

	if [ "$1" == "posix" ]; then
		$TARGET_NS_CMD ip link set lo up
		$TARGET_NS_CMD ip addr add $TARGET_IP/24 dev $TARGET_INTERFACE
		$TARGET_NS_CMD ip link set $TARGET_INTERFACE up
		if [ $SPDK_TEST_VPP -eq 1 ]; then
			$TARGET_NS_CMD vpp unix { nodaemon cli-listen /run/vpp/cli.sock } &
			sleep 5
		fi
	else
		start_vpp
	fi
}

function cleanup_veth_interfaces() {
	# $1 = test type (posix/vpp)
	if [ "$1" == "vpp" ] || [ $SPDK_TEST_VPP -eq 1 ]; then
		kill_vpp
	fi

	# Cleanup veth interfaces and network namespace
	# Note: removing one veth, removes the pair
	ip link delete $INITIATOR_INTERFACE
	ip netns del $TARGET_NAMESPACE
}

function start_vpp() {
	# Disable VPP communication library debug
	export VCL_DEBUG=0

	# Start VPP process in SPDK target network namespace
	$TARGET_NS_CMD vpp unix { nodaemon cli-listen /run/vpp/cli.sock } &
	vpp_pid=$!
	echo "VPP Process pid: $vpp_pid"
	sleep 5

	# Setup host interface
	vppctl create host-interface name $TARGET_INTERFACE
	VPP_TGT_INT="host-$TARGET_INTERFACE"
	vppctl set interface state $VPP_TGT_INT up
	vppctl set interface ip address $VPP_TGT_INT $TARGET_IP/24

	# Verify connectivity
	vppctl show int addr
	ip addr show $INITIATOR_INTERFACE
	ip netns exec $TARGET_NAMESPACE ip addr show $TARGET_INTERFACE
	ping -c 1 $TARGET_IP
	vppctl ping $INITIATOR_IP repeat 1
}

function kill_vpp() {
	vppctl delete host-interface name $TARGET_INTERFACE || true
	vpp_pid=$(pgrep vpp)
	killprocess $vpp_pid
}
