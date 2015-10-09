set -xe
ulimit -c unlimited

case `uname` in
	FreeBSD)
		DPDK_DIR=/usr/local/share/dpdk/x86_64-native-bsdapp-clang
		MAKE=gmake
		;;
	Linux)
		DPDK_DIR=/usr/local/dpdk-2.1.0/x86_64-native-linuxapp-gcc
		MAKE=make
		;;
	*)
		echo "Unknown OS in $0"
		exit 1
		;;
esac

MAKEFLAGS=${MAKEFLAGS:--j16}

if [ -z "$output_dir" ]; then
	if [ -z "$rootdir" ] || [ ! -d "$rootdir/../output" ]; then
		output_dir=.
	else
		output_dir=$rootdir/../output
	fi
	export output_dir
fi

if hash valgrind &> /dev/null; then
	# TODO: add --error-exitcode=2 when all Valgrind warnings are fixed
	valgrind='valgrind --leak-check=full'
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
	timing "enter" "$1"
}

function timing_exit() {
	timing "exit" "$1"
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
		exe=$(eu-readelf -n "$core" | grep psargs | awk '{ print $2 }')
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

