set -xe
PS4=' \t	\$ '
ulimit -c unlimited

export RUN_NIGHTLY=0

if [[ ! -z $1 ]]; then
	if [ -f $1 ]; then
		source $1
	fi
fi

# Set defaults for missing test config options
: ${SPDK_BUILD_DOC=1}; export SPDK_BUILD_DOC
: ${SPDK_BUILD_IOAT_KMOD=1}; export SPDK_BUILD_IOAT_KMOD
: ${SPDK_RUN_CHECK_FORMAT=1}; export SPDK_RUN_CHECK_FORMAT
: ${SPDK_RUN_SCANBUILD=1}; export SPDK_RUN_SCANBUILD
: ${SPDK_RUN_VALGRIND=1}; export SPDK_RUN_VALGRIND
: ${SPDK_TEST_UNITTEST=1}; export SPDK_TEST_UNITTEST
: ${SPDK_TEST_ISCSI=1}; export SPDK_TEST_ISCSI
: ${SPDK_TEST_NVME=1}; export SPDK_TEST_NVME
: ${SPDK_TEST_NVMF=1}; export SPDK_TEST_NVMF
: ${SPDK_TEST_RBD=1}; export SPDK_TEST_RBD
: ${SPDK_TEST_VHOST=1}; export SPDK_TEST_VHOST
: ${SPDK_TEST_BLOCKDEV=1}; export SPDK_TEST_BLOCKDEV
: ${SPDK_TEST_IOAT=1}; export SPDK_TEST_IOAT
: ${SPDK_TEST_EVENT=1}; export SPDK_TEST_EVENT
: ${SPDK_TEST_BLOBFS=1}; export SPDK_TEST_BLOBFS
: ${SPDK_RUN_ASAN=1}; export SPDK_RUN_ASAN
: ${SPDK_RUN_UBSAN=1}; export SPDK_RUN_UBSAN

# pass our valgrind desire on to unittest.sh
if [ $SPDK_RUN_VALGRIND -eq 0 ]; then
	export valgrind=''
fi

config_params='--enable-debug --enable-werror'

export UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1:abort_on_error=1'

export SPDK_GPT_GUID=`grep SPDK_GPT_PART_TYPE_GUID $rootdir/lib/bdev/gpt/gpt.h \
			| awk -F "(" '{ print $2}' | sed 's/)//g' \
			| awk -F ", " '{ print $1 "-" $2 "-" $3 "-" $4 "-" $5}' | sed 's/0x//g'`

# Override the default NRHUGE in scripts/setup.sh
export NRHUGE=4096

case `uname` in
	FreeBSD)
		DPDK_FREEBSD_DIR=/usr/local/share/dpdk/x86_64-native-bsdapp-clang
		if [ -d $DPDK_FREEBSD_DIR ]; then
			WITH_DPDK_DIR=$DPDK_FREEBSD_DIR
		fi
		MAKE=gmake
		MAKEFLAGS=${MAKEFLAGS:--j$(sysctl -a | egrep -i 'hw.ncpu' | awk '{print $2}')}
		;;
	Linux)
		DPDK_LINUX_DIR=/usr/local/share/dpdk/x86_64-native-linuxapp-gcc
		if [ -d $DPDK_LINUX_DIR ]; then
			WITH_DPDK_DIR=$DPDK_LINUX_DIR
		fi
		MAKE=make
		MAKEFLAGS=${MAKEFLAGS:--j$(nproc)}
		config_params+=' --enable-coverage'
		if [ $SPDK_RUN_UBSAN -eq 1 ]; then
			config_params+=' --enable-ubsan'
		fi
		if [ $SPDK_RUN_ASAN -eq 1 ]; then
			if /usr/sbin/ldconfig -p | grep -q asan; then
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

if [ -d /usr/src/fio ]; then
	config_params+=' --with-fio=/usr/src/fio'
fi

if [ -d /usr/include/rbd ] &&  [ -d /usr/include/rados ]; then
	config_params+=' --with-rbd'
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

function process_core() {
	ret=0
	for core in $(find . -type f \( -name 'core*' -o -name '*.core' \)); do
		exe=$(eu-readelf -n "$core" | grep psargs | sed "s/.*psargs: \([^ \'\" ]*\).*/\1/")
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

function waitforlisten() {
	# $1 = process pid
	# $2 = TCP port number
	if [ -z "$1" ] || [ -z "$2" ]; then
		exit 1
	fi

	echo "Waiting for process to start up and listen on TCP port $2..."
	# turn off trace for this loop
	set +x
	ret=1
	while [ $ret -ne 0 ]; do
		# if the process is no longer running, then exit the script
		#  since it means the application crashed
		if ! kill -s 0 $1; then
			exit
		fi
		if netstat -an --tcp | grep -iw listen | grep -q $2; then
			ret=0
		fi
	done
	set -x
}

function waitforbdev() {
	bdev_name=$1
	rpc_py=$2

	for ((i=1; i<=10; i++)); do
		if $rpc_py get_bdevs -b "$bdev_name" &>/dev/null; then
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

	echo "killing process with pid $1"
	kill $1
	wait $1
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
	if hash ceph; then
		export RBD_POOL=rbd
		export RBD_NAME=foo
		(cd $rootdir/scripts/ceph && ./start.sh)
		rbd create $RBD_NAME --size 1000
	fi
}

function rbd_cleanup() {
	if hash ceph; then
		(cd $rootdir/scripts/ceph && ./stop.sh || true)
	fi
}

function start_stub() {
	$rootdir/test/app/stub/stub $1 &
	stubpid=$!
	echo Waiting for stub to ready for secondary processes...
	while ! [ -e /var/run/spdk_stub0 ]; do
		sleep 1s
	done
	echo done.
}

function kill_stub() {
	kill $stubpid
	wait $stubpid
	rm -f /var/run/spdk_stub0
}

function run_test() {
	set +x
	echo "************************************"
	echo "START TEST $1"
	echo "************************************"
	set -x
	time "$@"
	set +x
	echo "************************************"
	echo "END TEST $1"
	echo "************************************"
	set -x
}

function print_backtrace() {
	set +x
	echo "========== Backtrace start: =========="
	echo ""
	for i in $(seq 1 $((${#FUNCNAME[@]} - 1))); do
		local func="${FUNCNAME[$i]}"
		local line_nr="${BASH_LINENO[$((i - 1))]}"
		local src="${BASH_SOURCE[$i]/#$rootdir/.}"
		echo "in $src:$line_nr -> $func()"
		echo "     ..."
		nl -w 4 -ba -nln $src | grep -B 5 -A 5 "^$line_nr" | \
			sed "s/^/   /g" | sed "s/^   $line_nr /=> $line_nr /g"
		echo "     ..."
	done
	echo ""
	echo "========== Backtrace end =========="
	set -x
	return 0
}

function part_dev_by_gpt () {
	if [ $(uname -s) = Linux ] && hash sgdisk; then
		conf=$1
		devname=$2
		rootdir=$3
		operation=$4

		if [ ! -e $conf ]; then
			return 1
		fi

		if [ -z "$operation" ]; then
			operation="create"
		fi

		cp $conf ${conf}.gpt
		echo "[Gpt]" >> ${conf}.gpt
		echo "  Disable Yes" >> ${conf}.gpt

		modprobe nbd
		$rootdir/test/lib/bdev/nbd/nbd -c ${conf}.gpt -b $devname -n /dev/nbd0 &
		nbd_pid=$!
		echo "Process nbd pid: $nbd_pid"
		waitforlisten $nbd_pid 5260
		waitforbdev $devname "python $rootdir/scripts/rpc.py"

		if [ -e /dev/nbd0 ]; then
			if [ "$operation" = create ]; then
				parted -s /dev/nbd0 mklabel gpt mkpart first '0%' '50%' mkpart second '50%' '100%'
				# change the GUID to SPDK GUID value
				sgdisk -t 1:$SPDK_GPT_GUID /dev/nbd0
				sgdisk -t 2:$SPDK_GPT_GUID /dev/nbd0
			elif [ "$operation" = reset ]; then
				# clear the partition table
				dd if=/dev/zero of=/dev/nbd0 bs=4096 count=8 oflag=direct
			fi
		fi
		killprocess $nbd_pid
		rm -f ${conf}.gpt
	fi

	return 0
}

function discover_bdevs()
{
	local rootdir=$1
	local config_file=$2

	if [ ! -e $config_file ]; then
		echo "Invalid Configuration File: $config_file"
		return -1
	fi

	# Start the bdev service to query for the list of available
	# bdevs.
	$rootdir/test/app/bdev_svc/bdev_svc -i 0 -c $config_file &>/dev/null &
	stubpid=$!
	while ! [ -e /var/run/spdk_bdev0 ]; do
		sleep 1
	done

	# Get all of the bdevs
	$rootdir/scripts/rpc.py get_bdevs

	# Shut down the bdev service
	kill $stubpid
	wait $stubpid
	rm -f /var/run/spdk_bdev0
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

set -o errtrace
trap "trap - ERR; print_backtrace >&2" ERR
