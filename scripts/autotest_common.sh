set -xe
ulimit -c unlimited

export RUN_NIGHTLY=0

if [[ ! -z $1 ]]; then
	if [ -f $1 ]; then
		source $1
	fi
fi

# Set defaults for missing test config options
: ${SPDK_BUILD_DOC=1}; export SPDK_BUILD_DOC
: ${SPDK_TEST_ISCSI=1}; export SPDK_TEST_ISCSI
: ${SPDK_TEST_NVME=1}; export SPDK_TEST_NVME
: ${SPDK_TEST_NVMF=1}; export SPDK_TEST_NVMF
: ${SPDK_TEST_VHOST=1}; export SPDK_TEST_VHOST
: ${SPDK_TEST_BLOCKDEV=1}; export SPDK_TEST_BLOCKDEV
: ${SPDK_TEST_IOAT=1}; export SPDK_TEST_IOAT
: ${SPDK_TEST_EVENT=1}; export SPDK_TEST_EVENT
: ${SPDK_TEST_BLOBFS=1}; export SPDK_TEST_BLOBFS

config_params='--enable-debug --enable-werror'

export UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1:abort_on_error=1'

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
		config_params+=' --enable-ubsan'
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

if hash valgrind &> /dev/null; then
	valgrind='valgrind --leak-check=full --error-exitcode=2'
else
	valgrind=''
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
				gdb -batch -ex "bt full" $exe $core
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
	export CEPH_DIR=/home/sys_sgsw/ceph/build

	if [ -d $CEPH_DIR ]; then
		export RBD_POOL=rbd
		export RBD_NAME=foo
		(cd $CEPH_DIR && ../src/vstart.sh -d -n -x -l)
		/usr/local/bin/rbd create $RBD_NAME --size 1000
	fi
}

function rbd_cleanup() {
	if [ -d $CEPH_DIR ]; then
		(cd $CEPH_DIR && ../src/stop.sh || true)
	fi
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

set -o errtrace
trap "trap - ERR; print_backtrace >&2" ERR
