set -e

BASE_DIR=$(readlink -f $(dirname $0))

MAKE="make -j$(( $(nproc)  * 2 ))"

# Default running dir -> spdk/..
[[ -z "$TEST_DIR" ]] && TEST_DIR=$BASE_DIR/../../../../

TEST_DIR="$(mkdir -p $TEST_DIR && cd $TEST_DIR && echo $PWD)"

SPDK_SRC_DIR=$TEST_DIR/spdk
SPDK_BUILD_DIR=$BASE_DIR/../../../
VHOST_APP=$SPDK_BUILD_DIR/app/vhost/vhost

SPDK_VHOST_SCSI_TEST_DIR=$TEST_DIR/vhost

# QEMU source and build folders
[[ -z "$QEMU_SRC_DIR" ]] && QEMU_SRC_DIR="$TEST_DIR/qemu"
QEMU_BUILD_DIR="$QEMU_SRC_DIR/build"

# DPDK source and build folders
[[ -z "$DPDK_SRC_DIR" ]] && DPDK_SRC_DIR="$TEST_DIR/dpdk"

# SSH key file
[[ -z "$SPDK_VHOST_SSH_KEY_FILE" ]] && SPDK_VHOST_SSH_KEY_FILE="$HOME/.ssh/spdk_vhost_id_rsa"
if [[ ! -e "$SPDK_VHOST_SSH_KEY_FILE" ]]; then
	echo "Could not find SSH key file $SPDK_VHOST_SSH_KEY_FILE"
	exit 1
fi
echo "Using SSH key file $SPDK_VHOST_SSH_KEY_FILE"

VM_CNT=0

VM_BASE_DIR="$TEST_DIR/vms"


INSTALL_DIR="$TEST_DIR/root"

mkdir -p $TEST_DIR

. $BASE_DIR/autotest.config

RPC_PORT=5260

# Trace flag is optional, if it wasn't set earlier - disable it after sourcing
# autotest_common.sh
if [[ $- =~ x ]]; then
	source $SPDK_BUILD_DIR/scripts/autotest_common.sh
else
	source $SPDK_BUILD_DIR/scripts/autotest_common.sh
	set +x
fi

function error()
{
	echo "==========="
	echo -e "ERROR: $@"
	echo "==========="
	return 1
}

# Build QEMU from $QEMU_SRC_DIR directory in $QEMU_BUILD_DIR and install in $INSTALL_DIR
#
# NOTE: It will use CCACHE if detected.
# FIXME: quiet configuration an build
#
function qemu_build_and_install()
{
	mkdir -p $QEMU_BUILD_DIR

        cd $QEMU_SRC_DIR
        make clean
        cd $QEMU_BUILD_DIR

	echo "INFO: Configuring QEMU from source in $QEMU_SRC_DIR"
	if type ccache > /dev/null 2>&1; then
		echo "INFO: CCACHE detected"
		export CC="ccache cc"
		export CXX="ccache c++"
		export CPP="ccache cpp"
	else
		echo "INFO: CCACHE NOT detected - consider installing."
	fi

	$QEMU_SRC_DIR/configure --prefix=$INSTALL_DIR \
		--target-list="x86_64-softmmu" \
		--enable-kvm --enable-linux-aio --enable-numa

	echo "INFO: Compiling and installing QEMU in $INSTALL_DIR"
	$MAKE install
	echo "INFO: DONE"
}

# Build SPDK using $SPDK_SRC as source directory.
function spdk_build_and_install()
{
	echo "INFO: Building SPDK"
	echo "checking dependencies..."
	case `uname` in
		FreeBSD)
			local dpdk_target=x86_64-native-bsdapp-clang
			;;
		Linux)
			local dpdk_target=x86_64-native-linuxapp-gcc
			;;
		*)
			echo "Unknown OS in $0"
			exit 1
			;;
	esac

	if [[ ! -x $DPDK_SRC_DIR/$dpdk_target ]]; then
		echo "ERROR: can't find $DPDK_SRC_DIR/$dpdk_target"
		exit 1
	fi

	cd $SPDK_BUILD_DIR

	$MAKE clean
	$MAKE DPDK_DIR=$DPDK_SRC_DIR

	echo "INFO: DONE"
}

function spdk_vhost_run()
{
	local vhost_log_file="$SPDK_VHOST_SCSI_TEST_DIR/vhost.log"
	local vhost_pid_file="$SPDK_VHOST_SCSI_TEST_DIR/vhost.pid"
	local vhost_socket="$SPDK_VHOST_SCSI_TEST_DIR/usvhost"
	local vhost_conf_template="$BASE_DIR/vhost.conf.in"
	local vhost_conf_file="$BASE_DIR/vhost.conf"
	echo "INFO: starting vhost app in background"
	[[ -r "$vhost_pid_file" ]] && spdk_vhost_kill
	[[ -d $SPDK_VHOST_SCSI_TEST_DIR ]] && rm -f $SPDK_VHOST_SCSI_TEST_DIR/*
	mkdir -p $SPDK_VHOST_SCSI_TEST_DIR

	if [[ ! -x $VHOST_APP ]]; then
		error "application not found: $VHOST_APP"
		return 1
	fi

	if [[ -z "$vhost_reactor_mask" ]] || [[ -z "$vhost_master_core" ]]; then
		error "Parameters vhost_reactor_mask or vhost_master_core not found in autotest.config file"
		return 1
	fi

	cp $vhost_conf_template $vhost_conf_file
	$BASE_DIR/../../../scripts/gen_nvme.sh >> $vhost_conf_file

	local cmd="$VHOST_APP -m $vhost_reactor_mask -p $vhost_master_core -c $vhost_conf_file"

	echo "INFO: Loging to:   $vhost_log_file"
	echo "INFO: Config file: $vhost_conf_file"
	echo "INFO: Socket:      $vhost_socket"
	echo "INFO: Command:     $cmd"

	cd $SPDK_VHOST_SCSI_TEST_DIR; $cmd &
	vhost_pid=$!
	echo $vhost_pid > $vhost_pid_file

	echo "INFO: waiting for app to run..."
	waitforlisten "$vhost_pid" ${RPC_PORT}
	echo "INFO: vhost started - pid=$vhost_pid"

	rm $vhost_conf_file
}

function spdk_vhost_kill()
{
	local vhost_pid_file="$SPDK_VHOST_SCSI_TEST_DIR/vhost.pid"

	if [[ ! -r $vhost_pid_file ]]; then
		echo "WARN: no vhost pid file found"
		return 0
	fi

	local vhost_pid="$(cat $vhost_pid_file)"
	echo "INFO: killing vhost (PID $vhost_pid) app"

	if /bin/kill -INT $vhost_pid >/dev/null; then
		echo "INFO: sent SIGINT to vhost app - waiting 60 seconds to exit"
		for ((i=0; i<60; i++)); do
			if /bin/kill -0 $vhost_pid; then
				echo "."
				sleep 1
			else
				break
			fi
		done
		if /bin/kill -0 $vhost_pid; then
			echo "ERROR: vhost was NOT killed - sending SIGKILL"
			/bin/kill -KILL $vhost_pid
			rm $vhost_pid_file
			return 1
		fi
	elif /bin/kill -0 $vhost_pid; then
		error "vhost NOT killed - you need to kill it manually"
		return 1
	else
		echo "INFO: vhost was no running"
	fi

	rm $vhost_pid_file
}

###
# Mgmt functions
###

function assert_number()
{
	[[ "$1" =~ [0-9]+ ]] && return 0

	echo "${FUNCNAME[1]}() - ${BASH_LINENO[1]}: ERROR Invalid or missing paramter: need number but got '$1'" > /dev/stderr
	return 1;
}

# Helper to validate VM number
# param $1 VM number
#
function vm_num_is_valid()
{
	[[ "$1" =~ [0-9]+ ]] && return 0

	echo "${FUNCNAME[1]}() - ${BASH_LINENO[1]}: ERROR Invalid or missing paramter: vm number '$1'" > /dev/stderr
	return 1;
}


# Print network socket for given VM number
# param $1 virtual machine number
#
function vm_ssh_socket()
{
	vm_num_is_valid $1 || return 1
	local vm_dir="$VM_BASE_DIR/$1"

	cat $vm_dir/ssh_socket
}

function vm_fio_socket()
{
	vm_num_is_valid $1 || return 1
	local vm_dir="$VM_BASE_DIR/$1"

	cat $vm_dir/fio_socket
}

# Execute ssh command on given VM
# param $1 virtual machine number
#
function vm_ssh()
{
	vm_num_is_valid $1 || return 1
	local ssh_config="$VM_BASE_DIR/ssh_config"
	if [[ ! -f $ssh_config ]]; then
		(
		echo "Host *"
		echo "	ControlPersist=10m"
		echo "	ConnectTimeout=2"
		echo "	Compression=no"
		echo "	ControlMaster=auto"
		echo "	UserKnownHostsFile=/dev/null"
		echo "	StrictHostKeyChecking=no"
		echo "	User root"
		echo "  ControlPath=$VM_BASE_DIR/%r@%h:%p.ssh"
		echo ""
		) > $ssh_config
	fi

	local ssh_cmd="ssh -i $SPDK_VHOST_SSH_KEY_FILE -F $ssh_config \
		-p $(vm_ssh_socket $1) 127.0.0.1"

	shift
	$ssh_cmd "$@"
}

# check if specified VM is running
# param $1 VM num
function vm_is_running()
{
	vm_num_is_valid $1 || return 1
	local vm_dir="$VM_BASE_DIR/$1"

	if [[ ! -r $vm_dir/qemu.pid ]]; then
		return 1
	fi

	local vm_pid="$(cat $vm_dir/qemu.pid)"

	if /bin/kill -0 $vm_pid; then
		return 0
	else
		if [[ $EUID -ne 0 ]]; then
			echo "WARNING: not root - assuming we running since can't be checked"
			return 0
		fi

		# not running - remove pid file
		rm $vm_dir/qemu.pid
		return 1
	fi
}

# check if specified VM is running
# param $1 VM num
function vm_os_booted()
{
	vm_num_is_valid $1 || return 1
	local vm_dir="$VM_BASE_DIR/$1"

	if [[ ! -r $vm_dir/qemu.pid ]]; then
		error "VM $1 is not running"
		return 1
	fi

	if ! vm_ssh $1 "true" 2>/dev/null; then
		return 1
	fi

	return 0
}


# Shutdown given VM
# param $1 virtual machine number
# return non-zero in case of error.
function vm_shutdown()
{
	vm_num_is_valid $1 || return 1
	local vm_dir="$VM_BASE_DIR/$1"
	if [[ ! -d "$vm_dir" ]]; then
		error "VM$1 ($vm_dir) not exist - setup it first"
		return 1
	fi

	if ! vm_is_running $1; then
		echo "INFO: VM$1 ($vm_dir) is not running"
		return 0
	fi

	# Temporarily disabling exit flag for next ssh command, since it will
	# "fail" due to shutdown
	echo "Shutting down virtual machine $vm_dir"
	set +e
	vm_ssh $1 "nohup sh -c 'shutdown -h -P now'" || true
	echo "INFO: VM$1 is shutting down - wait a while to complete"
	set -e
}

# Kill given VM
# param $1 virtual machine number
#
function vm_kill()
{
	vm_num_is_valid $1 || return 1
	local vm_dir="$VM_BASE_DIR/$1"

	if [[ ! -r $vm_dir/qemu.pid ]]; then
		#echo "WARN: VM$1 pid not found - not killing"
		return 0
	fi

	local vm_pid="$(cat $vm_dir/qemu.pid)"

	echo "Killing virtual machine $vm_dir (pid=$vm_pid)"
	# First kill should fail, second one must fail
	if /bin/kill $vm_pid; then
		echo "INFO: process $vm_pid killed"
		rm $vm_dir/qemu.pid
	elif vm_is_running $1; then
		erorr "Process $vm_pid NOT killed"
		return 1
	fi
}

# Kills all VM in $VM_BASE_DIR
#
function vm_kill_all()
{
	for vm in $VM_BASE_DIR/[0-9]*; do
		vm_kill $(basename $vm)
	done
}

# Shutdown all VM in $VM_BASE_DIR
#
function vm_shutdown_all()
{
	for vm in $VM_BASE_DIR/[0-9]*; do
		vm_shutdown $(basename $vm)
	done

	echo "INFO: Waiting for VMs to shutdown..."
	timeo=10
	while [[ $timeo -gt 0 ]]; do
		all_vms_down=1
		for vm in $VM_BASE_DIR/[0-9]*; do
			if /bin/kill -0 "$(cat $vm/qemu.pid)"; then
				all_vms_down=0
				break
			fi
		done

		if [[ $all_vms_down == 1 ]]; then
			echo "INFO: All VMs successfully shut down"
			return 0
		fi

		((timeo-=1))
		sleep 1
	done

	echo "ERROR: VMs were NOT shutdown properly - sending SIGKILL"
	for vm in $VM_BASE_DIR/[0-9]*; do
		/bin/kill -KILL "$(cat $vm/qemu.pid)"
	done
	return 1
}

function vm_setup()
{
	local OPTIND optchar a

	local os=""
	local qemu_args=""
	local disk_type=NOT_DEFINED
	local disks=""
	local raw_cache=""
	local force_vm=""
	while getopts ':-:' optchar; do
		case "$optchar" in
			-)
			case "$OPTARG" in
				os=*) local os="${OPTARG#*=}" ;;
				os-mode=*) local os_mode="${OPTARG#*=}" ;;
				qemu-args=*) local qemu_args="${qemu_args} ${OPTARG#*=}" ;;
				disk-type=*) local disk_type="${OPTARG#*=}" ;;
				disks=*) local disks="${OPTARG#*=}" ;;
				raw-cache=*) local raw_cache=",cache${OPTARG#*=}" ;;
				force=*) local force_vm=${OPTARG#*=} ;;
				*)
					error "unknown argument $OPTARG"
					return 1
			esac
			;;
			*)
				error "vm_create Unknown param $OPTARG"
				return 1
			;;
		esac
	done

	# Find next directory we can use
	if [[ ! -z $force_vm ]]; then
		vm_num=$force_vm

		vm_num_is_valid $vm_num || return 1
		local vm_dir="$VM_BASE_DIR/$vm_num"
		[[ -d $vm_dir ]] && echo "WARNING: removing existing VM in '$vm_dir'"
		echo "rm -rf $vm_dir"
	else
		local vm_dir=""
		for (( i=0; i<=256; i++)); do
			local vm_dir="$VM_BASE_DIR/$i"
			[[ ! -d $vm_dir ]] && break
		done

		vm_num=$i
	fi

	if [[ $i -eq 256 ]]; then
		error "no free VM found. do some cleanup (256 VMs created, are you insane?)"
		return 1
	fi

	echo "INFO: Creating new VM in $vm_dir"
	mkdir -p $vm_dir
	if [[ ! -r $os ]]; then
		error "file not found: $os"
		return 1
	fi

	# WARNING:
	# each cmd+= must contain ' ${eol}' at the end
	#
	local eol="\\\\\n  "
	local qemu_mask_param="VM_${vm_num}_qemu_mask"
	local qemu_numa_node_param="VM_${vm_num}_qemu_numa_node"

	if [[ -z "${!qemu_mask_param}" ]] || [[ -z "${!qemu_numa_node_param}" ]]; then
		error "Parameters ${qemu_mask_param} or ${qemu_numa_node_param} not found in autotest.config file"
		return 1
	fi

	local task_mask=${!qemu_mask_param}

	echo "INFO: TASK MASK: $task_mask"
	local cmd="taskset -a $task_mask $INSTALL_DIR/bin/qemu-system-x86_64 ${eol}"
	local vm_socket_offset=$(( 10000 + 100 * vm_num ))

	local ssh_socket=$(( vm_socket_offset + 0 ))
	local fio_socket=$(( vm_socket_offset + 1 ))
	local http_socket=$(( vm_socket_offset + 2 ))
	local https_socket=$(( vm_socket_offset + 3 ))
	local gdbserver_socket=$(( vm_socket_offset + 4 ))
	local vnc_socket=$(( 100 + vm_num ))
	local qemu_pid_file="$vm_dir/qemu.pid"
	local cpu_num=0

	for ((cpu=0; cpu<$(nproc --all); cpu++))
	do
		(($task_mask&1<<$cpu)) && ((cpu_num++)) || :
	done

	#-cpu host
	local node_num=${!qemu_numa_node_param}
	echo "INFO: NUMA NODE: $node_num"
	cmd+="-m 1024 --enable-kvm -smp $cpu_num -vga std -vnc :$vnc_socket -daemonize -snapshot ${eol}"
	cmd+="-object memory-backend-file,id=mem,size=1G,mem-path=/dev/hugepages,share=on,prealloc=yes,host-nodes=$node_num,policy=bind ${eol}"
	cmd+="-numa node,memdev=mem ${eol}"
	cmd+="-pidfile $qemu_pid_file ${eol}"
	cmd+="-serial file:$vm_dir/serial.log ${eol}"
	cmd+="-D $vm_dir/qemu.log ${eol}"
	cmd+="-net user,hostfwd=tcp::$ssh_socket-:22,hostfwd=tcp::$fio_socket-:8765,hostfwd=tcp::$https_socket-:443,hostfwd=tcp::$http_socket-:80 ${eol}"
	cmd+="-net nic ${eol}"

	cmd+="-drive file=$os,if=none,id=os_disk ${eol}"
	cmd+="-device ide-hd,drive=os_disk,bootindex=0 ${eol}"

	IFS=':'

	if ( [[ $disks == '' ]] && [[ $disk_type == virtio* ]] ); then
		disks=1
	fi

	for disk in $disks; do
		case $disk_type in
			virtio)
				local raw_name="RAWSCSI"
				local raw_disk=$vm_dir/test.img

				if [[ ! -z $disk ]]; then
					[[ ! -b $disk ]] && touch $disk
					local raw_disk=$(readlink -f $disk)
				fi

				# Create disk file if it not exist or it is smaller than 10G
				if ( [[ -f $raw_disk ]] && [[ $(stat --printf="%s" $raw_disk) -lt $((1024 * 1024 * 1024 * 10)) ]] ) || \
					[[ ! -e $raw_disk ]]; then
					if [[ $raw_disk =~ /dev/.* ]]; then
						error \
							"ERROR: Virtio disk point to missing device ($raw_disk) - \n" \
							"       this is probably not what you want."
							return 1
					fi

					echo "INFO: Creating Virtio disc $raw_disk"
					dd if=/dev/zero of=$raw_disk bs=1024k count=10240
				else
					echo "INFO: Using existing image $raw_disk"
				fi

				cmd+="-device virtio-scsi-pci ${eol}"
				cmd+="-device scsi-hd,drive=hd$i,vendor=$raw_name ${eol}"
				cmd+="-drive if=none,id=hd$i,file=$raw_disk,format=raw$raw_cache ${eol}"
				;;
			spdk_vhost_scsi)
				echo "INFO: using socket $SPDK_VHOST_SCSI_TEST_DIR/naa.$disk.$vm_num"
				cmd+="-chardev socket,id=char_$disk,path=$SPDK_VHOST_SCSI_TEST_DIR/naa.$disk.$vm_num ${eol}"
				cmd+="-device vhost-user-scsi-pci,id=scsi_$disk,num_queues=$cpu_num,chardev=char_$disk ${eol}"
				;;
			spdk_vhost_blk)
				[[ $disk =~ _size_([0-9]+[MG]?) ]] || true
				size=${BASH_REMATCH[1]}
				if [ -z "$size" ]; then
					size="20G"
				fi
				disk=${disk%%_*}
				echo "INFO: using socket $SPDK_VHOST_SCSI_TEST_DIR/naa.$disk.$vm_num"
				cmd+="-chardev socket,id=char_$disk,path=$SPDK_VHOST_SCSI_TEST_DIR/naa.$disk.$vm_num ${eol}"
				cmd+="-device vhost-user-blk-pci,num_queues=$cpu_num,chardev=char_$disk,"
				cmd+="logical_block_size=4096,size=$size ${eol}"
				;;
			kernel_vhost)
				if [[ -z $disk ]]; then
					error "need WWN for $disk_type"
					return 1
				elif [[ ! $disk =~ ^[[:alpha:]]{3}[.][[:xdigit:]]+$ ]]; then
					error "$disk_type - disk(wnn)=$disk does not look like WNN number"
					return 1
				fi
				echo "Using kernel vhost disk wwn=$disk"
				cmd+=" -device vhost-scsi-pci,wwpn=$disk ${eol}"
				;;
			*)
				error "unknown mode '$disk_type', use: virtio, spdk_vhost_scsi, spdk_vhost_blk or kernel_vhost"
				return 1
		esac
	done

	[[ ! -z $qemu_args ]] && cmd+=" $qemu_args ${eol}"
	# remove last $eol
	cmd="${cmd%\\\\\\n  }"

	echo "Saving to $vm_dir/run.sh:"
	(
	echo '#!/bin/bash'
	echo 'if [[ $EUID -ne 0 ]]; then '
	echo '	echo "Go away user come back as root"'
	echo '	exit 1'
	echo 'fi';
	echo
	echo -e "qemu_cmd=\"$cmd\"";
	echo
	echo "echo 'Running VM in $vm_dir'"
	echo "rm -f $qemu_pid_file"
	echo '$qemu_cmd'
	echo "echo 'Waiting for QEMU pid file'"
	echo "sleep 1"
	echo "[[ ! -f $qemu_pid_file ]] && sleep 1"
	echo "[[ ! -f $qemu_pid_file ]] && echo 'ERROR: no qemu pid file found' && exit 1"
	echo
	echo "chmod +r $vm_dir/*"
	echo
	echo "echo '=== qemu.log ==='"
	echo "cat $vm_dir/qemu.log"
	echo "echo '=== qemu.log ==='"
	echo '# EOF'
	) > $vm_dir/run.sh
	chmod +x $vm_dir/run.sh

	# Save generated sockets redirection
	echo $ssh_socket > $vm_dir/ssh_socket
	echo $fio_socket > $vm_dir/fio_socket
	echo $http_socket > $vm_dir/http_socket
	echo $https_socket > $vm_dir/https_socket
	echo $gdbserver_socket > $vm_dir/gdbserver_socket
	echo $vnc_socket >> $vm_dir/vnc_socket
}

function vm_run()
{
	local OPTIND optchar a
	local run_all=false
	while getopts 'a-:' optchar; do
		case "$optchar" in
			a) run_all=true ;;
			*)
				echo "vm_run Unknown param $OPTARG"
				return 1
			;;
		esac
	done

	local vms_to_run=""

	if $run_all; then
		shopt -s nullglob
		vms_to_run=$VM_BASE_DIR/[0-9]*
	else
		shift $((OPTIND-1))
		for vm in $@; do
			vm_num_is_valid $1 || return 1
			if [[ ! -x $VM_BASE_DIR/$vm/run.sh ]]; then
				error "VM$vm not defined - setup it first"
				return 1
			fi
			vms_to_run+=" $VM_BASE_DIR/$vm"
		done
	fi

	for vm in $vms_to_run; do
		if vm_is_running $(basename $vm); then
			echo "WARNING: VM$(basename $vm) ($vm) already running"
			continue
		fi

		echo "INFO: running $vm/run.sh"
		if ! $vm/run.sh; then
			error "FAILED to run vm $vm"
			return 1
		fi
	done
}

# Wait for all created VMs to boot.
# param $1 max wait time
function vm_wait_for_boot()
{
	assert_number $1

	local all_booted=false
	local timeout_time=$1
	[[ $timeout_time -lt 10 ]] && timeout_time=10
	local timeout_time=$(date -d "+$timeout_time seconds" +%s)

	echo "Waiting for VMs to boot"
	shift
	if [[ "$@" == "" ]]; then
		local vms_to_check="$VM_BASE_DIR/[0-9]*"
	else
		local vms_to_check=""
		for vm in $@; do
			vms_to_check+=" $VM_BASE_DIR/$vm"
		done
	fi

	for vm in $vms_to_check; do
		local vm_num=$(basename $vm)
		local i=0
		echo "INFO: waiting for VM$vm_num ($vm)"
		while ! vm_os_booted $vm_num; do
			if ! vm_is_running $vm_num; then
				echo
				echo "ERROR: VM $vm_num is not running"
				echo "================"
				echo "QEMU LOG:"
				if [[ -r $vm/qemu.log ]]; then
					cat $vm/qemu.log
				else
					echo "LOG not found"
				fi

				echo "VM LOG:"
				if [[ -r $vm/serial.log ]]; then
					cat $vm/serial.log
				else
					echo "LOG not found"
				fi
				echo "================"
				return 1
			fi

			if [[ $(date +%s) -gt $timeout_time ]]; then
				error "timeout waiting for machines to boot"
				return 1
			fi
			if (( i > 30 )); then
				local i=0
				echo
			fi
			echo -n "."
			sleep 1
		done
		echo ""
		echo "INFO: VM$vm_num ready"
	done

	echo "INFO: all VMs ready"
	return 0
}

function vm_start_fio_server()
{
	local OPTIND optchar
	local readonly=''
	while getopts ':-:' optchar; do
		case "$optchar" in
			-)
			case "$OPTARG" in
				fio-bin=*) local fio_bin="${OPTARG#*=}" ;;
				readonly) local readonly="--readonly" ;;
				*) echo "Invalid argument '$OPTARG'" && return 1;;
			esac
			;;
			*) echo "Invalid argument '$OPTARG'" && return 1;;
		esac
	done

	shift $(( OPTIND - 1 ))
	for vm_num in $@; do
		echo "INFO: Starting fio server on VM$vm_num"
		if [[ $fio_bin != "" ]]; then
			cat $fio_bin | vm_ssh $vm_num 'cat > /root/fio; chmod +x /root/fio'
			vm_ssh $vm_num /root/fio $readonly --eta=never --server --daemonize=/root/fio.pid
		else
			vm_ssh $vm_num fio $readonly --eta=never --server --daemonize=/root/fio.pid
		fi
	done
}

function vm_check_scsi_location()
{
	# Script to find wanted disc
	local script='shopt -s nullglob; \
	for entry in /sys/block/sd*; do \
		disk_type="$(cat $entry/device/vendor)"; \
		if [[ $disk_type == INTEL* ]] || [[ $disk_type == RAWSCSI* ]] || [[ $disk_type == LIO-ORG* ]]; then \
			fname=$(basename $entry); \
			echo -n " $fname"; \
		fi; \
	done'

	SCSI_DISK="$(echo "$script" | vm_ssh $1 bash -s)"

	if [[ -z "$SCSI_DISK" ]]; then
		error "no test disk found!"
		return 1
	fi
}

# Script to perform scsi device reset on all disks in VM
# param $1 VM num
# param $2..$n Disks to perform reset on
function vm_reset_scsi_devices()
{
	for disk in "${@:2}"; do
		echo "INFO: VM$1 Performing device reset on disk $disk"
		vm_ssh $1 sg_reset /dev/$disk -vNd
	done
}

function vm_check_blk_location()
{
	local script='shopt -s nullglob; cd /sys/block; echo vd*'
	SCSI_DISK="$(echo "$script" | vm_ssh $1 bash -s)"

	if [[ -z "$SCSI_DISK" ]]; then
		error "no blk test disk found!"
		return 1
	fi
}

# Shutdown or kill any running VM and SPDK APP.
#
function at_app_exit()
{
	echo "INFO: APP EXITING"
	echo "INFO: killing all VMs"
	vm_kill_all
	# Kill vhost application
	echo "INFO: killing vhost app"
	spdk_vhost_kill

	echo "INFO: EXIT DONE"
}

function error_exit()
{
	trap - ERR
	set +e
	echo "Error on $1 $2"

	at_app_exit
	exit 1
}
