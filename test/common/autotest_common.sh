function xtrace_disable() {
	PREV_BASH_OPTS="$-"
	set +x
}

xtrace_disable
set -e
shopt -s expand_aliases

# Dummy function to be called after restoring xtrace just so that it appears in the
# xtrace log. This way we can consistently track when xtrace is enabled/disabled.
function xtrace_enable() {
	# We have to do something inside a function in bash, and calling any command
	# (even `:`) will produce an xtrace entry, so we just define another function.
	function xtrace_dummy() { :; }
}

# Keep it as alias to avoid xtrace_enable backtrace always pointing to xtrace_restore.
# xtrace_enable will appear as called directly from the user script, from the same line
# that "called" xtrace_restore.
alias xtrace_restore='if [[ "$PREV_BASH_OPTS" == *"x"* ]]; then set -x; xtrace_enable; fi'

: ${RUN_NIGHTLY:=0}
export RUN_NIGHTLY

: ${RUN_NIGHTLY_FAILING:=0}
export RUN_NIGHTLY_FAILING

# Set defaults for missing test config options
: ${SPDK_BUILD_DOC=0}; export SPDK_BUILD_DOC
: ${SPDK_BUILD_SHARED_OBJECT=0}; export SPDK_BUILD_SHARED_OBJECT
: ${SPDK_RUN_CHECK_FORMAT=0}; export SPDK_RUN_CHECK_FORMAT
: ${SPDK_RUN_SCANBUILD=0}; export SPDK_RUN_SCANBUILD
: ${SPDK_RUN_VALGRIND=0}; export SPDK_RUN_VALGRIND
: ${SPDK_RUN_FUNCTIONAL_TEST=0}; export SPDK_RUN_FUNCTIONAL_TEST
: ${SPDK_TEST_UNITTEST=0}; export SPDK_TEST_UNITTEST
: ${SPDK_TEST_ISAL=0}; export SPDK_TEST_ISAL
: ${SPDK_TEST_ISCSI=0}; export SPDK_TEST_ISCSI
: ${SPDK_TEST_ISCSI_INITIATOR=0}; export SPDK_TEST_ISCSI_INITIATOR
: ${SPDK_TEST_NVME=0}; export SPDK_TEST_NVME
: ${SPDK_TEST_NVME_CLI=0}; export SPDK_TEST_NVME_CLI
: ${SPDK_TEST_NVMF=0}; export SPDK_TEST_NVMF
: ${SPDK_TEST_RBD=0}; export SPDK_TEST_RBD
: ${SPDK_TEST_VHOST=0}; export SPDK_TEST_VHOST
: ${SPDK_TEST_BLOCKDEV=0}; export SPDK_TEST_BLOCKDEV
: ${SPDK_TEST_IOAT=0}; export SPDK_TEST_IOAT
: ${SPDK_TEST_EVENT=0}; export SPDK_TEST_EVENT
: ${SPDK_TEST_BLOBFS=0}; export SPDK_TEST_BLOBFS
: ${SPDK_TEST_VHOST_INIT=0}; export SPDK_TEST_VHOST_INIT
: ${SPDK_TEST_PMDK=0}; export SPDK_TEST_PMDK
: ${SPDK_TEST_LVOL=0}; export SPDK_TEST_LVOL
: ${SPDK_TEST_JSON=0}; export SPDK_TEST_JSON
: ${SPDK_TEST_REDUCE=0}; export SPDK_TEST_REDUCE
: ${SPDK_RUN_ASAN=0}; export SPDK_RUN_ASAN
: ${SPDK_RUN_UBSAN=0}; export SPDK_RUN_UBSAN
: ${SPDK_RUN_INSTALLED_DPDK=0}; export SPDK_RUN_INSTALLED_DPDK
: ${SPDK_TEST_CRYPTO=0}; export SPDK_TEST_CRYPTO
: ${SPDK_TEST_FTL=0}; export SPDK_TEST_FTL
: ${SPDK_TEST_BDEV_FTL=0}; export SPDK_TEST_BDEV_FTL
: ${SPDK_TEST_OCF=0}; export SPDK_TEST_OCF
: ${SPDK_TEST_FTL_EXTENDED=0}; export SPDK_TEST_FTL_EXTENDED
: ${SPDK_AUTOTEST_X=true}; export SPDK_AUTOTEST_X

# Export PYTHONPATH with addition of RPC framework. New scripts can be created
# specific use cases for tests.
export PYTHONPATH=$PYTHONPATH:$rootdir/scripts

# Export flag to skip the known bug that exists in librados
# Bug is reported on ceph bug tracker with number 24078
export ASAN_OPTIONS=new_delete_type_mismatch=0
export UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1:abort_on_error=1'

# Export LeakSanitizer option to use suppression file in order to prevent false positives
# and known leaks in external executables or libraries from showing up.
asan_suppression_file="/var/tmp/asan_suppression_file"
sudo rm -rf "$asan_suppression_file"

# ASAN has some bugs around thread_local variables.  We have a destructor in place
# to free the thread contexts, but ASAN complains about the leak before those
# destructors have a chance to run.  So suppress this one specific leak using
# LSAN_OPTIONS.
echo "leak:spdk_fs_alloc_thread_ctx" >> "$asan_suppression_file"

# Suppress known leaks in fio project
echo "leak:/usr/src/fio/parse.c" >> "$asan_suppression_file"
echo "leak:/usr/src/fio/iolog.c" >> "$asan_suppression_file"
echo "leak:/usr/src/fio/init.c" >> "$asan_suppression_file"

# Suppress leaks in libiscsi
echo "leak:libiscsi.so" >> "$asan_suppression_file"

export LSAN_OPTIONS=suppressions="$asan_suppression_file"

export DEFAULT_RPC_ADDR="/var/tmp/spdk.sock"

if [ -z "$DEPENDENCY_DIR" ]; then
	export DEPENDENCY_DIR=/home/sys_sgsw
else
	export DEPENDENCY_DIR
fi

# pass our valgrind desire on to unittest.sh
if [ $SPDK_RUN_VALGRIND -eq 0 ]; then
	export valgrind=''
fi

if [ "$(uname -s)" = "Linux" ]; then
	MAKE=make
	MAKEFLAGS=${MAKEFLAGS:--j$(nproc)}
	DPDK_LINUX_DIR=/usr/share/dpdk/x86_64-default-linuxapp-gcc
	if [ -d $DPDK_LINUX_DIR ] && [ $SPDK_RUN_INSTALLED_DPDK -eq 1 ]; then
		WITH_DPDK_DIR=$DPDK_LINUX_DIR
	fi
	# Override the default HUGEMEM in scripts/setup.sh to allocate 8GB in hugepages.
	export HUGEMEM=8192
elif [ "$(uname -s)" = "FreeBSD" ]; then
	MAKE=gmake
	MAKEFLAGS=${MAKEFLAGS:--j$(sysctl -a | egrep -i 'hw.ncpu' | awk '{print $2}')}
	DPDK_FREEBSD_DIR=/usr/local/share/dpdk/x86_64-native-bsdapp-clang
	if [ -d $DPDK_FREEBSD_DIR ] && [ $SPDK_RUN_INSTALLED_DPDK -eq 1 ]; then
		WITH_DPDK_DIR=$DPDK_FREEBSD_DIR
	fi
	# FreeBSD runs a much more limited set of tests, so keep the default 2GB.
	export HUGEMEM=2048
else
	echo "Unknown OS \"$(uname -s)\""
	exit 1
fi

config_params='--enable-debug --enable-werror'

if echo -e "#include <libunwind.h>\nint main(int argc, char *argv[]) {return 0;}\n" | \
	gcc -o /dev/null -lunwind -x c - 2>/dev/null; then
	config_params+=' --enable-log-bt'
fi

# for options with dependencies but no test flag, set them here
if [ -f /usr/include/infiniband/verbs.h ]; then
	config_params+=' --with-rdma'
fi

if [ -d /usr/src/fio ]; then
	config_params+=' --with-fio=/usr/src/fio'
fi

if [ -d ${DEPENDENCY_DIR}/vtune_codes ]; then
	config_params+=' --with-vtune='${DEPENDENCY_DIR}'/vtune_codes'
fi

if [ -d /usr/include/iscsi ]; then
	libiscsi_version=$(grep LIBISCSI_API_VERSION /usr/include/iscsi/iscsi.h | head -1 | awk '{print $3}' | awk -F '(' '{print $2}' | awk -F ')' '{print $1}')
	if [ $libiscsi_version -ge 20150621 ]; then
		config_params+=' --with-iscsi-initiator'
	fi
fi

# for options with both dependencies and a test flag, set them here
if [ -f /usr/include/libpmemblk.h ] && [ $SPDK_TEST_PMDK -eq 1 ]; then
	config_params+=' --with-pmdk'
fi

if [ -f /usr/include/libpmem.h ] && [ $SPDK_TEST_REDUCE -eq 1 ]; then
	if [ $SPDK_TEST_ISAL -eq 1 ]; then
		config_params+=' --with-reduce'
	else
		echo "reduce not enabled because isal is not enabled."
	fi
fi

if [ -d /usr/include/rbd ] &&  [ -d /usr/include/rados ] && [ $SPDK_TEST_RBD -eq 1 ]; then
	if [ $SPDK_TEST_ISAL -eq 0 ]; then
		config_params+=' --with-rbd'
	else
		echo "rbd not enabled because isal is enabled."
	fi
fi

# for options with no required dependencies, just test flags, set them here
if [ $SPDK_TEST_CRYPTO -eq 1 ]; then
	config_params+=' --with-crypto'
fi

if [ $SPDK_TEST_OCF -eq 1 ]; then
	config_params+=" --with-ocf"
fi

if [ $SPDK_RUN_UBSAN -eq 1 ]; then
	config_params+=' --enable-ubsan'
fi

if [ $SPDK_RUN_ASAN -eq 1 ]; then
	config_params+=' --enable-asan'
fi

if [ "$(uname -s)" = "Linux" ]; then
	config_params+=' --enable-coverage'
fi

if [ $SPDK_TEST_ISAL -eq 0 ]; then
	config_params+=' --without-isal'
fi

# By default, --with-dpdk is not set meaning the SPDK build will use the DPDK submodule.
# If a DPDK installation is found in a well-known location though, WITH_DPDK_DIR will be
# set which will override the default and use that DPDK installation instead.
if [ ! -z "$WITH_DPDK_DIR" ]; then
	config_params+=" --with-dpdk=$WITH_DPDK_DIR"
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

TEST_MODE=
for i in "$@"; do
	case "$i" in
		--iso)
			TEST_MODE=iso
			;;
		--transport=*)
			TEST_TRANSPORT="${i#*=}"
			;;
		--sock=*)
			TEST_SOCK="${i#*=}"
			;;
	esac
done

rpc_input_pipe="/tmp/rpc_input"
rpc_output_pipe="/tmp/rpc_output"

function rpc_cmd() {
	echo "$@" >&$rpc_input_fd
	exec {rpc_output_fd}<"$rpc_output_pipe"
	read -n1 rc <&$rpc_output_fd
	cat <&$rpc_output_fd
	eval "exec $rpc_output_fd>&-"
	[ "$rc" = "0" ]
}

# initialize rpc.py daemon if it's not up yet
if [ -z "$rpc_input_fd" ]; then
	xtrace_restore
	rm -f "$rpc_input_pipe"
	rm -f "$rpc_output_pipe"
	mkfifo "$rpc_input_pipe"

	$rootdir/scripts/rpc.py -i "$rpc_input_pipe" -o "$rpc_output_pipe" &
	rpc_pid=$!

	# make sure it didn't fail right away
	sleep 0.5
	kill -0 $rpc_pid

	# rpc.py will shut down automatically once this pipe is closed
	# (after this bash process exists)
	exec {rpc_input_fd}>"$rpc_input_pipe"
	export rpc_input_fd

	# since input pipe is readable already, make sure output pipe
	# is ready as well
	ls "$rpc_output_pipe"
	xtrace_disable
fi

function timing() {
	direction="$1"
	testname="$2"

	now=$(date +%s)

	if [ "$direction" = "enter" ]; then
		export timing_stack="${timing_stack};${now}"
		export test_stack="${test_stack};${testname}"
	else
		touch "$output_dir/timing.txt"
		child_time=$(grep "^${test_stack:1};" $output_dir/timing.txt | awk '{s+=$2} END {print s}')

		start_time=$(echo "$timing_stack" | sed -e 's@^.*;@@')
		timing_stack=$(echo "$timing_stack" | sed -e 's@;[^;]*$@@')

		elapsed=$((now - start_time - child_time))
		echo "${test_stack:1} $elapsed" >> $output_dir/timing.txt

		test_stack=$(echo "$test_stack" | sed -e 's@;[^;]*$@@')
	fi
}

function timing_enter() {
	xtrace_disable
	timing "enter" "$1"
	xtrace_restore
}

function timing_exit() {
	xtrace_disable
	timing "exit" "$1"
	xtrace_restore
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
	grep -rshI --exclude="autotest_common.sh" --exclude="$rootdir/test/common/autotest_common.sh" -e "report_test_completion" $rootdir | sed 's/report_test_completion//g; s/[[:blank:]]//g; s/"//g;' > $output_dir/all_tests.txt || true
}

function report_test_completion() {
	echo "$1" >> $output_dir/test_completions.txt
}

function process_core() {
	ret=0
	for core in $(find . -type f \( -name 'core\.?[0-9]*' -o -name '*.core' \)); do
		exe=$(eu-readelf -n "$core" | grep psargs | sed "s/.*psargs: \([^ \'\" ]*\).*/\1/")
		if [[ ! -f "$exe" ]]; then
			exe=$(eu-readelf -n "$core" | grep -oP -m1 "$exe.+")
		fi
		echo "exe for $core is $exe"
		if [[ ! -z "$exe" ]]; then
			if hash gdb &>/dev/null; then
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

	local rpc_addr="${2:-$DEFAULT_RPC_ADDR}"

	echo "Waiting for process to start up and listen on UNIX domain socket $rpc_addr..."
	# turn off trace for this loop
	xtrace_disable
	local ret=0
	local i
	for (( i = 40; i != 0; i-- )); do
		# if the process is no longer running, then exit the script
		#  since it means the application crashed
		if ! kill -s 0 $1; then
			echo "ERROR: process (pid: $1) is no longer running"
			ret=1
			break
		fi

		if $rootdir/scripts/rpc.py -t 1 -s "$rpc_addr" rpc_get_methods &>/dev/null; then
			break
		fi

		sleep 0.5
	done

	xtrace_restore
	if (( i == 0 )); then
		echo "ERROR: timeout while waiting for process (pid: $1) to start listening on '$rpc_addr'"
		ret=1
	fi
	return $ret
}

function waitfornbd() {
	local nbd_name=$1
	local i

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
		size=$(stat -c %s /tmp/nbdtest)
		rm -f /tmp/nbdtest
		if [ "$size" != "0" ]; then
			return 0
		else
			sleep 0.1
		fi
	done

	return 1
}

function waitforbdev() {
	local bdev_name=$1
	local i

	for ((i=1; i<=20; i++)); do
		if ! $rpc_py get_bdevs | jq -r '.[] .name' | grep -qw $bdev_name; then
			sleep 0.1
		else
			return 0
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
		$NS_CMD $rootdir/scripts/ceph/stop.sh || true
		$NS_CMD $rootdir/scripts/ceph/start.sh $1

		$NS_CMD ceph osd pool create $RBD_POOL $PG_NUM || true
		$NS_CMD rbd create $RBD_NAME --size 1000
	fi
}

function rbd_cleanup() {
	if hash ceph; then
		$rootdir/scripts/ceph/stop.sh || true
		rm -f /var/tmp/ceph_raw.img
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
	echo done.
}

function kill_stub() {
	kill $1 $stubpid
	wait $stubpid
	rm -f /var/run/spdk_stub0
	# Re-enable ASLR now that we are done with multi-process testing
	# Note: "1" enables ASLR w/o randomizing data segments, "2" adds data segment
	#  randomizing and is the default on all recent Linux kernels
	echo 2 > /proc/sys/kernel/randomize_va_space
}

function run_test() {
	xtrace_disable
	local test_type="$(echo $1 | tr 'a-z' 'A-Z')"
	shift
	echo "************************************"
	echo "START TEST $test_type $@"
	echo "************************************"
	xtrace_restore
	time "$@"
	xtrace_disable
	echo "************************************"
	echo "END TEST $test_type $@"
	echo "************************************"
	xtrace_restore
}

function print_backtrace() {
	# if errexit is not enabled, don't print a backtrace
	[[ "$-" =~ e ]] || return 0

	xtrace_disable
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
	xtrace_restore
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
		nbd_start_disks "$rpc_server" $devname $nbd_path

		waitfornbd ${nbd_path:5}

		if [ "$operation" = create ]; then
			parted -s $nbd_path mklabel gpt mkpart first '0%' '50%' mkpart second '50%' '100%'

			# change the GUID to SPDK GUID value
			SPDK_GPT_GUID=$(grep SPDK_GPT_PART_TYPE_GUID $rootdir/lib/bdev/gpt/gpt.h \
				| awk -F "(" '{ print $2}' | sed 's/)//g' \
				| awk -F ", " '{ print $1 "-" $2 "-" $3 "-" $4 "-" $5}' | sed 's/0x//g')
			sgdisk -t 1:$SPDK_GPT_GUID $nbd_path
			sgdisk -t 2:$SPDK_GPT_GUID $nbd_path
		elif [ "$operation" = reset ]; then
			# clear the partition table
			dd if=/dev/zero of=$nbd_path bs=4096 count=8 oflag=direct
		fi

		nbd_stop_disks "$rpc_server" $nbd_path

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
	$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 \
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

function waitforblk_disconnect()
{
	local i=0
	while lsblk -l -o NAME | grep -q -w $1; do
		[ $i -lt 15 ] || break
		i=$[$i+1]
		sleep 1
	done

	if lsblk -l -o NAME | grep -q -w $1; then
		return 1
	fi

	return 0
}

function waitforfile()
{
	local i=0
	while [ ! -f $1 ]; do
		[ $i -lt 200 ] || break
		i=$[$i+1]
		sleep 0.1
	done

	if [ ! -f $1 ]; then
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

function fio_bdev()
{
	# Setup fio binary cmd line
	local fio_dir="/usr/src/fio"
	local bdev_plugin="$rootdir/examples/bdev/fio_plugin/fio_plugin"

	# Preload AddressSanitizer library to fio if fio_plugin was compiled with it
	local asan_lib=$(ldd $bdev_plugin | grep libasan | awk '{print $3}')

	LD_PRELOAD=""$asan_lib" "$bdev_plugin"" "$fio_dir"/fio "$@"
}

function fio_nvme()
{
	# Setup fio binary cmd line
	local fio_dir="/usr/src/fio"
	local nvme_plugin="$rootdir/examples/nvme/fio_plugin/fio_plugin"

	# Preload AddressSanitizer library to fio if fio_plugin was compiled with it
	asan_lib=$(ldd $nvme_plugin | grep libasan | awk '{print $3}')

	LD_PRELOAD=""$asan_lib" "$nvme_plugin"" "$fio_dir"/fio "$@"
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
	rm -rf "$asan_suppression_file"
}

function freebsd_update_contigmem_mod()
{
	if [ $(uname) = FreeBSD ]; then
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

PS4=' \t	\$ '
if $SPDK_AUTOTEST_X; then
	# explicitly enable xtraces
	set -x
	xtrace_enable
else
	xtrace_restore
fi
