: ${SPDK_VHOST_VERBOSE=false}
: ${VHOST_DIR="$HOME/vhost_test"}
: ${QEMU_BIN="qemu-system-x86_64"}
: ${QEMU_IMG_BIN="qemu-img"}

TEST_DIR=$(readlink -f $rootdir/..)
VM_DIR=$VHOST_DIR/vms
TARGET_DIR=$VHOST_DIR/vhost
VM_PASSWORD="root"

#TODO: Move vhost_vm_image.qcow2 into VHOST_DIR on test systems.
VM_IMAGE=$HOME/vhost_vm_image.qcow2

if ! hash $QEMU_IMG_BIN $QEMU_BIN; then
	error 'QEMU is not installed on this system. Unable to run vhost tests.'
	exit 1
fi

mkdir -p $VHOST_DIR
mkdir -p $VM_DIR
mkdir -p $TARGET_DIR

#
# Source config describing QEMU and VHOST cores and NUMA
#
source $rootdir/test/vhost/common/autotest.config

function vhosttestinit()
{
	if [ "$TEST_MODE" == "iso" ]; then
		$rootdir/scripts/setup.sh

		# Look for the VM image
		if [[ ! -f $VM_IMAGE ]]; then
			echo "VM image not found at $VM_IMAGE"
			echo "Download to $HOME? [yn]"
			read -r download
			if [ "$download" = "y" ]; then
				curl https://dqtibwqq6s6ux.cloudfront.net/download/test_resources/vhost_vm_image.tar.gz | tar xz -C $HOME
			fi
		fi
	fi

	# Look for the VM image
	if [[ ! -f $VM_IMAGE ]]; then
		error "VM image not found at $VM_IMAGE"
		exit 1
	fi
}

function vhosttestfini()
{
	if [ "$TEST_MODE" == "iso" ]; then
		$rootdir/scripts/setup.sh reset
	fi
}

function message()
{
	local verbose_out
	if ! $SPDK_VHOST_VERBOSE; then
		verbose_out=""
	elif [[ ${FUNCNAME[2]} == "source" ]]; then
		verbose_out=" (file $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[1]})"
	else
		verbose_out=" (function ${FUNCNAME[2]}:${BASH_LINENO[1]})"
	fi

	local msg_type="$1"
	shift
	echo -e "${msg_type}${verbose_out}: $*"
}

function fail()
{
	echo "===========" >&2
	message "FAIL" "$@" >&2
	echo "===========" >&2
	exit 1
}

function error()
{
	echo "===========" >&2
	message "ERROR" "$@" >&2
	echo "===========" >&2
	# Don't 'return 1' since the stack trace will be incomplete (why?) missing upper command.
	false
}

function warning()
{
	message "WARN" "$@" >&2
}

function notice()
{
	message "INFO" "$@"
}

function get_vhost_dir()
{
	local vhost_name="$1"

	if [[ -z "$vhost_name" ]]; then
		error "vhost name must be provided to get_vhost_dir"
		return 1
	fi

	echo "$TARGET_DIR/${vhost_name}"
}

function vhost_run()
{
	local vhost_name="$1"
	local run_gen_nvme=true

	if [[ -z "$vhost_name" ]]; then
		error "vhost name must be provided to vhost_run"
		return 1
	fi
	shift

	if [[ "$1" == "--no-gen-nvme" ]]; then
		notice "Skipping gen_nvmf.sh NVMe bdev configuration"
		run_gen_nvme=false
		shift
	fi

	local vhost_dir
	vhost_dir="$(get_vhost_dir $vhost_name)"
	local vhost_app="$rootdir/app/vhost/vhost"
	local vhost_log_file="$vhost_dir/vhost.log"
	local vhost_pid_file="$vhost_dir/vhost.pid"
	local vhost_socket="$vhost_dir/usvhost"
	notice "starting vhost app in background"
	[[ -r "$vhost_pid_file" ]] && vhost_kill $vhost_name
	[[ -d $vhost_dir ]] && rm -f $vhost_dir/*
	mkdir -p $vhost_dir

	if [[ ! -x $vhost_app ]]; then
		error "application not found: $vhost_app"
		return 1
	fi

	local cmd="$vhost_app -r $vhost_dir/rpc.sock $*"

	notice "Loging to:   $vhost_log_file"
	notice "Socket:      $vhost_socket"
	notice "Command:     $cmd"

	timing_enter vhost_start
	cd $vhost_dir; $cmd &
	vhost_pid=$!
	echo $vhost_pid > $vhost_pid_file

	notice "waiting for app to run..."
	waitforlisten "$vhost_pid" "$vhost_dir/rpc.sock"
	#do not generate nvmes if pci access is disabled
	if [[ "$cmd" != *"--no-pci"* ]] && [[ "$cmd" != *"-u"* ]] && $run_gen_nvme; then
		$rootdir/scripts/gen_nvme.sh "--json" | $rootdir/scripts/rpc.py\
		 -s $vhost_dir/rpc.sock load_subsystem_config
	fi

	notice "vhost started - pid=$vhost_pid"
	timing_exit vhost_start
}

function vhost_load_config()
{
	local vhost_num="$1"
	local vhost_json_conf="$2"
	local vhost_dir
	vhost_dir="$(get_vhost_dir $vhost_num)"

	$rootdir/scripts/rpc.py -s $vhost_dir/rpc.sock load_config < "$vhost_json_conf"
}

function vhost_kill()
{
	local rc=0
	local vhost_name="$1"

	if [[ -z "$vhost_name" ]]; then
		error "Must provide vhost name to vhost_kill"
		return 0
	fi

	local vhost_dir
	vhost_dir="$(get_vhost_dir $vhost_name)"
	local vhost_pid_file="$vhost_dir/vhost.pid"

	if [[ ! -r $vhost_pid_file ]]; then
		warning "no vhost pid file found"
		return 0
	fi

	timing_enter vhost_kill
	local vhost_pid
	vhost_pid="$(cat $vhost_pid_file)"
	notice "killing vhost (PID $vhost_pid) app"

	if kill -INT $vhost_pid > /dev/null; then
		notice "sent SIGINT to vhost app - waiting 60 seconds to exit"
		for ((i=0; i<60; i++)); do
			if kill -0 $vhost_pid; then
				echo "."
				sleep 1
			else
				break
			fi
		done
		if kill -0 $vhost_pid; then
			error "ERROR: vhost was NOT killed - sending SIGABRT"
			kill -ABRT $vhost_pid
			rm $vhost_pid_file
			rc=1
		else
			while kill -0 $vhost_pid; do
				echo "."
			done
		fi
	elif kill -0 $vhost_pid; then
		error "vhost NOT killed - you need to kill it manually"
		rc=1
	else
		notice "vhost was not running"
	fi

	timing_exit vhost_kill
	if [[ $rc == 0 ]]; then
		rm $vhost_pid_file
	fi

	rm -rf "$vhost_dir"

	return $rc
}

function vhost_rpc
{
	local vhost_name="$1"

	if [[ -z "$vhost_name" ]]; then
		error "vhost name must be provided to vhost_rpc"
		return 1
	fi
	shift

	$rootdir/scripts/rpc.py -s $(get_vhost_dir $vhost_name)/rpc.sock "$@"
}

###
# Mgmt functions
###

function assert_number()
{
	[[ "$1" =~ [0-9]+ ]] && return 0

	error "Invalid or missing paramter: need number but got '$1'"
	return 1;
}

# Run command on vm with given password
# First argument - vm number
# Second argument - ssh password for vm
#
function vm_sshpass()
{
	vm_num_is_valid $1 || return 1

	local ssh_cmd
	ssh_cmd="sshpass -p $2 ssh \
		-o UserKnownHostsFile=/dev/null \
		-o StrictHostKeyChecking=no \
		-o User=root \
		-p $(vm_ssh_socket $1) $VM_SSH_OPTIONS 127.0.0.1"

	shift 2
	$ssh_cmd "$@"
}


# Helper to validate VM number
# param $1 VM number
#
function vm_num_is_valid()
{
	[[ "$1" =~ ^[0-9]+$ ]] && return 0

	error "Invalid or missing paramter: vm number '$1'"
	return 1;
}


# Print network socket for given VM number
# param $1 virtual machine number
#
function vm_ssh_socket()
{
	vm_num_is_valid $1 || return 1
	local vm_dir="$VM_DIR/$1"

	cat $vm_dir/ssh_socket
}

function vm_fio_socket()
{
	vm_num_is_valid $1 || return 1
	local vm_dir="$VM_DIR/$1"

	cat $vm_dir/fio_socket
}

# Execute command on given VM
# param $1 virtual machine number
#
function vm_exec()
{
	vm_num_is_valid $1 || return 1

	local vm_num="$1"
	shift

	sshpass -p "$VM_PASSWORD" ssh \
		-o UserKnownHostsFile=/dev/null \
		-o StrictHostKeyChecking=no \
		-o User=root \
		-p $(vm_ssh_socket $vm_num) $VM_SSH_OPTIONS 127.0.0.1 \
		"$@"
}

# Execute scp command on given VM
# param $1 virtual machine number
#
function vm_scp()
{
	vm_num_is_valid $1 || return 1

	local vm_num="$1"
	shift

	sshpass -p "$VM_PASSWORD" scp \
		-o UserKnownHostsFile=/dev/null \
		-o StrictHostKeyChecking=no \
		-o User=root \
		-P $(vm_ssh_socket $vm_num) $VM_SSH_OPTIONS \
		"$@"
}


# check if specified VM is running
# param $1 VM num
function vm_is_running()
{
	vm_num_is_valid $1 || return 1
	local vm_dir="$VM_DIR/$1"

	if [[ ! -r $vm_dir/qemu.pid ]]; then
		return 1
	fi

	local vm_pid
	vm_pid="$(cat $vm_dir/qemu.pid)"

	if /bin/kill -0 $vm_pid; then
		return 0
	else
		if [[ $EUID -ne 0 ]]; then
			warning "not root - assuming VM running since can't be checked"
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
	local vm_dir="$VM_DIR/$1"

	if [[ ! -r $vm_dir/qemu.pid ]]; then
		error "VM $1 is not running"
		return 1
	fi

	if ! VM_SSH_OPTIONS="-o ControlMaster=no" vm_exec $1 "true" 2>/dev/null; then
		# Shutdown existing master. Ignore errors as it might not exist.
		VM_SSH_OPTIONS="-O exit" vm_exec $1 "true" 2>/dev/null
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
	local vm_dir="$VM_DIR/$1"
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
	notice "Shutting down virtual machine $vm_dir"
	set +e
	vm_exec $1 "nohup sh -c 'shutdown -h -P now'" || true
	notice "VM$1 is shutting down - wait a while to complete"
	set -e
}

# Kill given VM
# param $1 virtual machine number
#
function vm_kill()
{
	vm_num_is_valid $1 || return 1
	local vm_dir="$VM_DIR/$1"

	if [[ ! -r $vm_dir/qemu.pid ]]; then
		return 0
	fi

	local vm_pid
	vm_pid="$(cat $vm_dir/qemu.pid)"

	notice "Killing virtual machine $vm_dir (pid=$vm_pid)"
	# First kill should fail, second one must fail
	if /bin/kill $vm_pid; then
		notice "process $vm_pid killed"
		rm $vm_dir/qemu.pid
		rm -rf $vm_dir
	elif vm_is_running $1; then
		error "Process $vm_pid NOT killed"
		return 1
	fi
}

# List all VM numbers in VM_DIR
#
function vm_list_all()
{
	local vms
	vms="$(shopt -s nullglob; echo $VM_DIR/[0-9]*)"
	if [[ -n "$vms" ]]; then
		basename --multiple $vms
	fi
}

# Kills all VM in $VM_DIR
#
function vm_kill_all()
{
	local vm
	for vm in $(vm_list_all); do
		vm_kill $vm
	done

	rm -rf $VM_DIR
}

# Shutdown all VM in $VM_DIR
#
function vm_shutdown_all()
{
	# XXX: temporarily disable to debug shutdown issue
	# xtrace_disable

	local vms
	vms=$(vm_list_all)
	local vm

	for vm in $vms; do
		vm_shutdown $vm
	done

	notice "Waiting for VMs to shutdown..."
	local timeo=30
	while [[ $timeo -gt 0 ]]; do
		local all_vms_down=1
		for vm in $vms; do
			if vm_is_running $vm; then
				all_vms_down=0
				break
			fi
		done

		if [[ $all_vms_down == 1 ]]; then
			notice "All VMs successfully shut down"
			xtrace_restore
			return 0
		fi

		((timeo-=1))
		sleep 1
	done

	rm -rf $VM_DIR

	xtrace_restore
}

function vm_setup()
{
	xtrace_disable
	local OPTIND optchar vm_num

	local os=""
	local os_mode=""
	local qemu_args=""
	local disk_type_g=NOT_DEFINED
	local read_only="false"
	local disks=""
	local raw_cache=""
	local vm_incoming=""
	local vm_migrate_to=""
	local force_vm=""
	local guest_memory=1024
	local queue_number=""
	local vhost_dir
	vhost_dir="$(get_vhost_dir 0)"
	while getopts ':-:' optchar; do
		case "$optchar" in
			-)
			case "$OPTARG" in
				os=*) os="${OPTARG#*=}" ;;
				os-mode=*) os_mode="${OPTARG#*=}" ;;
				qemu-args=*) qemu_args="${qemu_args} ${OPTARG#*=}" ;;
				disk-type=*) disk_type_g="${OPTARG#*=}" ;;
				read-only=*) read_only="${OPTARG#*=}" ;;
				disks=*) disks="${OPTARG#*=}" ;;
				raw-cache=*) raw_cache=",cache${OPTARG#*=}" ;;
				force=*) force_vm=${OPTARG#*=} ;;
				memory=*) guest_memory=${OPTARG#*=} ;;
				queue_num=*) queue_number=${OPTARG#*=} ;;
				incoming=*) vm_incoming="${OPTARG#*=}" ;;
				migrate-to=*) vm_migrate_to="${OPTARG#*=}" ;;
				vhost-name=*) vhost_dir="$(get_vhost_dir ${OPTARG#*=})" ;;
				spdk-boot=*) local boot_from="${OPTARG#*=}" ;;
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
	if [[ -n $force_vm ]]; then
		vm_num=$force_vm

		vm_num_is_valid $vm_num || return 1
		local vm_dir="$VM_DIR/$vm_num"
		[[ -d $vm_dir ]] && warning "removing existing VM in '$vm_dir'"
	else
		local vm_dir=""

		set +x
		for (( i=0; i<=256; i++)); do
			local vm_dir="$VM_DIR/$i"
			[[ ! -d $vm_dir ]] && break
		done
		xtrace_restore

		vm_num=$i
	fi

	if [[ $vm_num -eq 256 ]]; then
		error "no free VM found. do some cleanup (256 VMs created, are you insane?)"
		return 1
	fi

	if [[ -n "$vm_migrate_to" && -n "$vm_incoming" ]]; then
		error "'--incoming' and '--migrate-to' cannot be used together"
		return 1
	elif [[ -n "$vm_incoming" ]]; then
		if [[ -n "$os_mode" || -n "$os" ]]; then
			error "'--incoming' can't be used together with '--os' nor '--os-mode'"
			return 1
		fi

		os_mode="original"
		os="$VM_DIR/$vm_incoming/os.qcow2"
	elif [[ -n "$vm_migrate_to" ]]; then
		[[ "$os_mode" != "backing" ]] && warning "Using 'backing' mode for OS since '--migrate-to' is used"
		os_mode=backing
	fi

	notice "Creating new VM in $vm_dir"
	mkdir -p $vm_dir

	if [[ "$os_mode" == "backing" ]]; then
		notice "Creating backing file for OS image file: $os"
		if ! $QEMU_IMG_BIN create -f qcow2 -b $os $vm_dir/os.qcow2; then
			error "Failed to create OS backing file in '$vm_dir/os.qcow2' using '$os'"
			return 1
		fi

		local os=$vm_dir/os.qcow2
	elif [[ "$os_mode" == "original" ]]; then
		warning "Using original OS image file: $os"
	elif [[ "$os_mode" != "snapshot" ]]; then
		if [[ -z "$os_mode" ]]; then
			notice "No '--os-mode' parameter provided - using 'snapshot'"
			os_mode="snapshot"
		else
			error "Invalid '--os-mode=$os_mode'"
			return 1
		fi
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

	notice "TASK MASK: $task_mask"
	local cmd="taskset -a -c $task_mask $QEMU_BIN ${eol}"
	local vm_socket_offset=$(( 10000 + 100 * vm_num ))

	local ssh_socket=$(( vm_socket_offset + 0 ))
	local fio_socket=$(( vm_socket_offset + 1 ))
	local monitor_port=$(( vm_socket_offset + 2 ))
	local migration_port=$(( vm_socket_offset + 3 ))
	local gdbserver_socket=$(( vm_socket_offset + 4 ))
	local vnc_socket=$(( 100 + vm_num ))
	local qemu_pid_file="$vm_dir/qemu.pid"
	local cpu_num=0

	set +x
	# cpu list for taskset can be comma separated or range
	# or both at the same time, so first split on commas
	cpu_list=$(echo $task_mask | tr "," "\n")
	queue_number=0
	for c in $cpu_list; do
		# if range is detected - count how many cpus
		if [[ $c =~ [0-9]+-[0-9]+ ]]; then
			val=$((c-1))
			val=${val#-}
		else
			val=1
		fi
		cpu_num=$((cpu_num+val))
		queue_number=$((queue_number+val))
	done

	if [ -z $queue_number ]; then
		queue_number=$cpu_num
	fi

	xtrace_restore

	local node_num=${!qemu_numa_node_param}
	local boot_disk_present=false
	notice "NUMA NODE: $node_num"
	cmd+="-m $guest_memory --enable-kvm -cpu host -smp $cpu_num -vga std -vnc :$vnc_socket -daemonize ${eol}"
	cmd+="-object memory-backend-file,id=mem,size=${guest_memory}M,mem-path=/dev/hugepages,share=on,prealloc=yes,host-nodes=$node_num,policy=bind ${eol}"
	[[ $os_mode == snapshot ]] && cmd+="-snapshot ${eol}"
	[[ -n "$vm_incoming" ]] && cmd+=" -incoming tcp:0:$migration_port ${eol}"
	cmd+="-monitor telnet:127.0.0.1:$monitor_port,server,nowait ${eol}"
	cmd+="-numa node,memdev=mem ${eol}"
	cmd+="-pidfile $qemu_pid_file ${eol}"
	cmd+="-serial file:$vm_dir/serial.log ${eol}"
	cmd+="-D $vm_dir/qemu.log ${eol}"
	cmd+="-chardev file,path=$vm_dir/seabios.log,id=seabios -device isa-debugcon,iobase=0x402,chardev=seabios ${eol}"
	cmd+="-net user,hostfwd=tcp::$ssh_socket-:22,hostfwd=tcp::$fio_socket-:8765 ${eol}"
	cmd+="-net nic ${eol}"
	if [[ -z "$boot_from" ]]; then
		cmd+="-drive file=$os,if=none,id=os_disk ${eol}"
		cmd+="-device ide-hd,drive=os_disk,bootindex=0 ${eol}"
	fi

	if [[ $disks == '' ]] && [[ $disk_type_g == virtio* ]]; then
		disks=1
	fi

	for disk in ${disks//:/ }; do
		if [[ $disk = *","* ]]; then
			disk_type=${disk#*,}
			disk=${disk%,*}
		else
			disk_type=$disk_type_g
		fi

		case $disk_type in
			virtio)
				local raw_name="RAWSCSI"
				local raw_disk=$vm_dir/test.img

				if [[ -n $disk ]]; then
					[[ ! -b $disk ]] && touch $disk
					local raw_disk
					raw_disk=$(readlink -f $disk)
				fi

				# Create disk file if it not exist or it is smaller than 1G
				if { [[ -f $raw_disk ]] && [[ $(stat --printf="%s" $raw_disk) -lt $((1024 * 1024 * 1024)) ]]; } || \
					[[ ! -e $raw_disk ]]; then
					if [[ $raw_disk =~ /dev/.* ]]; then
						error \
							"ERROR: Virtio disk point to missing device ($raw_disk) -\n" \
							"       this is probably not what you want."
							return 1
					fi

					notice "Creating Virtio disc $raw_disk"
					dd if=/dev/zero of=$raw_disk bs=1024k count=1024
				else
					notice "Using existing image $raw_disk"
				fi

				cmd+="-device virtio-scsi-pci,num_queues=$queue_number ${eol}"
				cmd+="-device scsi-hd,drive=hd$i,vendor=$raw_name ${eol}"
				cmd+="-drive if=none,id=hd$i,file=$raw_disk,format=raw$raw_cache ${eol}"
				;;
			spdk_vhost_scsi)
				notice "using socket $vhost_dir/naa.$disk.$vm_num"
				cmd+="-chardev socket,id=char_$disk,path=$vhost_dir/naa.$disk.$vm_num ${eol}"
				cmd+="-device vhost-user-scsi-pci,id=scsi_$disk,num_queues=$queue_number,chardev=char_$disk"
				if [[ "$disk" == "$boot_from" ]]; then
					cmd+=",bootindex=0"
					boot_disk_present=true
				fi
				cmd+=" ${eol}"
				;;
			spdk_vhost_blk)
				notice "using socket $vhost_dir/naa.$disk.$vm_num"
				cmd+="-chardev socket,id=char_$disk,path=$vhost_dir/naa.$disk.$vm_num ${eol}"
				cmd+="-device vhost-user-blk-pci,num-queues=$queue_number,chardev=char_$disk"
				if [[ "$disk" == "$boot_from" ]]; then
					cmd+=",bootindex=0"
					boot_disk_present=true
				fi
				cmd+=" ${eol}"
				;;
			kernel_vhost)
				if [[ -z $disk ]]; then
					error "need WWN for $disk_type"
					return 1
				elif [[ ! $disk =~ ^[[:alpha:]]{3}[.][[:xdigit:]]+$ ]]; then
					error "$disk_type - disk(wnn)=$disk does not look like WNN number"
					return 1
				fi
				notice "Using kernel vhost disk wwn=$disk"
				cmd+=" -device vhost-scsi-pci,wwpn=$disk,num_queues=$queue_number ${eol}"
				;;
			*)
				error "unknown mode '$disk_type', use: virtio, spdk_vhost_scsi, spdk_vhost_blk or kernel_vhost"
				return 1
		esac
	done

	if [[ -n $boot_from ]] && [[ $boot_disk_present == false ]]; then
		error "Boot from $boot_from is selected but device is not present"
		return 1
	fi

	[[ -n $qemu_args ]] && cmd+=" $qemu_args ${eol}"
	# remove last $eol
	cmd="${cmd%\\\\\\n  }"

	notice "Saving to $vm_dir/run.sh"
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
	echo $monitor_port > $vm_dir/monitor_port

	rm -f $vm_dir/migration_port
	[[ -z $vm_incoming ]] || echo $migration_port > $vm_dir/migration_port

	echo $gdbserver_socket > $vm_dir/gdbserver_socket
	echo $vnc_socket >> $vm_dir/vnc_socket

	[[ -z $vm_incoming ]] || ln -fs $VM_DIR/$vm_incoming $vm_dir/vm_incoming
	[[ -z $vm_migrate_to ]] || ln -fs $VM_DIR/$vm_migrate_to $vm_dir/vm_migrate_to
}

function vm_run()
{
	local OPTIND optchar vm
	local run_all=false
	local vms_to_run=""

	while getopts 'a-:' optchar; do
		case "$optchar" in
			a) run_all=true ;;
			*)
				error "Unknown param $OPTARG"
				return 1
			;;
		esac
	done

	if $run_all; then
		vms_to_run="$(vm_list_all)"
	else
		shift $((OPTIND-1))
		for vm in "$@"; do
			vm_num_is_valid $1 || return 1
			if [[ ! -x $VM_DIR/$vm/run.sh ]]; then
				error "VM$vm not defined - setup it first"
				return 1
			fi
			vms_to_run+=" $vm"
		done
	fi

	for vm in $vms_to_run; do
		if vm_is_running $vm; then
			warning "VM$vm ($VM_DIR/$vm) already running"
			continue
		fi

		notice "running $VM_DIR/$vm/run.sh"
		if ! $VM_DIR/$vm/run.sh; then
			error "FAILED to run vm $vm"
			return 1
		fi
	done
}

function vm_print_logs()
{
	vm_num=$1
	warning "================"
	warning "QEMU LOG:"
	if [[ -r $VM_DIR/$vm_num/qemu.log ]]; then
		cat $VM_DIR/$vm_num/qemu.log
	else
		warning "LOG qemu.log not found"
	fi

	warning "VM LOG:"
	if [[ -r $VM_DIR/$vm_num/serial.log ]]; then
		cat $VM_DIR/$vm_num/serial.log
	else
		warning "LOG serial.log not found"
	fi

	warning "SEABIOS LOG:"
	if [[ -r $VM_DIR/$vm_num/seabios.log ]]; then
		cat $VM_DIR/$vm_num/seabios.log
	else
		warning "LOG seabios.log not found"
	fi
	warning "================"
}

# Wait for all created VMs to boot.
# param $1 max wait time
function vm_wait_for_boot()
{
	assert_number $1

	xtrace_disable

	local all_booted=false
	local timeout_time=$1
	[[ $timeout_time -lt 10 ]] && timeout_time=10
	local timeout_time
	timeout_time=$(date -d "+$timeout_time seconds" +%s)

	notice "Waiting for VMs to boot"
	shift
	if [[ "$*" == "" ]]; then
		local vms_to_check="$VM_DIR/[0-9]*"
	else
		local vms_to_check=""
		for vm in "$@"; do
			vms_to_check+=" $VM_DIR/$vm"
		done
	fi

	for vm in $vms_to_check; do
		local vm_num
		vm_num=$(basename $vm)
		local i=0
		notice "waiting for VM$vm_num ($vm)"
		while ! vm_os_booted $vm_num; do
			if ! vm_is_running $vm_num; then
				warning "VM $vm_num is not running"
				vm_print_logs $vm_num
				xtrace_restore
				return 1
			fi

			if [[ $(date +%s) -gt $timeout_time ]]; then
				warning "timeout waiting for machines to boot"
				vm_print_logs $vm_num
				xtrace_restore
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
		#Change Timeout for stopping services to prevent lengthy powerdowns
		#Check that remote system is not Cygwin in case of Windows VMs
		local vm_os
		vm_os=$(vm_exec $vm_num "uname -o")
		if [[ "$vm_os" != "Cygwin" ]]; then
			vm_exec $vm_num "echo 'DefaultTimeoutStopSec=10' >> /etc/systemd/system.conf; systemctl daemon-reexec"
		fi
	done

	notice "all VMs ready"
	xtrace_restore
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
				*) error "Invalid argument '$OPTARG'" && return 1;;
			esac
			;;
			*) error "Invalid argument '$OPTARG'" && return 1;;
		esac
	done

	shift $(( OPTIND - 1 ))
	for vm_num in "$@"; do
		notice "Starting fio server on VM$vm_num"
		if [[ $fio_bin != "" ]]; then
			vm_exec $vm_num 'cat > /root/fio; chmod +x /root/fio' < $fio_bin
			vm_exec $vm_num /root/fio $readonly --eta=never --server --daemonize=/root/fio.pid
		else
			vm_exec $vm_num fio $readonly --eta=never --server --daemonize=/root/fio.pid
		fi
	done
}

function vm_check_scsi_location()
{
	# Script to find wanted disc
	local script='shopt -s nullglob;
	for entry in /sys/block/sd*; do
		disk_type="$(cat $entry/device/vendor)";
		if [[ $disk_type == INTEL* ]] || [[ $disk_type == RAWSCSI* ]] || [[ $disk_type == LIO-ORG* ]]; then
			fname=$(basename $entry);
			echo -n " $fname";
		fi;
	done'

	SCSI_DISK="$(echo "$script" | vm_exec $1 bash -s)"

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
		vm_exec $1 sg_reset /dev/$disk -vNd
	done
}

function vm_check_blk_location()
{
	local script='shopt -s nullglob; cd /sys/block; echo vd*'
	SCSI_DISK="$(echo "$script" | vm_exec $1 bash -s)"

	if [[ -z "$SCSI_DISK" ]]; then
		error "no blk test disk found!"
		return 1
	fi
}

function run_fio()
{
	local arg
	local job_file=""
	local fio_bin=""
	local vms=()
	local out=""
	local fio_disks=""
	local vm
	local run_server_mode=true

	for arg in "$@"; do
		case "$arg" in
			--job-file=*) local job_file="${arg#*=}" ;;
			--fio-bin=*) local fio_bin="${arg#*=}" ;;
			--vm=*) vms+=( "${arg#*=}" ) ;;
			--out=*)
				local out="${arg#*=}"
				mkdir -p $out
				;;
			--local) run_server_mode=false ;;
			--json) json="--json" ;;
		*)
			error "Invalid argument '$arg'"
			return 1
			;;
		esac
	done

	if [[ -n "$fio_bin" && ! -r "$fio_bin" ]]; then
		error "FIO binary '$fio_bin' does not exist"
		return 1
	fi

	if [[ ! -r "$job_file" ]]; then
		error "Fio job '$job_file' does not exist"
		return 1
	fi

	local job_fname
	job_fname=$(basename "$job_file")
	# prepare job file for each VM
	for vm in "${vms[@]}"; do
		local vm_num=${vm%%:*}
		local vmdisks=${vm#*:}

		sed "s@filename=@filename=$vmdisks@" $job_file | vm_exec $vm_num "cat > /root/$job_fname"
		fio_disks+="127.0.0.1:$(vm_fio_socket $vm_num):$vmdisks,"

		vm_exec $vm_num cat /root/$job_fname
		if ! $run_server_mode; then
			if [[ -n "$fio_bin" ]]; then
				vm_exec $vm_num 'cat > /root/fio; chmod +x /root/fio' < $fio_bin
			fi

			notice "Running local fio on VM $vm_num"
			vm_exec $vm_num "nohup /root/fio /root/$job_fname 1>/root/$job_fname.out 2>/root/$job_fname.out </dev/null & echo \$! > /root/fio.pid"
		fi
	done

	if ! $run_server_mode; then
		# Give FIO time to run
		sleep 0.5
		return 0
	fi

	$rootdir/test/vhost/common/run_fio.py --job-file=/root/$job_fname \
		$([[ -n "$fio_bin" ]] && echo "--fio-bin=$fio_bin") \
		--out=$out $json ${fio_disks%,}
}

# Shutdown or kill any running VM and SPDK APP.
#
function at_app_exit()
{
	local vhost_name

	notice "APP EXITING"
	notice "killing all VMs"
	vm_kill_all
	# Kill vhost application
	notice "killing vhost app"

	for vhost_name in "$TARGET_DIR"/*; do
		vhost_kill $vhost_name
	done

	notice "EXIT DONE"
}

function error_exit()
{
	trap - ERR
	print_backtrace
	set +e
	error "Error on $1 $2"

	at_app_exit
	exit 1
}
