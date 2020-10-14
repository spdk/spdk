NVMF_PORT=4420
NVMF_SECOND_PORT=4421
NVMF_THIRD_PORT=4422
NVMF_IP_PREFIX="192.168.100"
NVMF_IP_LEAST_ADDR=8
NVMF_TCP_IP_ADDRESS="127.0.0.1"
NVMF_TRANSPORT_OPTS=""
NVMF_SERIAL=SPDK00000000000001

function build_nvmf_app_args() {
	if [ $SPDK_RUN_NON_ROOT -eq 1 ]; then
		NVMF_APP=(sudo -u "$USER" "${NVMF_APP[@]}")
		NVMF_APP+=(-i "$NVMF_APP_SHM_ID" -e 0xFFFF)
	else
		NVMF_APP+=(-i "$NVMF_APP_SHM_ID" -e 0xFFFF)
	fi
}

: ${NVMF_APP_SHM_ID="0"}
export NVMF_APP_SHM_ID
build_nvmf_app_args

have_pci_nics=0

function rxe_cfg() {
	"$rootdir/scripts/rxe_cfg_small.sh" "$@"
}

function load_ib_rdma_modules() {
	if [ $(uname) != Linux ]; then
		return 0
	fi

	modprobe ib_cm
	modprobe ib_core
	# Newer kernels do not have the ib_ucm module
	modprobe ib_ucm || true
	modprobe ib_umad
	modprobe ib_uverbs
	modprobe iw_cm
	modprobe rdma_cm
	modprobe rdma_ucm
}

function detect_soft_roce_nics() {
	rxe_cfg stop # make sure we run tests with a clean slate
	rxe_cfg start
}

# args 1 and 2 represent the grep filters for finding our NICS.
# subsequent args are all drivers that should be loaded if we find these NICs.
# Those drivers should be supplied in the correct order.
function detect_nics_and_probe_drivers() {
	NIC_VENDOR="$1"
	NIC_CLASS="$2"

	nvmf_nic_bdfs=$(lspci | grep Ethernet | grep "$NIC_VENDOR" | grep "$NIC_CLASS" | awk -F ' ' '{print "0000:"$1}')

	if [ -z "$nvmf_nic_bdfs" ]; then
		return 0
	fi

	have_pci_nics=1
	if [ $# -ge 2 ]; then
		# shift out the first two positional arguments.
		shift 2
		# Iterate through the remaining arguments.
		for i; do
			modprobe "$i"
		done
	fi
}

function detect_pci_nics() {

	if ! hash lspci; then
		return 0
	fi

	detect_nics_and_probe_drivers "Mellanox" "ConnectX-4" "mlx4_core" "mlx4_ib" "mlx4_en"
	detect_nics_and_probe_drivers "Mellanox" "ConnectX-5" "mlx5_core" "mlx5_ib"
	detect_nics_and_probe_drivers "Intel" "X722" "i40e" "i40iw"
	detect_nics_and_probe_drivers "Chelsio" "Unified Wire" "cxgb4" "iw_cxgb4"

	if [ "$have_pci_nics" -eq "0" ]; then
		return 0
	fi

	# Provide time for drivers to properly load.
	sleep 5
}

function detect_rdma_nics() {
	detect_pci_nics
	if [ "$have_pci_nics" -eq "0" ]; then
		detect_soft_roce_nics
	fi
}

function allocate_nic_ips() {
	((count = NVMF_IP_LEAST_ADDR))
	for nic_name in $(get_rdma_if_list); do
		ip="$(get_ip_address $nic_name)"
		if [[ -z $ip ]]; then
			ip addr add $NVMF_IP_PREFIX.$count/24 dev $nic_name
			ip link set $nic_name up
			((count = count + 1))
		fi
		# dump configuration for debug log
		ip addr show $nic_name
	done
}

function get_available_rdma_ips() {
	for nic_name in $(get_rdma_if_list); do
		get_ip_address $nic_name
	done
}

function get_rdma_if_list() {
	for nic_type in /sys/class/infiniband/*; do
		[[ -e "$nic_type" ]] || break
		for nic_name in /sys/class/infiniband/"$(basename ${nic_type})"/device/net/*; do
			[[ -e "$nic_name" ]] || break
			basename "$nic_name"
		done
	done
}

function get_ip_address() {
	interface=$1
	ip -o -4 addr show $interface | awk '{print $4}' | cut -d"/" -f1
}

function nvmfcleanup() {
	sync
	set +e
	for i in {1..20}; do
		modprobe -v -r nvme-$TEST_TRANSPORT
		if modprobe -v -r nvme-fabrics; then
			set -e
			return 0
		fi
		sleep 1
	done
	set -e

	# So far unable to remove the kernel modules. Try
	# one more time and let it fail.
	# Allow the transport module to fail for now. See Jim's comment
	# about the nvme-tcp module below.
	modprobe -v -r nvme-$TEST_TRANSPORT || true
	modprobe -v -r nvme-fabrics
}

function nvmf_veth_init() {
	NVMF_INITIATOR_IP=10.0.0.1
	NVMF_FIRST_TARGET_IP=10.0.0.2
	NVMF_SECOND_TARGET_IP=10.0.0.3
	NVMF_BRIDGE="nvmf_br"
	NVMF_INITIATOR_INTERFACE="nvmf_init_if"
	NVMF_INITIATOR_BRIDGE="nvmf_init_br"
	NVMF_TARGET_NAMESPACE="nvmf_tgt_ns"
	NVMF_TARGET_NS_CMD=(ip netns exec "$NVMF_TARGET_NAMESPACE")
	NVMF_TARGET_INTERFACE="nvmf_tgt_if"
	NVMF_TARGET_INTERFACE2="nvmf_tgt_if2"
	NVMF_TARGET_BRIDGE="nvmf_tgt_br"
	NVMF_TARGET_BRIDGE2="nvmf_tgt_br2"

	ip link set $NVMF_INITIATOR_BRIDGE nomaster || true
	ip link set $NVMF_TARGET_BRIDGE nomaster || true
	ip link set $NVMF_TARGET_BRIDGE2 nomaster || true
	ip link set $NVMF_INITIATOR_BRIDGE down || true
	ip link set $NVMF_TARGET_BRIDGE down || true
	ip link set $NVMF_TARGET_BRIDGE2 down || true
	ip link delete $NVMF_BRIDGE type bridge || true
	ip link delete $NVMF_INITIATOR_INTERFACE || true
	"${NVMF_TARGET_NS_CMD[@]}" ip link delete $NVMF_TARGET_INTERFACE || true
	"${NVMF_TARGET_NS_CMD[@]}" ip link delete $NVMF_TARGET_INTERFACE2 || true
	ip netns del $NVMF_TARGET_NAMESPACE || true

	trap 'nvmf_veth_fini; exit 1' SIGINT SIGTERM EXIT

	# Create network namespace
	ip netns add $NVMF_TARGET_NAMESPACE

	# Create veth (Virtual ethernet) interface pairs
	ip link add $NVMF_INITIATOR_INTERFACE type veth peer name $NVMF_INITIATOR_BRIDGE
	ip link add $NVMF_TARGET_INTERFACE type veth peer name $NVMF_TARGET_BRIDGE
	ip link add $NVMF_TARGET_INTERFACE2 type veth peer name $NVMF_TARGET_BRIDGE2

	# Associate veth interface pairs with network namespace
	ip link set $NVMF_TARGET_INTERFACE netns $NVMF_TARGET_NAMESPACE
	ip link set $NVMF_TARGET_INTERFACE2 netns $NVMF_TARGET_NAMESPACE

	# Allocate IP addresses
	ip addr add $NVMF_INITIATOR_IP/24 dev $NVMF_INITIATOR_INTERFACE
	"${NVMF_TARGET_NS_CMD[@]}" ip addr add $NVMF_FIRST_TARGET_IP/24 dev $NVMF_TARGET_INTERFACE
	"${NVMF_TARGET_NS_CMD[@]}" ip addr add $NVMF_SECOND_TARGET_IP/24 dev $NVMF_TARGET_INTERFACE2

	# Link up veth interfaces
	ip link set $NVMF_INITIATOR_INTERFACE up
	ip link set $NVMF_INITIATOR_BRIDGE up
	ip link set $NVMF_TARGET_BRIDGE up
	ip link set $NVMF_TARGET_BRIDGE2 up
	"${NVMF_TARGET_NS_CMD[@]}" ip link set $NVMF_TARGET_INTERFACE up
	"${NVMF_TARGET_NS_CMD[@]}" ip link set $NVMF_TARGET_INTERFACE2 up
	"${NVMF_TARGET_NS_CMD[@]}" ip link set lo up

	# Create a bridge
	ip link add $NVMF_BRIDGE type bridge
	ip link set $NVMF_BRIDGE up

	# Add veth interfaces to the bridge
	ip link set $NVMF_INITIATOR_BRIDGE master $NVMF_BRIDGE
	ip link set $NVMF_TARGET_BRIDGE master $NVMF_BRIDGE
	ip link set $NVMF_TARGET_BRIDGE2 master $NVMF_BRIDGE

	# Accept connections from veth interface
	iptables -I INPUT 1 -i $NVMF_INITIATOR_INTERFACE -p tcp --dport $NVMF_PORT -j ACCEPT

	# Verify connectivity
	ping -c 1 $NVMF_FIRST_TARGET_IP
	ping -c 1 $NVMF_SECOND_TARGET_IP
	"${NVMF_TARGET_NS_CMD[@]}" ping -c 1 $NVMF_INITIATOR_IP

	NVMF_APP=("${NVMF_TARGET_NS_CMD[@]}" "${NVMF_APP[@]}")
}

function nvmf_veth_fini() {
	# Cleanup bridge, veth interfaces, and network namespace
	# Note: removing one veth removes the pair
	ip link set $NVMF_INITIATOR_BRIDGE nomaster
	ip link set $NVMF_TARGET_BRIDGE nomaster
	ip link set $NVMF_TARGET_BRIDGE2 nomaster
	ip link set $NVMF_INITIATOR_BRIDGE down
	ip link set $NVMF_TARGET_BRIDGE down
	ip link set $NVMF_TARGET_BRIDGE2 down
	ip link delete $NVMF_BRIDGE type bridge
	ip link delete $NVMF_INITIATOR_INTERFACE
	"${NVMF_TARGET_NS_CMD[@]}" ip link delete $NVMF_TARGET_INTERFACE
	"${NVMF_TARGET_NS_CMD[@]}" ip link delete $NVMF_TARGET_INTERFACE2
	ip netns del $NVMF_TARGET_NAMESPACE
}

function nvmftestinit() {
	if [ -z $TEST_TRANSPORT ]; then
		echo "transport not specified - use --transport= to specify"
		return 1
	fi
	if [ "$TEST_MODE" == "iso" ]; then
		$rootdir/scripts/setup.sh
		if [ "$TEST_TRANSPORT" == "rdma" ]; then
			rdma_device_init
		fi
	fi

	NVMF_TRANSPORT_OPTS="-t $TEST_TRANSPORT"
	if [ "$TEST_TRANSPORT" == "rdma" ]; then
		RDMA_IP_LIST=$(get_available_rdma_ips)
		NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
		NVMF_SECOND_TARGET_IP=$(echo "$RDMA_IP_LIST" | tail -n +2 | head -n 1)
		if [ -z $NVMF_FIRST_TARGET_IP ]; then
			echo "no NIC for nvmf test"
			exit 0
		fi
	elif [ "$TEST_TRANSPORT" == "tcp" ]; then
		nvmf_veth_init
		NVMF_TRANSPORT_OPTS="$NVMF_TRANSPORT_OPTS -o"
	fi

	# currently we run the host/perf test for TCP even on systems without kernel nvme-tcp
	#  support; that's fine since the host/perf test uses the SPDK initiator
	# maybe later we will enforce modprobe to succeed once we have systems in the test pool
	#  with nvme-tcp kernel support - but until then let this pass so we can still run the
	#  host/perf test with the tcp transport
	modprobe nvme-$TEST_TRANSPORT || true
}

function nvmfappstart() {
	timing_enter start_nvmf_tgt
	"${NVMF_APP[@]}" "$@" &
	nvmfpid=$!
	trap 'process_shm --id $NVMF_APP_SHM_ID; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $nvmfpid
	timing_exit start_nvmf_tgt
}

function nvmftestfini() {
	nvmfcleanup || :
	if [ -n "$nvmfpid" ]; then
		killprocess $nvmfpid
	fi
	if [ "$TEST_MODE" == "iso" ]; then
		$rootdir/scripts/setup.sh reset
		if [ "$TEST_TRANSPORT" == "rdma" ]; then
			rdma_device_init
		elif [ "$TEST_TRANSPORT" == "tcp" ]; then
			nvmf_veth_fini
		fi
	fi
}

function rdma_device_init() {
	load_ib_rdma_modules
	detect_rdma_nics
	allocate_nic_ips
}

function revert_soft_roce() {
	rxe_cfg stop
}

function check_ip_is_soft_roce() {
	if [ "$TEST_TRANSPORT" != "rdma" ]; then
		return 0
	fi
	rxe_cfg status rxe | grep -wq "$1"
}

function nvme_connect() {
	local init_count
	init_count=$(nvme list | wc -l)

	if ! nvme connect "$@"; then return $?; fi

	for i in $(seq 1 10); do
		if [ $(nvme list | wc -l) -gt $init_count ]; then
			return 0
		else
			sleep 1s
		fi
	done
	return 1
}

function get_nvme_devs() {
	local dev rest

	nvmes=()
	while read -r dev rest; do
		if [[ $dev == /dev/nvme* ]]; then
			nvmes+=("$dev")
		fi
		if [[ $1 == print ]]; then
			echo "$dev $rest"
		fi
	done < <(nvme list)
	((${#nvmes[@]})) || return 1
	echo "${#nvmes[@]}" >&2
}

function gen_nvmf_target_json() {
	local subsystem config=()

	for subsystem in "${@:-1}"; do
		config+=(
			"$(
				cat <<- EOF
					{
					  "params": {
					    "name": "Nvme$subsystem",
					    "trtype": "$TEST_TRANSPORT",
					    "traddr": "$NVMF_FIRST_TARGET_IP",
					    "adrfam": "ipv4",
					    "trsvcid": "$NVMF_PORT",
					    "subnqn": "nqn.2016-06.io.spdk:cnode$subsystem",
					    "hostnqn": "nqn.2016-06.io.spdk:host$subsystem"
					  },
					  "method": "bdev_nvme_attach_controller"
					}
				EOF
			)"
		)
	done
	jq . <<- JSON
		{
		  "subsystems": [
		    {
		      "subsystem": "bdev",
		      "config": [
		        $(
		IFS=","
		printf '%s\n' "${config[*]}"
		)
		      ]
		    }
		  ]
		}
	JSON
}
