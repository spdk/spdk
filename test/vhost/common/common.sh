set -e

SPDK_AUTOTEST_X=false
SPDK_VHOST_AUTOTEST_X=false
SPDK_VHOST_VERBOSE=false

BASE_DIR=$(readlink -f $(dirname $0))

# Default running dir -> spdk/..
declare -r SPDK_DEFAULT_BUILD_DIR=$BASE_DIR/../../../
declare -r SPDK_VHOST_DEFAULT_TEST_DIR=$(readlink -f $SPDK_DEFAULT_BUILD_DIR/..)

function error()
{
	echo "==========="
	if ! $SPDK_VHOST_VERBOSE; then
		local verbose_out=""
	elif [[ ${FUNCNAME[1]} == "source" ]]; then
		local verbose_out=" (file $(basename ${BASH_SOURCE[0]}):${BASH_LINENO[0]})"
	else
		local verbose_out=" (function ${FUNCNAME[1]}:${BASH_LINENO[0]})"
		
	fi	
	echo -e "ERROR$verbose_out: $@"
	echo "==========="
	# Don't 'return 1' since the stack trace will be incomplete (why?) missing upper command.
	false
}

function warning()
{
	if ! $SPDK_VHOST_VERBOSE; then
		local verbose_out=""
	elif [[ ${FUNCNAME[1]} == "source" ]]; then
		local verbose_out=" (file $(basename ${BASH_SOURCE[0]}) line ${BASH_LINENO[0]})"
	else
		local verbose_out=" (function ${FUNCNAME[1]}:${BASH_LINENO[0]})"
	fi
	
	echo -e "WARN$verbose_out: $@"
}
function notice()
{
	if ! $SPDK_VHOST_VERBOSE; then
		local verbose_out=""
	elif [[ ${FUNCNAME[1]} == "source" ]]; then
		local verbose_out=" (file $(basename ${BASH_SOURCE[0]}) line ${BASH_LINENO[0]})"
	else
		local verbose_out=" (function ${FUNCNAME[1]}:${BASH_LINENO[0]})"
	fi	
	
	echo -e "INFO$verbose_out: $@"
}


# This functions require no arguments
function spdk_vhost_usage_common()
{
	echo "
COMMON OPTIONS:
       --work-dir=WORK_DIR
              Directory where test will be execured. TEST_DIR will be set to WORK_DIR.
       --autotest-x
             Turn on script debug only for autotest_common.sh.
       -x    Turn on script debug for all files (set -x).
       -v    Make some commands be logs be more verbose.
       -h, --help
              Print this help and exit.

ENVIRONMENT
       TODO: Add RO/RW Info
       Those environment variables are read only and MIGHT NOT be changed after sourcing common.sh file.
       After sourcing common.sh the environment will be set as follows.

       VHOST_TEST_ARGS
              Program arguments without COMMON ARGUMENTS. Please use this variable in your scripts.
       TEST_DIR
              Any test data (VMs, configs etc) will be created here. Default is: $SPDK_VHOST_DEFAULT_TEST_DIR.
       SPDK_BUILD_DIR
              Path to SPDK we are running against. Default is: $SPDK_DEFAULT_BUILD_DIR

FOLDERS AND FILES:
    TEST_DIR/
              Root test directory. Will be created if not exist.
            vhost/
              SPDK vhost working directory. Here are created sockets, configs etc.
            vms/[0-N]/
              Generated VMs
            root/bin
              Place where all tools (like QEMU) are installed
"
}

declare -a VHOST_TEST_ARGS
for arg in "$@"; do
	case "$arg" in
		--work-dir=*)
			TEST_DIR="$(readlink -f ${arg#*=})/"
			;;
		--autotest-x)
			SPDK_AUTOTEST_X=true
			;;
		-x)
			SPDK_VHOST_AUTOTEST_X=true
			set -x
			;;
		-v)
			SPDK_VHOST_VERBOSE=true
			;;
		*)
			VHOST_TEST_ARGS+=( "$arg" )
			;;
	esac
done

# TODO: Change this to VHOST_TEST_DIR
: ${TEST_DIR="$SPDK_VHOST_DEFAULT_TEST_DIR"}
declare -r SPDK_BUILD_DIR="$(readlink -f $BASE_DIR/../../../)"

declare -r TEST_DIR="$(mkdir -p $TEST_DIR && cd $TEST_DIR && echo $PWD)"

# SSH key file - this variable might be changed any time we need to
: ${SPDK_VHOST_SSH_KEY_FILE="$HOME/.ssh/spdk_vhost_id_rsa"}
if [[ ! -e "$SPDK_VHOST_SSH_KEY_FILE" ]]; then
	error "Could not find SSH key file $SPDK_VHOST_SSH_KEY_FILE"
	exit 1
fi

notice "Using SSH key file $SPDK_VHOST_SSH_KEY_FILE"
#
# Include config describing QEMU and VHOST cores and NUMA
#
source $(readlink -f $(dirname ${BASH_SOURCE[0]}))/autotest.config

if $SPDK_VHOST_AUTOTEST_X; then
	set -x;
else
	set +x;
fi

source $SPDK_BUILD_DIR/scripts/autotest_common.sh
#
# If autotest_common.sh turn on debug flag and we don't want this turn off after including it
#
if $SPDK_VHOST_AUTOTEST_X; then
	set -x;
else
	set +x;
fi

declare -r VHOST_DIR="$TEST_DIR/vhost"
declare -r VM_BASE_DIR="$TEST_DIR/vms"
declare -r INSTALL_DIR="$TEST_DIR/root"

function spdk_vhost_run_usage()
{
	echo -e "
Usage: spdk_vhost_run [OPTIONS]

OPTIONS
       --gdbserver=ADR      Run app under gdbserver using ADDR. If empty of not passed don't run under gdbserver
       --pid-timeout=TIME   Timeout in seconds how long to wait for PID file. If 0 don't wait. Default: 20s.

DESCRIPTION
       Run SPDK vhost. Parameters accepted by vhost app are accepted here, except: -r, -f, -c.
       -r used internally so can't be changed.
       -f used internally so can't be changed.
       -c is intercepted and and NVMe PCI are added to the config using scripts/gen_nvme.sh.
       All other options accepted by vhost passed directly to the app. See 'app/vhost/vhost --help' to check
       what parameters you can pass.
       Additional accepted parameters are listened in ADDITIONAL ARGUMENTS.

       After running vhost there will be additional files created in TEST_DIR/vhost directory:
       vhost.pid - file with PID of executed process.
       vhost.conf - for '-c', the final configuration file.
"
	spdk_vhost_usage_common
}

function spdk_vhost_run()
{
	local conf_in=""
	local gdbserver_addr=""
	
	local vhost_args=()
	local args=( $@ )
	local pid_timeout=20
	local run_in_bg="&"
	
	for (( i = 0; i < ${#args[@]}; i++ )); do
		local arg="${args[i]}"
		case "$arg" in
			-c|-e|-i|-m|-p|-s|-S|-t)
				if (( i == ${#args[@]} )); then
					error "spdk_vhost_run: argument '${args[i]}' require parameter"
					return 1
				fi
				
				if [[ "$arg" == "-c" ]]; then
					local conf_in="${args[i + 1]}"
					(( i += 1 ))
					continue
				fi  
				
				vhost_args+=( "${args[i]}" "${args[i + 1]}" )
				(( i+= 1 ))
				;;
			-N|-d|-q)
				vhost_args+=( "${args[i]}" )
				;;
			-r|-f)
				error "spdk_vhost_run: sorry option '$arg' is not supported in this script"
				return 1
				;;
			-h|--help)
				spdk_vhost_run_usage
				return 0
				;;
			--gdbserver=*)
				local gdbserver_addr="${arg#*=} "
				local run_in_bg=''
				;;
			--pid-timeout=*)
				local pid_timeout="${arg#*=}"
				;;
			*)
				error "spdk_vhost_run: Invalid parameter: $arg"
				return 1
				break
				;;
		esac
	done
	unset args arg
	
	local vhost_app="$SPDK_BUILD_DIR/app/vhost/vhost"
	
	if [[ $EUID -ne 0 ]]; then
		error "go away user come back as root"
		return 1
	elif [[ ! -x $vhost_app ]]; then
		error "application '$vhost_app' not found. Try building SPDK first."
		return 1
	elif [[ -z "$vhost_reactor_mask" ]] || [[ -z "$vhost_master_core" ]]; then
		error "parameters vhost_reactor_mask or vhost_master_core not set. Don't know how to run vhost :("
		return 1
	elif [[ -r "$VHOST_DIR/vhost.pid" ]] && pkill -0 -F $VHOST_DIR/vhost.pid; then
		error "one instance of vhost already running in '$VHOST_DIR'. Kill it first." 
		return 1
	fi
	
	rm -rf $VHOST_DIR
	mkdir -p $VHOST_DIR
	
	touch "$VHOST_DIR/vhost.conf"
	if [[ ! -z "$conf_in" ]]; then
		cp "$conf_in" "$VHOST_DIR/vhost.conf"
		echo "# Auto generated part from $SPDK_BUILD_DIR/scripts/gen_nvme.sh" >> $VHOST_DIR/vhost.conf 
		$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $VHOST_DIR/vhost.conf
		
		notice "Using generated configuration file:"
		cat "$VHOST_DIR/vhost.conf"
	fi
	
	notice "running SPDK vhost app:"
	notice "gdb: $do_gdbserver"
	notice "$vhost_app ${vhost_args[@]}"
	
	DEFAULT_RPC_ADDR="$VHOST_DIR/vhost_rpc.socket"
	
	if [[ ! -z "$gdbserver_addr" ]]; then
		notice ""
		notice "Runing under gdbserver on address $gdbserver_addr - connect to continue"
		notice ""
		$gdbserver $vhost_app -S "$VHOST_DIR" -m "$vhost_reactor_mask" -p "$vhost_master_core" -r "$DEFAULT_RPC_ADDR" \
			-f $VHOST_DIR/vhost.pid -c "$VHOST_	/vhost.conf" "${vhost_args[@]}"
		# If gdbserver exit the vhost process is dead.
		return 0
	fi
	
	$vhost_app -S "$VHOST_DIR" -m "$vhost_reactor_mask" -p "$vhost_master_core" -r "$DEFAULT_RPC_ADDR" \
		-f $VHOST_DIR/vhost.pid -c "$VHOST_DIR/vhost.conf" "${vhost_args[@]}" &

	if (( pid_timeout == 0 )); then
		notice "not waiting for vhost to start"
		return 0
	fi
	
	notice "waiting for app to start..."
	for (( ; pid_timeout; pid_timeout--)); do
		if [[ ! -r "$VHOST_DIR/vhost.pid" ]]; then
			sleep 1
			continue
		fi	
		
		waitforlisten "$(cat $VHOST_DIR/vhost.pid)"
		notice "vhost started - pid=$(cat $VHOST_DIR/vhost.pid)"
		return 0
	done

	error "spdk_vhost_run: timeout waiting for PID file"
	return 1
}

function spdk_vhost_kill()
{
	local vhost_pid_file="$VHOST_DIR/vhost.pid"

	if [[ ! -r $vhost_pid_file ]]; then
		echo "WARN: no vhost pid file found"
		return 0
	fi

	local vhost_pid="$(cat $vhost_pid_file)"
	notice "killing vhost (PID $vhost_pid) app"

	if /bin/kill -INT $vhost_pid >/dev/null; then
		notice "sent SIGINT to vhost app - waiting 60 seconds to exit"
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
		notice "vhost was no running"
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
	[[ "$1" =~ ^[0-9]+$ ]] && return 0

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

function vm_create_ssh_config()
{
	local ssh_config="$VM_BASE_DIR/ssh_config"
	if [[ ! -f $ssh_config ]]; then
		(
		echo "Host *"
		echo "  ControlPersist=10m"
		echo "  ConnectTimeout=2"
		echo "  Compression=no"
		echo "  ControlMaster=auto"
		echo "  UserKnownHostsFile=/dev/null"
		echo "  StrictHostKeyChecking=no"
		echo "  User root"
		echo "  ControlPath=$VM_BASE_DIR/%r@%h:%p.ssh"
		echo ""
		) > $ssh_config
	fi
}

# Execute ssh command on given VM
# param $1 virtual machine number
#
function vm_ssh()
{
	vm_num_is_valid $1 || return 1
	vm_create_ssh_config
	local ssh_config="$VM_BASE_DIR/ssh_config"

	local ssh_cmd="ssh -i $SPDK_VHOST_SSH_KEY_FILE -F $ssh_config \
		-p $(vm_ssh_socket $1) 127.0.0.1"

	shift
	$ssh_cmd "$@"
}

# Execute scp command on given VM
# param $1 virtual machine number
#
function vm_scp()
{
	vm_num_is_valid $1 || return 1
	vm_create_ssh_config
	local ssh_config="$VM_BASE_DIR/ssh_config"

	local scp_cmd="scp -i $SPDK_VHOST_SSH_KEY_FILE -F $ssh_config \
		-P $(vm_ssh_socket $1) "

	shift
	$scp_cmd "$@"
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
			warning "not root - assuming we running since can't be checked"
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
		notice "VM$1 ($vm_dir) is not running"
		return 0
	fi

	# Temporarily disabling exit flag for next ssh command, since it will
	# "fail" due to shutdown
	echo "Shutting down virtual machine $vm_dir"
	set +e
	vm_ssh $1 "nohup sh -c 'shutdown -h -P now'" || true
	notice "VM$1 is shutting down - wait a while to complete"
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
		notice "process $vm_pid killed"
		rm $vm_dir/qemu.pid
	elif vm_is_running $1; then
		error "Process $vm_pid NOT killed"
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

	notice "Waiting for VMs to shutdown..."
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
			notice "All VMs successfully shut down"
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

function vm_setup_usage()
{
	echo -e "
Usage: vm_setup [OPTIONS]
OPTIONS
       --vm-num=VM_NUM
              OPTIONAL: Force VM_NUM reconfiguration if already exist. If not provided,
              next free VM number will be used.
       --disk=TYPE[,PARAMS]
              MANDATORY: Disk to add (can be added multiple times). TYPE can be one of:
              virtio-scsi -QEMU virito SCSI disk. Parameters:
                     path - path to existing file (might be block device)
                     size - Mandatory for 'path'. size of the disk.
                     cache QEMU caching type ( writethrough, writeback, none, unsafe or directsyns).
                            See QEMU doc for more details
                     EXAMPLE: --disk=virtio-scsi,size=10G,cache=writeback
              kernel-vhost-scsi
                     naa - WWN number to be used
                     EXAMPLE: --disk=kernel-vhost-scsi,naa=naa.123456789
              spdk-vhost-scsi - the SPDK vhost SCSI controller name. Parameters:
                     ctrl - the SPDK vhost SCSI controller name.
                     EXAMPLE: --disk=spdk_vhost_scsi,path=ctrlr0
              spdk-vhost-blk - the SPDK vhost Block controller name. Parameters:
                     ctrlr - the SPDK vhost SCSI controller name.
                     size - size of the given bdev disk.
                     lba - LBA size of the backing disk. default: 4096
                     EXAMPLE: --disk=spdk_vhost_blk,path=ctrlr1,size=10G
       --os=[MODE,]OS_QCOW2
              MANDATORY: OS image file in qcow2 format. MODE is optional (default snapshot)
                     snapshot - use '-snapshot'
                     backing - create new image but use provided backing file
                     original - use file directly. Will modify the provided file
                     EXAMPLE: --os=backing,some_image.qcow2
       --qemu-args=ARGS
              OPTIONAL: Custom QEMU args to be directly appended to QEMU command line.
"
	spdk_vhost_usage_common
}

function vm_setup()
{
	local os_mode="snapshot"
	local qemu_args=""
	local disks=()
	local virtio_cache=""
	local vm_num=""
	local args=( "$@" )

	for (( i = 0; i < ${#args[@]}; i++ )); do
		local arg="${args[i]}"
		case "$arg" in
			--disk=*) disks+=( "${arg#*=}" ) ;;
			--vm-num=*) local vm_num="${arg#*=}" ;;
			--os=*)
				if [[ ! "${arg#*=}" =~ ^((snapshot|backing|original)[,])?([^,]+)$ ]]; then
					error "invalid argument '$arg'"
					return 0
				elif (( ${#BASH_REMATCH[@]} == 4 )); then
					local os_mode="${BASH_REMATCH[2]}"
					local os_img="${BASH_REMATCH[3]}"
				else
					local os_img="${BASH_REMATCH[3]}"
				fi
			;;
			--qemu-args=*) local qemu_args="${arg#*=}" ;;
			-h|--help)
				vm_setup_usage
				return 0
			;;
			*)
				error "Invalid parameter: $arg"
				return 1
			;;
		esac
	done
	unset args arg

	# Find next directory we can use
	if [[ ! -z "$vm_num" ]]; then
		if ! vm_num_is_valid $vm_num; then
			error "'$vm_num' is invalid VM number"  
			return 1
		fi
					
		local vm_dir="$VM_BASE_DIR/$vm_num"
		[[ -d $vm_dir ]] && warning "removing existing VM in '$vm_dir'"
		rm -rf $vm_dir
	else
		for (( i=0; i<256; i++)); do
			[[ ! -d $VM_BASE_DIR/$i ]] && break
		done

		if (( i == 256 )); then
			error "no free VM found. do some cleanup (256 VMs created, are you insane?)"
			return 1
		fi
		
		local vm_num=$i
		local vm_dir="$VM_BASE_DIR/$vm_num"
		unset i
	fi
	
	mkdir -p $vm_dir
	notice "Creating new VM in $vm_dir"
		
	# Discover CPU mask and NUMA node for QEMU (VM number) 
	local qemu_mask="$(tmp=VM_${vm_num}_qemu_mask;  echo ${!tmp})"
	local qemu_numa_node="$(tmp=VM_${vm_num}_qemu_numa_node; echo ${!tmp})"
	if [[ -z "$qemu_mask" || -z "$qemu_numa_node" ]]; then
		error "Parameters VM_${vm_num}_qemu_mask or VM_${vm_num}_qemu_numa_node not found in autotest.config file"
		return 1
	fi

	local cpu_num=0
	for (( cpu = $(nproc --all); cpu >= 0; cpu--)); do
		(( ($qemu_mask & (1 << $cpu)) && cpu_num++)) || :
	done
	
	notice "TASK MASK: $qemu_mask"
	notice "NUMA NODE: $qemu_numa"
		
	if [[ -z "$os_img" || -z "${disks[@]}" ]]; then
		error "Missing mandatory parameter: os='$os_img', disks='${disks[@]}'"
		return 1
	fi

	local qemu_devices=""
	if [[ $os_mode = "backing" ]]; then
		notice "creating backing file for OS image file: $os_img"
		if ! $INSTALL_DIR/bin/qemu-img create -f qcow2 -b $os_img $vm_dir/os.qcow2; then
			error "Failed to  create OS backing file in '$vm_dir/os.qcow2' using $os_img"
			return 1
		fi
		
		local os_img=$vm_dir/os.qcow2
	elif [[ $os_mode == "original" ]]; then
		warning "Using original OS image file: $os_img"
		qemu_devices+="-drive file=$os_img,if=none,id=os_disk -device ide-hd,drive=os_disk,bootindex=0 \\"
	fi

	local eol=$'\\\n'
	local dev_num=0
	for disk in ${disks[@]}; do
		if [[ ! "$disk" =~ ^(virtio-scsi|kernel-vhost-scsi|spdk-vhost-scsi|spdk-vhost-blk)([,](.*))?$ ]]; then
			error "invalid disk '$disk'"
			return 1
		fi
		
		local params="${BASH_REMATCH[3]}"
		case ${BASH_REMATCH[1]} in
			virtio-scsi)
				unset disk_path
				unset disk_size
				unset virtio_cache
				
				[[ "$params" =~ size=([0-9]+[MBG]?) ]] && local size="${BASH_REMATCH[1]}"
				[[ "$params" =~ cache=([[:alpha:]]+) ]] && local virtio_cache=",${BASH_REMATCH[0]}"
				[[ "$params" =~ path=([[:alpha:]]+) ]] && local disk_path="${BASH_REMATCH[1]}"
				
				if [[ -z "$disk_path" ]]; then
					local disk_path="$vm_dir/virtio_${dev_num}.raw"
					if [[ -z "$size" ]]; then
						error "need 'size' for virtio disk"
						return 1
					fi
					
					notice "Creating Virtio disc $disk_path_disk of size=$size. This might take a while..."
					if ! $INSTALL_DIR/bin/qemu-img create -f raw -o "preallocation=falloc" $disk_path $size; then
						error "Failed to  create '$disk_path'"
						return 1
					fi
				elif [[ ! -z "$size" ]]; then
					error "'size' is only valid with 'path'"
					return 1
				elif [[ $disk_path =~ ^/dev/.+$ ]] && [[ ! -b $disk_path ]]; then
					error "Virtio disk point to missing device ($disk_path) this is probably not what you want."
					return 1 
				fi

				qemu_devices+=" -device virtio-scsi-pci -device scsi-hd,drive=virtio_$dev_num,vendor=RAWSCSI"
				qemu_devices+=" -drive if=none,id=virtio_$dev_num,file=$disk_path,format=raw$virtio_cache ${eol}"
				;;
			spdk-vhost-scsi)
				if [[ ! "$params" =~ ctrlr=([^,]+)$ ]]; then
					error "spdk-vhost-scsi invalid or missing ctrlr param '$param'"
					return 1
				fi	
				
				notice "using socket $VHOST_DIR/${BASH_REMATCH[1]}"
				qemu_devices+=" -chardev socket,id=char_$dev_num,path=$VHOST_DIR/${BASH_REMATCH[1]}"
				qemu_devices+=" -device vhost-user-scsi-pci,id=scsi_$dev_num,num_queues=$cpu_num,chardev=char_$dev_num ${eol}"
				;;
			spdk-vhost-blk)
				unset ctrlr
				unset size
				local lba=4096
				[[ "$params" =~ ctrlr=([^,]+) ]] && local ctrlr="${BASH_REMATCH[1]}"
				[[ "$params" =~ size=([0-9]+[MBG]?) ]] && local size="${BASH_REMATCH[1]}"
				[[ "$params" =~ lba=([0-9]+[MBG]?) ]] && local lba="${BASH_REMATCH[1]}"
				
				if [[ -z "$ctrlr" || -z "$size" ]]; then
					error "invalid or missing parameter param for '$disk'"
					return 1
				fi
									
				notice "using socket $VHOST_DIR/$ctrlr"
				qemu_devices+=" -chardev socket,id=char_$dev_num,path=$VHOST_DIR/$ctrlr"
				qemu_devices+=" -device vhost-user-blk-pci,num_queues=$cpu_num,chardev=char_$dev_num,logical_block_size=$lba,size=$size ${eol}"
				;;
			kernel-vhost-scsi)
				unset wwn
				[[ $params =~ ^naa=([[:alpha:]]{3}[.][[:xdigit:]]+)$ ]] && local wwn="${BASH_REMATCH[1]}"
				
				if [[ -z $wwn ]]; then
					error "invalid WWN number: $wwn"
					return 1
				else
					notice "using kernel vhost disk wwn=$disk"
					qemu_devices+=" -device vhost-scsi-pci,wwpn=$wwn ${eol}"
				fi
				;;
			*)
				error "unknown disk type '${BASH_REMATCH[1]}'"
				return 1
		esac
		(( dev_num++ )) || :
	done
	notice "dupa"
	warning "pupa"
	echo "Saving to $vm_dir/run.sh:"
	# Save sockets redirection
	local vm_socket_offset=$(( 10000 + 100 * vm_num ))
	echo $(( vm_socket_offset + 0 )) > $vm_dir/ssh_socket
	echo $(( vm_socket_offset + 1 )) > $vm_dir/fio_socket
	echo $(( vm_socket_offset + 4 )) > $vm_dir/gdbserver_socket
	echo $(( 100 + vm_num )) > $vm_dir/vnc_socket

	# Produce scripts and files for running VM	
	(
	echo "#!/bin/bash $($SPDK_VHOST_AUTOTEST_X && echo '-x')"
	echo "if [[ \$EUID -ne 0 ]]; then"
	echo "	echo 'Go away user come back as root'"
	echo "	exit 1
	echo "fi
	echo ""
	echo "echo 'Running VM in $vm_dir'"
	echo "rm -f $vm_dir/qemu.pid"
	echo "taskset -a $qemu_mask $INSTALL_DIR/bin/qemu-system-x86_64 $([[ $os_mode == 'snapshot' ]] && echo '-snapshot') \\"
	echo " -m 1024 --enable-kvm -smp $cpu_num -cpu host -vga std -vnc :$vnc_socket -daemonize \\"
	echo " -object memory-backend-file,id=mem,size=1G,mem-path=/dev/hugepages,share=on,prealloc=yes,host-nodes=$qemu_numa_node,policy=bind \\"
	echo " -numa node,memdev=mem \\"
	echo " -pidfile $vm_dir/qemu.pid \\"
	echo " -serial file:$vm_dir/serial.log \\"
	echo " -D $vm_dir/qemu.log \\"
	echo " -net user,hostfwd=tcp::$(cat $vm_dir/ssh_socket)-:22,hostfwd=tcp::$(cat $vm_dir/fio_socket)-:8765 -net nic \\"
	echo -e "${qemu_devices} $( [[ ! -z "$qemu_args" ]] && echo "$qemu_args" )"
	echo ""
	echo "echo 'Waiting for QEMU pid file'"
	echo "sleep 1 && [[ ! -f $qemu_pid_file ]] && sleep 1"
	echo "[[ ! -f $qemu_pid_file ]] && echo 'ERROR: no qemu pid file found' && exit 1"
	echo ""
	echo "chmod +r $vm_dir/*"
	echo ""
	echo "echo '=== qemu.log ==='"
	echo "cat $vm_dir/qemu.log"
	echo "echo '=== qemu.log ==='"
	echo "# EOF"
	) > $vm_dir/run.sh
	chmod +x $vm_dir/run.sh
}

function vm_run_usage()
{
	echo -e "
Usage: vm_run [OPTIONS] VM_LIST...
    OPTIONS:
    -a                    Run all VMs in WORK_DIR - VM_LIST must be empty
"
	spdk_vhost_usage_common
}

function vm_run()
{
	local vms_to_run=()
	local args=( $@ )
	for (( i = 0; i < ${#args[@]}; i++ )); do
		case "${args[i]}" in
			-a)
				if (( i == ${#args[@]} - 1 )); then
					error "No arguments allowed after '-a'"
					return 1
				fi 
				vms_to_run+=( $VM_BASE_DIR/[0-9]* )
				break
				;;
			-h|--help)
				vm_run_usage
				return 0
				;;
			*)
				vms_to_run+=( $(printf "$VM_BASE_DIR/%s" "${args[@]:i}") )
				break
				;;
		esac
	done
	unset args

	if [[ $EUID -ne 0 ]]; then
		error "vm_run: Go away user come back as root"
		return 1
	fi

	for vm in ${vms_to_run[@]}; do
		local vm_num="$(basename $vm)"
		vm_num_is_valid "$vm_num"
		if [[ ! -x "$vm/run.sh" ]]; then
			error "VM '$vm' not defined - setup it first"
			return 1
		fi
		
		if vm_is_running $vm_num; then
			warning "VM $vm already running"
		else		
			notice "running $vm/run.sh"
			if ! $vm/run.sh; then
				error "FAILED to run vm $vm"
				return 1
			fi
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
		notice "waiting for VM$vm_num ($vm)"
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
		notice "VM$vm_num ready"
	done

	notice "all VMs ready"
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
		notice "Starting fio server on VM$vm_num"
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
		notice "VM$1 Performing device reset on disk $disk"
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
	notice "APP EXITING"
	notice "killing all VMs"
	vm_kill_all
	# Kill vhost application
	notice "killing vhost app"
	spdk_vhost_kill

	notice "EXIT DONE"
}

function error_exit()
{
	trap - ERR
	print_backtrace
	set +e
	echo "Error on $1 $2"

	at_app_exit
	exit 1
}
