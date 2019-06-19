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
if [ $SPDK_TEST_VPP -eq 1 ]; then
	ISCSI_APP+=" -L sock_vpp"
fi
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

	$TARGET_NS_CMD ip link set $TARGET_INTERFACE up

	if [ "$1" == "posix" ]; then
		$TARGET_NS_CMD ip link set lo up
		$TARGET_NS_CMD ip addr add $TARGET_IP/24 dev $TARGET_INTERFACE

		# Verify connectivity
		ping -c 1 $TARGET_IP
		ip netns exec $TARGET_NAMESPACE ping -c 1 $INITIATOR_IP
	else
		start_vpp
	fi
}

function cleanup_veth_interfaces() {
	# $1 = test type (posix/vpp)
	if [ "$1" == "vpp" ]; then
		kill_vpp
	fi

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

function gdb_attach() {
	gdb -q --batch \
		-ex 'handle SIGHUP nostop pass' \
		-ex 'handle SIGQUIT nostop pass' \
		-ex 'handle SIGPIPE nostop pass' \
		-ex 'handle SIGALRM nostop pass' \
		-ex 'handle SIGTERM nostop pass' \
		-ex 'handle SIGUSR1 nostop pass' \
		-ex 'handle SIGUSR2 nostop pass' \
		-ex 'handle SIGCHLD nostop pass' \
		-ex 'set print thread-events off' \
		-ex 'cont' \
		-ex 'thread apply all bt' \
		-ex 'quit' \
		--tty=/dev/stdout \
		-p $1
}

function start_vpp() {
	# We need to make sure that posix side doesn't send jumbo packets while
	# for VPP side maximal size of MTU for TCP is 1460 and tests doesn't work
	# stable with larger packets
	MTU=1460
	ip link set dev $INITIATOR_INTERFACE mtu $MTU
	ethtool -K $INITIATOR_INTERFACE tso off
	ethtool -k $INITIATOR_INTERFACE

	# Start VPP process in SPDK target network namespace
	$TARGET_NS_CMD vpp \
		unix { nodaemon cli-listen /run/vpp/cli.sock } \
		dpdk { no-pci } \
		session { evt_qs_memfd_seg } \
		socksvr { socket-name /run/vpp-api.sock } \
		plugins { \
			plugin default { disable } \
			plugin dpdk_plugin.so { enable } \
		} &

	vpp_pid=$!
	echo "VPP Process pid: $vpp_pid"

	gdb_attach $vpp_pid &

	# Wait until VPP starts responding
	xtrace_disable
	counter=40
	while [ $counter -gt 0 ] ; do
		vppctl show version &> /dev/null && break
		counter=$(( $counter - 1 ))
		sleep 0.5
	done
	xtrace_restore
	if [ $counter -eq 0 ] ; then
		return 1
	fi

	# Setup host interface
	vppctl create host-interface name $TARGET_INTERFACE
	VPP_TGT_INT="host-$TARGET_INTERFACE"
	vppctl set interface state $VPP_TGT_INT up
	vppctl set interface ip address $VPP_TGT_INT $TARGET_IP/24
	vppctl set interface mtu $MTU $VPP_TGT_INT

	vppctl show interface

	# Disable session layer
	# NOTE: VPP net framework should enable it itself.
	vppctl session disable

	# Verify connectivity
	vppctl show int addr
	ip addr show $INITIATOR_INTERFACE
	ip netns exec $TARGET_NAMESPACE ip addr show $TARGET_INTERFACE
	sleep 3
	ping -c 1 $TARGET_IP -s $(( $MTU - 28 )) -M do
	vppctl ping $INITIATOR_IP repeat 1 size $(( $MTU - (28 + 8) )) verbose
}

function kill_vpp() {
	vppctl delete host-interface name $TARGET_INTERFACE

	# Dump VPP configuration before kill
	vppctl show api clients
	vppctl show session
	vppctl show errors

	killprocess $vpp_pid
}
