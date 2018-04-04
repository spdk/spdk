# Network configuration
TARGET_INTERFACE="spdk_tgt_int"
INITIATOR_INTERFACE="spdk_init_int"

# iSCSI target configuration
TARGET_IP=10.0.0.1
INITIATOR_IP=10.0.0.2
ISCSI_PORT=3260
NETMASK=$INITIATOR_IP/30
INITIATOR_TAG=2
INITIATOR_NAME=ANY
PORTAL_TAG=1
ISCSI_APP="./app/iscsi_tgt/iscsi_tgt -i 0"
ISCSI_TEST_CORE_MASK=0xFF

function create_veth_interfaces() {
	ip link delete $INITIATOR_INTERFACE || true

	# Create veth (Virtual ethernet) interface pair
	ip link add $INITIATOR_INTERFACE type veth peer name $TARGET_INTERFACE
	ip addr add $INITIATOR_IP/24 dev $INITIATOR_INTERFACE
	ip link set $INITIATOR_INTERFACE up

	ip addr add $TARGET_IP/24 dev $TARGET_INTERFACE
	ip link set $TARGET_INTERFACE up

	trap "cleanup_veth_interfaces; exit 1" SIGINT SIGTERM EXIT
}

function cleanup_veth_interfaces() {
	# Cleanup veth interfaces
	# Note: removing one veth, removes the pair
	ip link delete $INITIATOR_INTERFACE
}
