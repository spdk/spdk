: ${SPDK_AUTOTEST_X=true}; export SPDK_AUTOTEST_X

if $SPDK_AUTOTEST_X; then
	set -x
fi

set -e

# Export flag to skip the known bug that exists in librados
# Bug is reported on ceph bug tracker with number 24078
export ASAN_OPTIONS=new_delete_type_mismatch=0

PS4=' \t	\$ '
ulimit -c unlimited

: ${RUN_NIGHTLY:=0}
export RUN_NIGHTLY

: ${RUN_NIGHTLY_FAILING:=0}
export RUN_NIGHTLY_FAILING

if [[ ! -z $1 ]]; then
	if [ -f $1 ]; then
		source $1
	fi
fi

# If certain utilities are not installed, preemptively disable the tests
if ! hash ceph; then
	SPDK_TEST_RBD=0
fi

if ! hash pmempool; then
	SPDK_TEST_PMDK=0
fi

# Set defaults for missing test config options
: ${SPDK_BUILD_DOC=1}; export SPDK_BUILD_DOC
: ${SPDK_BUILD_SHARED_OBJECT=1}; export SPDK_BUILD_SHARED_OBJECT
: ${SPDK_RUN_CHECK_FORMAT=1}; export SPDK_RUN_CHECK_FORMAT
: ${SPDK_RUN_SCANBUILD=1}; export SPDK_RUN_SCANBUILD
: ${SPDK_RUN_VALGRIND=1}; export SPDK_RUN_VALGRIND
: ${SPDK_TEST_UNITTEST=1}; export SPDK_TEST_UNITTEST
: ${SPDK_TEST_ISCSI=1}; export SPDK_TEST_ISCSI
: ${SPDK_TEST_ISCSI_INITIATOR=1}; export SPDK_TEST_ISCSI_INITIATOR
: ${SPDK_TEST_NVME=1}; export SPDK_TEST_NVME
: ${SPDK_TEST_NVME_CLI=1}; export SPDK_TEST_NVME_CLI
: ${SPDK_TEST_NVMF=1}; export SPDK_TEST_NVMF
: ${SPDK_TEST_RBD=1}; export SPDK_TEST_RBD
: ${SPDK_TEST_VHOST=1}; export SPDK_TEST_VHOST
: ${SPDK_TEST_BLOCKDEV=1}; export SPDK_TEST_BLOCKDEV
: ${SPDK_TEST_IOAT=1}; export SPDK_TEST_IOAT
: ${SPDK_TEST_EVENT=1}; export SPDK_TEST_EVENT
: ${SPDK_TEST_BLOBFS=1}; export SPDK_TEST_BLOBFS
: ${SPDK_TEST_VHOST_INIT=1}; export SPDK_TEST_VHOST_INIT
: ${SPDK_TEST_PMDK=1}; export SPDK_TEST_PMDK
: ${SPDK_TEST_LVOL=1}; export SPDK_TEST_LVOL
: ${SPDK_TEST_JSON=1}; export SPDK_TEST_JSON
: ${SPDK_RUN_ASAN=1}; export SPDK_RUN_ASAN
: ${SPDK_RUN_UBSAN=1}; export SPDK_RUN_UBSAN
: ${SPDK_RUN_INSTALLED_DPDK=1}; export SPDK_RUN_INSTALLED_DPDK
: ${SPDK_TEST_CRYPTO=1}; export SPDK_TEST_CRYPTO

if [ -z "$DEPENDENCY_DIR" ]; then
	export DEPENDENCY_DIR=/home/sys_sgsw
else
	export DEPENDENCY_DIR
fi

if [ ! -z "$HUGEMEM" ]; then
	export HUGEMEM
fi

# pass our valgrind desire on to unittest.sh
if [ $SPDK_RUN_VALGRIND -eq 0 ]; then
	export valgrind=''
fi

config_params='--enable-debug --enable-werror'

if echo -e "#include <libunwind.h>\nint main(int argc, char *argv[]) {return 0;}\n" | \
	gcc -o /dev/null -lunwind -x c - 2>/dev/null; then
	config_params+=' --enable-log-bt'
fi

if [ $SPDK_TEST_CRYPTO -eq 1 ]; then
	config_params+=' --with-crypto'
fi

export UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1:abort_on_error=1'

# On Linux systems, override the default HUGEMEM in scripts/setup.sh to
#  allocate 8GB in hugepages.
# FreeBSD runs a much more limited set of tests, so keep the default 2GB.
if [ `uname -s` = "Linux" ]; then
	export HUGEMEM=8192
fi

DEFAULT_RPC_ADDR=/var/tmp/spdk.sock

case `uname` in
	FreeBSD)
		DPDK_FREEBSD_DIR=/usr/local/share/dpdk/x86_64-native-bsdapp-clang
		if [ -d $DPDK_FREEBSD_DIR ] && [ $SPDK_RUN_INSTALLED_DPDK -eq 1 ]; then
			WITH_DPDK_DIR=$DPDK_FREEBSD_DIR
		fi
		MAKE=gmake
		MAKEFLAGS=${MAKEFLAGS:--j$(sysctl -a | egrep -i 'hw.ncpu' | awk '{print $2}')}
		SPDK_RUN_ASAN=0
		SPDK_RUN_UBSAN=0
		;;
	Linux)
		DPDK_LINUX_DIR=/usr/share/dpdk/x86_64-default-linuxapp-gcc
		if [ -d $DPDK_LINUX_DIR ] && [ $SPDK_RUN_INSTALLED_DPDK -eq 1 ]; then
			WITH_DPDK_DIR=$DPDK_LINUX_DIR
		fi
		MAKE=make
		MAKEFLAGS=${MAKEFLAGS:--j$(nproc)}
		config_params+=' --enable-coverage'
		if [ $SPDK_RUN_UBSAN -eq 1 ]; then
			config_params+=' --enable-ubsan'
		fi
		if [ $SPDK_RUN_ASAN -eq 1 ]; then
			if ldconfig -p | grep -q asan; then
				config_params+=' --enable-asan'
			else
				SPDK_RUN_ASAN=0
			fi
		fi
		;;
	*)
		echo "Unknown OS in $0"
		exit 1
		;;
esac

# By default, --with-dpdk is not set meaning the SPDK build will use the DPDK submodule.
# If a DPDK installation is found in a well-known location though, WITH_DPDK_DIR will be
# set which will override the default and use that DPDK installation instead.
if [ ! -z "$WITH_DPDK_DIR" ]; then
	config_params+=" --with-dpdk=$WITH_DPDK_DIR"
fi

if [ -f /usr/include/infiniband/verbs.h ]; then
	config_params+=' --with-rdma'
fi

if [ -f /usr/include/libpmemblk.h ]; then
	config_params+=' --with-pmdk'
else
	# PMDK not installed so disable PMDK tests explicitly here
	SPDK_TEST_PMDK=0; export SPDK_TEST_PMDK
fi

if [ -d /usr/src/fio ]; then
	config_params+=' --with-fio=/usr/src/fio'
fi

if [ -d ${DEPENDENCY_DIR}/vtune_codes ]; then
	config_params+=' --with-vtune='${DEPENDENCY_DIR}'/vtune_codes'
fi

if [ -d /usr/include/rbd ] &&  [ -d /usr/include/rados ]; then
	config_params+=' --with-rbd'
fi

if [ -d /usr/include/iscsi ]; then
	libiscsi_version=`grep LIBISCSI_API_VERSION /usr/include/iscsi/iscsi.h | head -1 | awk '{print $3}' | awk -F '(' '{print $2}' | awk -F ')' '{print $1}'`
	if [ $libiscsi_version -ge 20150621 ]; then
		config_params+=' --with-iscsi-initiator'
	else
		export SPDK_TEST_ISCSI_INITIATOR=0
	fi
else
	export SPDK_TEST_ISCSI_INITIATOR=0
fi

if [ ! -d "${DEPENDENCY_DIR}/nvme-cli" ]; then
	export SPDK_TEST_NVME_CLI=0
fi

export config_params

if [ -z "$output_dir" ]; then
	if [ -z "$rootdir" ] || [ ! -d "$rootdir/../output" ]; then
		output_dir=.
	else
		output_dir=$rootdir/../output
	fi
	export output_dir
fi

function timing() {
	direction="$1"
	testname="$2"

	now=$(date +%s)

	if [ "$direction" = "enter" ]; then
		export timing_stack="${timing_stack};${now}"
		export test_stack="${test_stack};${testname}"
	else
		child_time=$(grep "^${test_stack:1};" $output_dir/timing.txt | awk '{s+=$2} END {print s}')

		start_time=$(echo "$timing_stack" | sed -e 's@^.*;@@')
		timing_stack=$(echo "$timing_stack" | sed -e 's@;[^;]*$@@')

		elapsed=$((now - start_time - child_time))
		echo "${test_stack:1} $elapsed" >> $output_dir/timing.txt

		test_stack=$(echo "$test_stack" | sed -e 's@;[^;]*$@@')
	fi
}

function timing_enter() {
	set +x
	timing "enter" "$1"
	set -x
}

function timing_exit() {
	set +x
	timing "exit" "$1"
	set -x
}

function timing_finish() {
	flamegraph='/usr/local/FlameGraph/flamegraph.pl'
	if [ -x "$flamegraph" ]; then
		"$flamegraph" \
			--title 'Build Timing' \
			--nametype 'Step:' \
			--countname seconds \
			$output_dir/timing.txt \
			>$output_dir/timing.svg
	fi
}

function create_test_list() {
	grep -rsh --exclude="autotest_common.sh" --exclude="$rootdir/test/common/autotest_common.sh" -e "report_test_completion" $rootdir | sed 's/report_test_completion//g; s/[[:blank:]]//g; s/"//g;' > $output_dir/all_tests.txt || true
}

function report_test_completion() {
	echo "$1" >> $output_dir/test_completions.txt
}

function process_core() {
	ret=0
	for core in $(find . -type f \( -name 'core*' -o -name '*.core' \)); do
		exe=$(eu-readelf -n "$core" | grep psargs | sed "s/.*psargs: \([^ \'\" ]*\).*/\1/")
		if [[ ! -f "$exe" ]]; then
			exe=$(eu-readelf -n "$core" | grep -oP -m1 "$exe.+")
		fi
		echo "exe for $core is $exe"
		if [[ ! -z "$exe" ]]; then
			if hash gdb; then
				gdb -batch -ex "thread apply all bt full" $exe $core
			fi
			cp $exe $output_dir
		fi
		mv $core $output_dir
		chmod a+r $output_dir/$core
		ret=1
	done
	return $ret
}

function process_shm() {
	type=$1
	id=$2
	if [ "$type" = "--pid" ]; then
		id="pid${id}"
	elif [ "$type" = "--id" ]; then
		id="${id}"
	else
		echo "Please specify to search for pid or shared memory id."
		return 1
	fi

	shm_files=$(find /dev/shm -name "*.${id}" -printf "%f\n")

	if [[ -z $shm_files ]]; then
		echo "SHM File for specified PID or shared memory id: ${id} not found!"
		return 1
	fi
	for n in $shm_files; do
		tar -C /dev/shm/ -cvzf $output_dir/${n}_shm.tar.gz ${n}
	done
	return 0
}

function waitforlisten() {
	# $1 = process pid
	if [ -z "$1" ]; then
		exit 1
	fi

	rpc_addr="${2:-$DEFAULT_RPC_ADDR}"

	echo "Waiting for process to start up and listen on UNIX domain socket $rpc_addr..."
	# turn off trace for this loop
	set +x
	ret=1
	while [ $ret -ne 0 ]; do
		# if the process is no longer running, then exit the script
		#  since it means the application crashed
		if ! kill -s 0 $1; then
			exit 1
		fi

		namespace=$(ip netns identify $1)
		if [ -n "$namespace" ]; then
			ns_cmd="ip netns exec $namespace"
		fi

		if hash ss; then
			if $ns_cmd ss -lx | grep -q $rpc_addr; then
				ret=0
			fi
		else
			# if system doesn't have ss, just assume it has netstat
			if $ns_cmd netstat -an -x | grep -iw LISTENING | grep -q $rpc_addr; then
				ret=0
			fi
		fi
	done
	set -x
}

function waitfornbd() {
	nbd_name=$1

	for ((i=1; i<=20; i++)); do
		if grep -q -w $nbd_name /proc/partitions; then
			break
		else
			sleep 0.1
		fi
	done

	# The nbd device is now recognized as a block device, but there can be
	#  a small delay before we can start I/O to that block device.  So loop
	#  here trying to read the first block of the nbd block device to a temp
	#  file.  Note that dd returns success when reading an empty file, so we
	#  need to check the size of the output file instead.
	for ((i=1; i<=20; i++)); do
		dd if=/dev/$nbd_name of=/tmp/nbdtest bs=4096 count=1 iflag=direct
		size=`stat -c %s /tmp/nbdtest`
		rm -f /tmp/nbdtest
		if [ "$size" != "0" ]; then
			return 0
		else
			sleep 0.1
		fi
	done

	return 1
}

function killprocess() {
	# $1 = process pid
	if [ -z "$1" ]; then
		exit 1
	fi

	if kill -0 $1; then
		echo "killing process with pid $1"
		kill $1
		wait $1
	fi
}

function iscsicleanup() {
	echo "Cleaning up iSCSI connection"
	iscsiadm -m node --logout || true
	iscsiadm -m node -o delete || true
}

function stop_iscsi_service() {
	if cat /etc/*-release | grep Ubuntu; then
		service open-iscsi stop
	else
		service iscsid stop
	fi
}

function start_iscsi_service() {
	if cat /etc/*-release | grep Ubuntu; then
		service open-iscsi start
	else
		service iscsid start
	fi
}

function rbd_setup() {
	# $1 = monitor ip address
	# $2 = name of the namespace
	if [ -z "$1" ]; then
		echo "No monitor IP address provided for ceph"
		exit 1
	fi
	if [ -n "$2" ]; then
		if ip netns list | grep "$2"; then
			NS_CMD="ip netns exec $2"
		else
			echo "No namespace $2 exists"
			exit 1
		fi
	fi

	if hash ceph; then
		export PG_NUM=128
		export RBD_POOL=rbd
		export RBD_NAME=foo
		$NS_CMD $rootdir/scripts/ceph/start.sh $1

		$NS_CMD ceph osd pool create $RBD_POOL $PG_NUM || true
		$NS_CMD rbd create $RBD_NAME --size 1000
	fi
}

function rbd_cleanup() {
	if hash ceph; then
		$rootdir/scripts/ceph/stop.sh || true
	fi
}

function start_stub() {
	# Disable ASLR for multi-process testing.  SPDK does support using DPDK multi-process,
	# but ASLR can still be unreliable in some cases.
	# We will reenable it again after multi-process testing is complete in kill_stub()
	echo 0 > /proc/sys/kernel/randomize_va_space
	$rootdir/test/app/stub/stub $1 &
	stubpid=$!
	echo Waiting for stub to ready for secondary processes...
	while ! [ -e /var/run/spdk_stub0 ]; do
		sleep 1s
	done
	# dump process memory map contents to help debug random ASLR failures
	pmap -pX $stubpid || pmap -x $stubpid || true
	echo done.
}

function kill_stub() {
	kill $stubpid
	wait $stubpid
	rm -f /var/run/spdk_stub0
	# Re-enable ASLR now that we are done with multi-process testing
	# Note: "1" enables ASLR w/o randomizing data segments, "2" adds data segment
	#  randomizing and is the default on all recent Linux kernels
	echo 2 > /proc/sys/kernel/randomize_va_space
}

function run_test() {
	set +x
	local test_type="$(echo $1 | tr 'a-z' 'A-Z')"
	shift
	echo "************************************"
	echo "START TEST $test_type $@"
	echo "************************************"
	set -x
	time "$@"
	set +x
	echo "************************************"
	echo "END TEST $test_type $@"
	echo "************************************"
	set -x
}

function print_backtrace() {
	# if errexit is not enabled, don't print a backtrace
	[[ "$-" =~ e ]] || return 0

	local shell_options="$-"
	set +x
	echo "========== Backtrace start: =========="
	echo ""
	for i in $(seq 1 $((${#FUNCNAME[@]} - 1))); do
		local func="${FUNCNAME[$i]}"
		local line_nr="${BASH_LINENO[$((i - 1))]}"
		local src="${BASH_SOURCE[$i]}"
		echo "in $src:$line_nr -> $func()"
		echo "     ..."
		nl -w 4 -ba -nln $src | grep -B 5 -A 5 "^$line_nr[^0-9]" | \
			sed "s/^/   /g" | sed "s/^   $line_nr /=> $line_nr /g"
		echo "     ..."
	done
	echo ""
	echo "========== Backtrace end =========="
	[[ "$shell_options" =~ x ]] && set -x
	return 0
}

function part_dev_by_gpt () {
	if [ $(uname -s) = Linux ] && hash sgdisk && modprobe nbd; then
		conf=$1
		devname=$2
		rootdir=$3
		operation=$4
		local nbd_path=/dev/nbd0
		local rpc_server=/var/tmp/spdk-gpt-bdevs.sock

		if [ ! -e $conf ]; then
			return 1
		fi

		if [ -z "$operation" ]; then
			operation="create"
		fi

		cp $conf ${conf}.gpt
		echo "[Gpt]" >> ${conf}.gpt
		echo "  Disable Yes" >> ${conf}.gpt

		$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 -c ${conf}.gpt &
		nbd_pid=$!
		echo "Process nbd pid: $nbd_pid"
		waitforlisten $nbd_pid $rpc_server

		# Start bdev as a nbd device
		$rootdir/scripts/rpc.py -s "$rpc_server" start_nbd_disk $devname $nbd_path

		waitfornbd ${nbd_path:5}

		if [ "$operation" = create ]; then
			parted -s $nbd_path mklabel gpt mkpart first '0%' '50%' mkpart second '50%' '100%'

			# change the GUID to SPDK GUID value
			SPDK_GPT_GUID=`grep SPDK_GPT_PART_TYPE_GUID $rootdir/lib/bdev/gpt/gpt.h \
				| awk -F "(" '{ print $2}' | sed 's/)//g' \
				| awk -F ", " '{ print $1 "-" $2 "-" $3 "-" $4 "-" $5}' | sed 's/0x//g'`
			sgdisk -t 1:$SPDK_GPT_GUID $nbd_path
			sgdisk -t 2:$SPDK_GPT_GUID $nbd_path
		elif [ "$operation" = reset ]; then
			# clear the partition table
			dd if=/dev/zero of=$nbd_path bs=4096 count=8 oflag=direct
		fi

		$rootdir/scripts/rpc.py -s "$rpc_server" stop_nbd_disk $nbd_path

		killprocess $nbd_pid
		rm -f ${conf}.gpt
	fi

	return 0
}

function discover_bdevs()
{
	local rootdir=$1
	local config_file=$2
	local rpc_server=/var/tmp/spdk-discover-bdevs.sock

	if [ ! -e $config_file ]; then
		echo "Invalid Configuration File: $config_file"
		return -1
	fi

	# Start the bdev service to query for the list of available
	# bdevs.
	$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 -s 1024 \
		-c $config_file &>/dev/null &
	stubpid=$!
	while ! [ -e /var/run/spdk_bdev0 ]; do
		sleep 1
	done

	# Get all of the bdevs
	if [ -z "$rpc_server" ]; then
		$rootdir/scripts/rpc.py get_bdevs
	else
		$rootdir/scripts/rpc.py -s "$rpc_server" get_bdevs
	fi

	# Shut down the bdev service
	kill $stubpid
	wait $stubpid
	rm -f /var/run/spdk_bdev0
}

function waitforblk()
{
	local i=0
	while ! lsblk -l -o NAME | grep -q -w $1; do
		[ $i -lt 15 ] || break
		i=$[$i+1]
		sleep 1
	done

	if ! lsblk -l -o NAME | grep -q -w $1; then
		return 1
	fi

	return 0
}

function fio_config_gen()
{
	local config_file=$1
	local workload=$2

	if [ -e "$config_file" ]; then
		echo "Configuration File Already Exists!: $config_file"
		return -1
	fi

	if [ -z "$workload" ]; then
		workload=randrw
	fi

	touch $1

	cat > $1 << EOL
[global]
thread=1
group_reporting=1
direct=1
norandommap=1
percentile_list=50:99:99.9:99.99:99.999
time_based=1
ramp_time=0
EOL

	if [ "$workload" == "verify" ]; then
		echo "verify=sha1" >> $config_file
		echo "rw=randwrite" >> $config_file
	elif [ "$workload" == "trim" ]; then
		echo "rw=trimwrite" >> $config_file
	else
		echo "rw=$workload" >> $config_file
	fi
}

function fio_config_add_job()
{
	config_file=$1
	filename=$2

	if [ ! -e "$config_file" ]; then
		echo "Configuration File Doesn't Exist: $config_file"
		return -1
	fi

	if [ -z "$filename" ]; then
		echo "No filename provided"
		return -1
	fi

	echo "[job_$filename]" >> $config_file
	echo "filename=$filename" >> $config_file
}

function get_lvs_free_mb()
{
	local lvs_uuid=$1
	local lvs_info=$($rpc_py get_lvol_stores)
	local fc=$(jq ".[] | select(.uuid==\"$lvs_uuid\") .free_clusters" <<< "$lvs_info")
	local cs=$(jq ".[] | select(.uuid==\"$lvs_uuid\") .cluster_size" <<< "$lvs_info")

	# Change to MB's
	free_mb=$((fc*cs/1024/1024))
	echo "$free_mb"
}

function get_bdev_size()
{
	local bdev_name=$1
	local bdev_info=$($rpc_py get_bdevs -b $bdev_name)
	local bs=$(jq ".[] .block_size" <<< "$bdev_info")
	local nb=$(jq ".[] .num_blocks" <<< "$bdev_info")

	# Change to MB's
	bdev_size=$((bs*nb/1024/1024))
	echo "$bdev_size"
}

function autotest_cleanup()
{
	$rootdir/scripts/setup.sh reset
	$rootdir/scripts/setup.sh cleanup
	if [ $(uname -s) = "Linux" ]; then
		if grep -q '#define SPDK_CONFIG_IGB_UIO_DRIVER 1' $rootdir/include/spdk/config.h; then
			rmmod igb_uio
		else
			modprobe -r uio_pci_generic
		fi
	fi
}

function freebsd_update_contigmem_mod()
{
	if [ `uname` = FreeBSD ]; then
		kldunload contigmem.ko || true
		if [ ! -z "$WITH_DPDK_DIR" ]; then
			echo "Warning: SPDK only works on FreeBSD with patches that only exist in SPDK's dpdk submodule"
			cp -f "$WITH_DPDK_DIR/kmod/contigmem.ko" /boot/modules/
			cp -f "$WITH_DPDK_DIR/kmod/contigmem.ko" /boot/kernel/
		else
			cp -f "$rootdir/dpdk/build/kmod/contigmem.ko" /boot/modules/
			cp -f "$rootdir/dpdk/build/kmod/contigmem.ko" /boot/kernel/
		fi
	fi
}

set -o errtrace
trap "trap - ERR; print_backtrace >&2" ERR
