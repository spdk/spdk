# Network configuration
# There is one initiator interface and it is accessed directly.
# There are two target interfaces and they are accessed through an namespace.
ISCSI_BRIDGE="iscsi_br"
INITIATOR_INTERFACE="spdk_init_int"
INITIATOR_BRIDGE="init_br"
TARGET_NAMESPACE="spdk_iscsi_ns"
TARGET_NS_CMD=(ip netns exec "$TARGET_NAMESPACE")
TARGET_INTERFACE="spdk_tgt_int"
TARGET_INTERFACE2="spdk_tgt_int2"
TARGET_BRIDGE="tgt_br"
TARGET_BRIDGE2="tgt_br2"

# iSCSI target configuration
TARGET_IP=10.0.0.1
TARGET_IP2=10.0.0.3
INITIATOR_IP=10.0.0.2
ISCSI_PORT=3260
NETMASK=$INITIATOR_IP/32
INITIATOR_TAG=2
INITIATOR_NAME=ANY
PORTAL_TAG=1
ISCSI_APP=("${TARGET_NS_CMD[@]}" "${ISCSI_APP[@]}")
ISCSI_TEST_CORE_MASK=0xFF

function create_veth_interfaces() {
	ip link set $INITIATOR_BRIDGE nomaster || true
	ip link set $TARGET_BRIDGE nomaster || true
	ip link set $TARGET_BRIDGE2 nomaster || true
	ip link set $INITIATOR_BRIDGE down || true
	ip link set $TARGET_BRIDGE down || true
	ip link set $TARGET_BRIDGE2 down || true
	ip link delete $ISCSI_BRIDGE type bridge || true
	ip link delete $INITIATOR_INTERFACE || true
	"${TARGET_NS_CMD[@]}" ip link delete $TARGET_INTERFACE || true
	"${TARGET_NS_CMD[@]}" ip link delete $TARGET_INTERFACE2 || true
	ip netns del $TARGET_NAMESPACE || true

	trap 'cleanup_veth_interfaces; exit 1' SIGINT SIGTERM EXIT

	# Create network namespace
	ip netns add $TARGET_NAMESPACE

	# Create veth (Virtual ethernet) interface pairs
	ip link add $INITIATOR_INTERFACE type veth peer name $INITIATOR_BRIDGE
	ip link add $TARGET_INTERFACE type veth peer name $TARGET_BRIDGE
	ip link add $TARGET_INTERFACE2 type veth peer name $TARGET_BRIDGE2

	# Associate veth interface pairs with network namespace
	ip link set $TARGET_INTERFACE netns $TARGET_NAMESPACE
	ip link set $TARGET_INTERFACE2 netns $TARGET_NAMESPACE

	# Allocate IP addresses
	ip addr add $INITIATOR_IP/24 dev $INITIATOR_INTERFACE
	"${TARGET_NS_CMD[@]}" ip addr add $TARGET_IP/24 dev $TARGET_INTERFACE
	"${TARGET_NS_CMD[@]}" ip addr add $TARGET_IP2/24 dev $TARGET_INTERFACE2

	# Link up veth interfaces
	ip link set $INITIATOR_INTERFACE up
	ip link set $INITIATOR_BRIDGE up
	ip link set $TARGET_BRIDGE up
	ip link set $TARGET_BRIDGE2 up
	"${TARGET_NS_CMD[@]}" ip link set $TARGET_INTERFACE up
	"${TARGET_NS_CMD[@]}" ip link set $TARGET_INTERFACE2 up
	"${TARGET_NS_CMD[@]}" ip link set lo up

	# Create a bridge
	ip link add $ISCSI_BRIDGE type bridge
	ip link set $ISCSI_BRIDGE up

	# Add veth interfaces to the bridge
	ip link set $INITIATOR_BRIDGE master $ISCSI_BRIDGE
	ip link set $TARGET_BRIDGE master $ISCSI_BRIDGE
	ip link set $TARGET_BRIDGE2 master $ISCSI_BRIDGE

	# Accept connections from veth interface
	iptables -I INPUT 1 -i $INITIATOR_INTERFACE -p tcp --dport $ISCSI_PORT -j ACCEPT

	# Verify connectivity
	ping -c 1 $TARGET_IP
	ping -c 1 $TARGET_IP2
	"${TARGET_NS_CMD[@]}" ping -c 1 $INITIATOR_IP
	"${TARGET_NS_CMD[@]}" ping -c 1 $INITIATOR_IP
}

function cleanup_veth_interfaces() {
	# Cleanup bridge, veth interfaces, and network namespace
	# Note: removing one veth, removes the pair
	ip link set $INITIATOR_BRIDGE nomaster
	ip link set $TARGET_BRIDGE nomaster
	ip link set $TARGET_BRIDGE2 nomaster
	ip link set $INITIATOR_BRIDGE down
	ip link set $TARGET_BRIDGE down
	ip link set $TARGET_BRIDGE2 down
	ip link delete $ISCSI_BRIDGE type bridge
	ip link delete $INITIATOR_INTERFACE
	"${TARGET_NS_CMD[@]}" ip link delete $TARGET_INTERFACE
	"${TARGET_NS_CMD[@]}" ip link delete $TARGET_INTERFACE2
	ip netns del $TARGET_NAMESPACE
}

function iscsitestinit() {
	if [ "$TEST_MODE" == "iso" ]; then
		$rootdir/scripts/setup.sh
		create_veth_interfaces
	fi
}

function waitforiscsidevices() {
	local num=$1

	for ((i = 1; i <= 20; i++)); do
		n=$(iscsiadm -m session -P 3 | grep -c "Attached scsi disk sd[a-z]*" || true)
		if [ $n -ne $num ]; then
			sleep 0.1
		else
			return 0
		fi
	done

	return 1
}

function iscsitestfini() {
	if [ "$TEST_MODE" == "iso" ]; then
		cleanup_veth_interfaces
		$rootdir/scripts/setup.sh reset
	fi
}

function initiator_json_config() {
	# Prepare config file for iSCSI initiator
	jq . <<- JSON
		{
		  "subsystems": [
		    {
		      "subsystem": "bdev",
		      "config": [
		        {
		          "method": "bdev_iscsi_create",
		          "params": {
		            "name": "iSCSI0",
		            "url": "iscsi://$TARGET_IP/iqn.2016-06.io.spdk:disk1/0",
		            "initiator_iqn": "iqn.2016-06.io.spdk:disk1/0"
		          }
		        },
		        {
		          "method": "bdev_wait_for_examine"
		        }
		      ]
		    }
		  ]
		}
	JSON
}
