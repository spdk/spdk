set -xe
ulimit -c unlimited

export RUN_NIGHTLY=0

MAKECONFIG='CONFIG_DEBUG=y CONFIG_WERROR=y'

export UBSAN_OPTIONS=halt_on_error=1

case `uname` in
	FreeBSD)
		DPDK_DIR=/usr/local/share/dpdk/x86_64-native-bsdapp-clang
		MAKE=gmake
		MAKEFLAGS=${MAKEFLAGS:--j$(sysctl -a | egrep -i 'hw.ncpu' | awk '{print $2}')}
		;;
	Linux)
		DPDK_DIR=/usr/local/share/dpdk/x86_64-native-linuxapp-gcc
		MAKE=make
		MAKEFLAGS=${MAKEFLAGS:--j$(nproc)}
		MAKECONFIG="$MAKECONFIG CONFIG_COVERAGE=y"
		MAKECONFIG="$MAKECONFIG CONFIG_UBSAN=y"
		;;
	*)
		echo "Unknown OS in $0"
		exit 1
		;;
esac

if [ -f /usr/include/infiniband/verbs.h ]; then
	MAKECONFIG="$MAKECONFIG CONFIG_RDMA=y"
fi

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
	for core in $(find . -type f -name 'core*'); do
		exe=$(eu-readelf -n "$core" | grep psargs | sed "s/.*psargs: \([^ \'\" ]*\).*/\1/")
		echo "exe for $core is $exe"
		if [[ ! -z "$exe" ]]; then
			if hash gdb; then
				gdb -batch -ex "bt" $exe $core
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
