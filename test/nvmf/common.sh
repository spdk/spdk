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
		# We assume that test script is started from sudo
		NVMF_APP=(sudo -E -u $SUDO_USER "LD_LIBRARY_PATH=$LD_LIBRARY_PATH" "${NVMF_APP[@]}")
	fi
	NVMF_APP+=(-i "$NVMF_APP_SHM_ID" -e 0xFFFF)

	if [ -n "$SPDK_HUGE_DIR" ]; then
		NVMF_APP+=(--huge-dir "$SPDK_HUGE_DIR")
	elif [ $SPDK_RUN_NON_ROOT -eq 1 ]; then
		echo "In non-root test mode you have to set SPDK_HUGE_DIR variable." >&2
		echo "For example:" >&2
		echo "sudo mkdir /mnt/spdk_hugetlbfs" >&2
		echo "sudo chown ${SUDO_USER}: /mnt/spdk_hugetlbfs" >&2
		echo "export SPDK_HUGE_DIR=/mnt/spdk_hugetlbfs" >&2
		return 1
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
			if [[ $i == irdma ]]; then
				# Our tests don't play well with iWARP protocol. Make sure we use RoCEv2 instead.
				if [[ -e /sys/module/irdma/parameters/roce_ena ]]; then
					# reload the module to re-init the rdma devices
					(($(< /sys/module/irdma/parameters/roce_ena) != 1)) && modprobe -r irdma
				fi
				modprobe "$i" roce_ena=1
			else
				modprobe "$i"
			fi
		done
	fi
}

function pci_rdma_switch() {
	local driver=$1

	local -a driver_args=()
	driver_args+=("Mellanox ConnectX-4 mlx5_core mlx5_ib")
	driver_args+=("Mellanox ConnectX-5 mlx5_core mlx5_ib")
	driver_args+=("Intel E810 ice irdma")
	driver_args+=("Intel X722 i40e i40iw")
	driver_args+=("Chelsio \"Unified Wire\" cxgb4 iw_cxgb4")

	case $driver in
		mlx5_ib)
			detect_nics_and_probe_drivers ${driver_args[0]}
			detect_nics_and_probe_drivers ${driver_args[1]}
			;;
		irdma)
			detect_nics_and_probe_drivers ${driver_args[2]}
			;;
		i40iw)
			detect_nics_and_probe_drivers ${driver_args[3]}
			;;
		iw_cxgb4)
			detect_nics_and_probe_drivers ${driver_args[4]}
			;;
		*)
			for d in "${driver_args[@]}"; do
				detect_nics_and_probe_drivers $d
			done
			;;
	esac
}

function pci_tcp_switch() {
	local driver=$1

	local -a driver_args=()
	driver_args+=("Intel E810 ice")

	case $driver in
		ice)
			detect_nics_and_probe_drivers ${driver_args[0]}
			;;
		*)
			for d in "${driver_args[@]}"; do
				detect_nics_and_probe_drivers $d
			done
			;;
	esac
}

function detect_pci_nics() {

	if ! hash lspci; then
		return 0
	fi

	local nic_drivers
	local found_drivers

	if [[ -z "$TEST_TRANSPORT" ]]; then
		TEST_TRANSPORT=$SPDK_TEST_NVMF_TRANSPORT
	fi

	if [[ "$TEST_TRANSPORT" == "rdma" ]]; then
		nic_drivers="mlx5_ib|irdma|i40iw|iw_cxgb4"

		# Try to find RDMA drivers which are already loded and try to
		# use only it's associated NICs, without probing all drivers.
		found_drivers=$(lsmod | grep -Eo $nic_drivers | sort -u)
		for d in $found_drivers; do
			pci_rdma_switch $d
		done

		# In case lsmod reported driver, but lspci does not report
		# physical NICs - fall back to old approach any try to
		# probe all compatible NICs.
		((have_pci_nics == 0)) && pci_rdma_switch "default"

	elif [[ "$TEST_TRANSPORT" == "tcp" ]]; then
		nic_drivers="ice"
		found_drivers=$(lsmod | grep -Eo $nic_drivers | sort -u)
		for d in $found_drivers; do
			pci_tcp_switch $d
		done
		((have_pci_nics == 0)) && pci_tcp_switch "default"
	fi

	# Use softroce if everything else failed.
	((have_pci_nics == 0)) && return 0

	# Provide time for drivers to properly load.
	sleep 5
}

function detect_transport_nics() {
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
	rxe_cfg rxe-net
}

function get_tcp_if_list_by_driver() {
	local driver
	driver=${1:-ice}

	shopt -s nullglob
	tcp_if_list=(/sys/bus/pci/drivers/$driver/0000*/net/*)
	shopt -u nullglob
	printf '%s\n' "${tcp_if_list[@]##*/}"
}

function get_ip_address() {
	interface=$1
	ip -o -4 addr show $interface | awk '{print $4}' | cut -d"/" -f1
}

function nvmfcleanup() {
	sync

	if [ "$TEST_TRANSPORT" == "tcp" ] || [ "$TEST_TRANSPORT" == "rdma" ]; then
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
	fi
}

function nvmf_veth_init() {
	NVMF_INITIATOR_IP=10.0.0.1
	NVMF_FIRST_TARGET_IP=10.0.0.2
	NVMF_SECOND_TARGET_IP=10.0.0.3
	NVMF_BRIDGE="nvmf_br"
	NVMF_INITIATOR_INTERFACE="nvmf_init_if"
	NVMF_INITIATOR_BRIDGE="nvmf_init_br"
	NVMF_TARGET_NAMESPACE="nvmf_tgt_ns_spdk"
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
	iptables -A FORWARD -i $NVMF_BRIDGE -o $NVMF_BRIDGE -j ACCEPT

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
	remove_spdk_ns
}

function nvmf_tcp_init() {
	NVMF_INITIATOR_IP=10.0.0.1
	NVMF_FIRST_TARGET_IP=10.0.0.2
	TCP_INTERFACE_LIST=($(get_tcp_if_list_by_driver))
	if ((${#TCP_INTERFACE_LIST[@]} == 0)) || [ "$TEST_MODE" == "iso" ]; then
		nvmf_veth_init
		return 0
	fi

	# We need two net devs at minimum
	((${#TCP_INTERFACE_LIST[@]} > 1))

	NVMF_TARGET_INTERFACE=${TCP_INTERFACE_LIST[0]}
	NVMF_INITIATOR_INTERFACE=${TCP_INTERFACE_LIST[1]}

	# Skip case nvmf_multipath in nvmf_tcp_init(), it will be covered by nvmf_veth_init().
	NVMF_SECOND_TARGET_IP=""

	NVMF_TARGET_NAMESPACE="${NVMF_TARGET_INTERFACE}_ns_spdk"
	NVMF_TARGET_NS_CMD=(ip netns exec "$NVMF_TARGET_NAMESPACE")
	ip -4 addr flush $NVMF_TARGET_INTERFACE || true
	ip -4 addr flush $NVMF_INITIATOR_INTERFACE || true

	# Create network namespace
	ip netns add $NVMF_TARGET_NAMESPACE

	# Associate phy interface pairs with network namespace
	ip link set $NVMF_TARGET_INTERFACE netns $NVMF_TARGET_NAMESPACE

	# Allocate IP addresses
	ip addr add $NVMF_INITIATOR_IP/24 dev $NVMF_INITIATOR_INTERFACE
	"${NVMF_TARGET_NS_CMD[@]}" ip addr add $NVMF_FIRST_TARGET_IP/24 dev $NVMF_TARGET_INTERFACE

	# Link up phy interfaces
	ip link set $NVMF_INITIATOR_INTERFACE up

	"${NVMF_TARGET_NS_CMD[@]}" ip link set $NVMF_TARGET_INTERFACE up
	"${NVMF_TARGET_NS_CMD[@]}" ip link set lo up

	# Accept connections from phy interface
	iptables -I INPUT 1 -i $NVMF_INITIATOR_INTERFACE -p tcp --dport $NVMF_PORT -j ACCEPT

	# Verify connectivity
	ping -c 1 $NVMF_FIRST_TARGET_IP
	"${NVMF_TARGET_NS_CMD[@]}" ping -c 1 $NVMF_INITIATOR_IP

	NVMF_APP=("${NVMF_TARGET_NS_CMD[@]}" "${NVMF_APP[@]}")
}

function nvmf_tcp_fini() {
	if [[ "$NVMF_TARGET_NAMESPACE" == "nvmf_tgt_ns" ]]; then
		nvmf_veth_fini
		return 0
	fi
	remove_spdk_ns
	ip -4 addr flush $NVMF_INITIATOR_INTERFACE || :
}

function nvmftestinit() {
	if [ -z $TEST_TRANSPORT ]; then
		echo "transport not specified - use --transport= to specify"
		return 1
	fi

	trap 'process_shm --id $NVMF_APP_SHM_ID || :; nvmftestfini' SIGINT SIGTERM EXIT

	if [ "$TEST_MODE" == "iso" ]; then
		$rootdir/scripts/setup.sh
		if [[ "$TEST_TRANSPORT" == "rdma" ]]; then
			rdma_device_init
		fi
		if [[ "$TEST_TRANSPORT" == "tcp" ]]; then
			tcp_device_init
		fi
	fi

	NVMF_TRANSPORT_OPTS="-t $TEST_TRANSPORT"
	if [[ "$TEST_TRANSPORT" == "rdma" ]]; then
		RDMA_IP_LIST=$(get_available_rdma_ips)
		NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
		NVMF_SECOND_TARGET_IP=$(echo "$RDMA_IP_LIST" | tail -n +2 | head -n 1)
		if [ -z $NVMF_FIRST_TARGET_IP ]; then
			echo "no RDMA NIC for nvmf test"
			exit 0
		fi
	elif [[ "$TEST_TRANSPORT" == "tcp" ]]; then
		remove_spdk_ns
		nvmf_tcp_init
		NVMF_TRANSPORT_OPTS="$NVMF_TRANSPORT_OPTS -o"
	fi

	if [ "$TEST_TRANSPORT" == "tcp" ] || [ "$TEST_TRANSPORT" == "rdma" ]; then
		# currently we run the host/perf test for TCP even on systems without kernel nvme-tcp
		#  support; that's fine since the host/perf test uses the SPDK initiator
		# maybe later we will enforce modprobe to succeed once we have systems in the test pool
		#  with nvme-tcp kernel support - but until then let this pass so we can still run the
		#  host/perf test with the tcp transport
		modprobe nvme-$TEST_TRANSPORT || true
	fi
}

function nvmfappstart() {
	timing_enter start_nvmf_tgt
	"${NVMF_APP[@]}" "$@" &
	nvmfpid=$!
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
		if [[ "$TEST_TRANSPORT" == "rdma" ]]; then
			rdma_device_init
		fi
	fi
	if [[ "$TEST_TRANSPORT" == "tcp" ]]; then
		nvmf_tcp_fini
	fi
}

function rdma_device_init() {
	load_ib_rdma_modules
	detect_transport_nics
	allocate_nic_ips
}

function tcp_device_init() {
	detect_transport_nics
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
	local dev _

	while read -r dev _; do
		if [[ $dev == /dev/nvme* ]]; then
			echo "$dev"
		fi
	done < <(nvme list)
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
					    "hostnqn": "nqn.2016-06.io.spdk:host$subsystem",
					    "hdgst": ${hdgst:-false},
					    "ddgst": ${ddgst:-false}
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
			{
			  "method": "bdev_nvme_set_options",
			  "params": {
				"action_on_timeout": "none",
				"timeout_us": 0,
				"retry_count": 4,
				"arbitration_burst": 0,
				"low_priority_weight": 0,
				"medium_priority_weight": 0,
				"high_priority_weight": 0,
				"nvme_adminq_poll_period_us": 10000,
				"keep_alive_timeout_ms" : 10000,
				"nvme_ioq_poll_period_us": 0,
				"io_queue_requests": 0,
				"delay_cmd_submit": true
			  }
			},
		        $(
		IFS=","
		printf '%s\n' "${config[*]}"
		),
			{
			  "method": "bdev_wait_for_examine"
			}
		      ]
		    }
		  ]
		}
	JSON
}

function remove_spdk_ns() {
	local ns
	while read -r ns _; do
		[[ $ns == *_spdk ]] || continue
		ip netns delete "$ns"
	done < <(ip netns list)
	# Let it settle
	sleep 1
}
