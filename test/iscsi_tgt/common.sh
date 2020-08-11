# Network configuration
TARGET_INTERFACE="spdk_tgt_int"
INITIATOR_INTERFACE="spdk_init_int"
TARGET_NAMESPACE="spdk_iscsi_ns"
TARGET_NS_CMD=(ip netns exec "$TARGET_NAMESPACE")

# iSCSI target configuration
TARGET_IP=10.0.0.1
INITIATOR_IP=10.0.0.2
ISCSI_PORT=3260
NETMASK=$INITIATOR_IP/32
INITIATOR_TAG=2
INITIATOR_NAME=ANY
PORTAL_TAG=1
ISCSI_APP=("${TARGET_NS_CMD[@]}" "${ISCSI_APP[@]}")
ISCSI_TEST_CORE_MASK=0xFF

function create_veth_interfaces() {
	ip netns del $TARGET_NAMESPACE || true
	ip link delete $INITIATOR_INTERFACE || true

	trap 'cleanup_veth_interfaces; exit 1' SIGINT SIGTERM EXIT

	# Create veth (Virtual ethernet) interface pair
	ip link add $INITIATOR_INTERFACE type veth peer name $TARGET_INTERFACE
	ip addr add $INITIATOR_IP/24 dev $INITIATOR_INTERFACE
	ip link set $INITIATOR_INTERFACE up

	# Create and add interface for target to network namespace
	ip netns add $TARGET_NAMESPACE
	ip link set $TARGET_INTERFACE netns $TARGET_NAMESPACE

	# Accept connections from veth interface
	iptables -I INPUT 1 -i $INITIATOR_INTERFACE -p tcp --dport $ISCSI_PORT -j ACCEPT

	"${TARGET_NS_CMD[@]}" ip link set $TARGET_INTERFACE up

	"${TARGET_NS_CMD[@]}" ip link set lo up
	"${TARGET_NS_CMD[@]}" ip addr add $TARGET_IP/24 dev $TARGET_INTERFACE

	# Verify connectivity
	ping -c 1 $TARGET_IP
	ip netns exec $TARGET_NAMESPACE ping -c 1 $INITIATOR_IP
}

function cleanup_veth_interfaces() {
	# Cleanup veth interfaces and network namespace
	# Note: removing one veth, removes the pair
	ip link delete $INITIATOR_INTERFACE
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
		        }${*:+,$*}
		      ]
		    }
		  ]
		}
	JSON
}
