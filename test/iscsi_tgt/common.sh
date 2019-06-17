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
ISCSI_APP="$TARGET_NS_CMD ./app/iscsi_tgt/iscsi_tgt"
ISCSI_TEST_CORE_MASK=0xFF

function create_veth_interfaces() {
	# $1 = test type (posix/vpp)
	ip netns del $TARGET_NAMESPACE || true
	ip link delete $INITIATOR_INTERFACE || true

	trap "cleanup_veth_interfaces $1; exit 1" SIGINT SIGTERM EXIT

	# Create veth (Virtual ethernet) interface pair
	ip link add $INITIATOR_INTERFACE type veth peer name $TARGET_INTERFACE
	ip addr add $INITIATOR_IP/24 dev $INITIATOR_INTERFACE
	ip link set $INITIATOR_INTERFACE up

	# Create and add interface for target to network namespace
	ip netns add $TARGET_NAMESPACE
	ip link set $TARGET_INTERFACE netns $TARGET_NAMESPACE

	# Accept connections from veth interface
	iptables -I INPUT 1 -i $INITIATOR_INTERFACE -p tcp --dport $ISCSI_PORT -j ACCEPT

	$TARGET_NS_CMD ip link set lo up
	$TARGET_NS_CMD ip addr add $TARGET_IP/24 dev $TARGET_INTERFACE
	$TARGET_NS_CMD ip link set $TARGET_INTERFACE up

	# Verify connectivity
	ping -c 1 $TARGET_IP
	ip netns exec $TARGET_NAMESPACE ping -c 1 $INITIATOR_IP
}

function cleanup_veth_interfaces() {
	# $1 = test type (posix/vpp)

	# Cleanup veth interfaces and network namespace
	# Note: removing one veth, removes the pair
	ip link delete $INITIATOR_INTERFACE
	ip netns del $TARGET_NAMESPACE
}

function iscsitestinit() {
	if [ "$1" == "iso" ]; then
		$rootdir/scripts/setup.sh
		if [ ! -z "$2" ]; then
			create_veth_interfaces $2
		else
			# default to posix
			create_veth_interfaces "posix"
		fi
	fi
}

function waitforiscsidevices() {
	local num=$1

	for ((i=1; i<=20; i++)); do
		n=$( iscsiadm -m session -P 3 | grep "Attached scsi disk sd[a-z]*" | wc -l )
		if [ $n -ne $num ]; then
			sleep 0.1
		else
			return 0
		fi
	done

	return 1
}

function iscsitestfini() {
	if [ "$1" == "iso" ]; then
		if [ ! -z "$2" ]; then
			cleanup_veth_interfaces $2
		else
			# default to posix
			cleanup_veth_interfaces "posix"
		fi
		$rootdir/scripts/setup.sh reset
	fi
}
