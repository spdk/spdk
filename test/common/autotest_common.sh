#!/usr/bin/env bash

function xtrace_fd() {
	if [[ -n $BASH_XTRACEFD && -e /proc/self/fd/$BASH_XTRACEFD ]]; then
		# Close it first to make sure it's sane
		exec {BASH_XTRACEFD}>&-
	fi
	exec {BASH_XTRACEFD}>&2

	set -x
}

function xtrace_disable() {
	if [ "$XTRACE_DISABLED" != "yes" ]; then
		PREV_BASH_OPTS="$-"
		if [[ "$PREV_BASH_OPTS" == *"x"* ]]; then
			XTRACE_DISABLED="yes"
		fi
		set +x
	elif [ -z $XTRACE_NESTING_LEVEL ]; then
		XTRACE_NESTING_LEVEL=1
	else
		XTRACE_NESTING_LEVEL=$((++XTRACE_NESTING_LEVEL))
	fi
}

xtrace_disable
set -e
shopt -s expand_aliases

if [[ -e $rootdir/test/common/build_config.sh ]]; then
	source "$rootdir/test/common/build_config.sh"
elif [[ -e $rootdir/mk/config.mk ]]; then
	build_config=$(< "$rootdir/mk/config.mk")
	source <(echo "${build_config//\?=/=}")
else
	source "$rootdir/CONFIG"
fi

# Source scripts after the config so that the definitions are available.
source "$rootdir/test/common/applications.sh"
source "$rootdir/scripts/common.sh"

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
alias xtrace_restore='if [ -z $XTRACE_NESTING_LEVEL ]; then
        if [[ "$PREV_BASH_OPTS" == *"x"* ]]; then
		XTRACE_DISABLED="no"; PREV_BASH_OPTS=""; set -x; xtrace_enable;
	fi
else
	XTRACE_NESTING_LEVEL=$((--XTRACE_NESTING_LEVEL));
	if [ $XTRACE_NESTING_LEVEL -eq "0" ]; then
		unset XTRACE_NESTING_LEVEL
	fi
fi'

: ${RUN_NIGHTLY:=0}
export RUN_NIGHTLY

# Set defaults for missing test config options
: ${SPDK_AUTOTEST_DEBUG_APPS:=0}
export SPDK_AUTOTEST_DEBUG_APPS
: ${SPDK_RUN_VALGRIND=0}
export SPDK_RUN_VALGRIND
: ${SPDK_RUN_FUNCTIONAL_TEST=0}
export SPDK_RUN_FUNCTIONAL_TEST
: ${SPDK_TEST_UNITTEST=0}
export SPDK_TEST_UNITTEST
: ${SPDK_TEST_AUTOBUILD=0}
export SPDK_TEST_AUTOBUILD
: ${SPDK_TEST_RELEASE_BUILD=0}
export SPDK_TEST_RELEASE_BUILD
: ${SPDK_TEST_ISAL=0}
export SPDK_TEST_ISAL
: ${SPDK_TEST_ISCSI=0}
export SPDK_TEST_ISCSI
: ${SPDK_TEST_ISCSI_INITIATOR=0}
export SPDK_TEST_ISCSI_INITIATOR
: ${SPDK_TEST_NVME=0}
export SPDK_TEST_NVME
: ${SPDK_TEST_NVME_PMR=0}
export SPDK_TEST_NVME_PMR
: ${SPDK_TEST_NVME_SCC=0}
export SPDK_TEST_NVME_SCC
: ${SPDK_TEST_NVME_BP=0}
export SPDK_TEST_NVME_BP
: ${SPDK_TEST_NVME_CLI=0}
export SPDK_TEST_NVME_CLI
: ${SPDK_TEST_NVME_CUSE=0}
export SPDK_TEST_NVME_CUSE
: ${SPDK_TEST_NVMF=0}
export SPDK_TEST_NVMF
: ${SPDK_TEST_VFIOUSER=0}
export SPDK_TEST_VFIOUSER
: ${SPDK_TEST_VFIOUSER_QEMU=0}
export SPDK_TEST_VFIOUSER_QEMU
: ${SPDK_TEST_NVMF_TRANSPORT="rdma"}
export SPDK_TEST_NVMF_TRANSPORT
: ${SPDK_TEST_RBD=0}
export SPDK_TEST_RBD
: ${SPDK_TEST_VHOST=0}
export SPDK_TEST_VHOST
: ${SPDK_TEST_BLOCKDEV=0}
export SPDK_TEST_BLOCKDEV
: ${SPDK_TEST_IOAT=0}
export SPDK_TEST_IOAT
: ${SPDK_TEST_BLOBFS=0}
export SPDK_TEST_BLOBFS
: ${SPDK_TEST_VHOST_INIT=0}
export SPDK_TEST_VHOST_INIT
: ${SPDK_TEST_PMDK=0}
export SPDK_TEST_PMDK
: ${SPDK_TEST_LVOL=0}
export SPDK_TEST_LVOL
: ${SPDK_TEST_REDUCE=0}
export SPDK_TEST_REDUCE
: ${SPDK_RUN_ASAN=0}
export SPDK_RUN_ASAN
: ${SPDK_RUN_UBSAN=0}
export SPDK_RUN_UBSAN
: ${SPDK_RUN_EXTERNAL_DPDK=""}
export SPDK_RUN_EXTERNAL_DPDK
: ${SPDK_RUN_NON_ROOT=0}
export SPDK_RUN_NON_ROOT
: ${SPDK_TEST_CRYPTO=0}
export SPDK_TEST_CRYPTO
: ${SPDK_TEST_FTL=0}
export SPDK_TEST_FTL
: ${SPDK_TEST_OCF=0}
export SPDK_TEST_OCF
: ${SPDK_TEST_VMD=0}
export SPDK_TEST_VMD
: ${SPDK_TEST_OPAL=0}
export SPDK_TEST_OPAL
: ${SPDK_TEST_NATIVE_DPDK}
export SPDK_TEST_NATIVE_DPDK
: ${SPDK_AUTOTEST_X=true}
export SPDK_AUTOTEST_X
: ${SPDK_TEST_RAID5=0}
export SPDK_TEST_RAID5
: ${SPDK_TEST_URING=0}
export SPDK_TEST_URING
: ${SPDK_TEST_USE_IGB_UIO:=0}
export SPDK_TEST_USE_IGB_UIO
: ${SPDK_TEST_SCHEDULER:=0}
export SPDK_TEST_SCHEDULER
: ${SPDK_TEST_SCANBUILD:=0}
export SPDK_TEST_SCANBUILD

export DPDK_LIB_DIR="${SPDK_RUN_EXTERNAL_DPDK:-$rootdir/dpdk/build}/lib"
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$SPDK_LIB_DIR:$DPDK_LIB_DIR

# Tell setup.sh to wait for block devices upon each reset
export PCI_BLOCK_SYNC_ON_RESET=yes

# Export PYTHONPATH with addition of RPC framework. New scripts can be created
# specific use cases for tests.
export PYTHONPATH=$PYTHONPATH:$rootdir/scripts

# Don't create Python .pyc files. When running with sudo these will be
# created with root ownership and can cause problems when cleaning the repository.
export PYTHONDONTWRITEBYTECODE=1

# Export flag to skip the known bug that exists in librados
# Bug is reported on ceph bug tracker with number 24078
export ASAN_OPTIONS=new_delete_type_mismatch=0:disable_coredump=0
export UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1:abort_on_error=1:disable_coredump=0'

# Export LeakSanitizer option to use suppression file in order to prevent false positives
# and known leaks in external executables or libraries from showing up.
asan_suppression_file="/var/tmp/asan_suppression_file"
sudo rm -rf "$asan_suppression_file"
cat << EOL >> "$asan_suppression_file"
# ASAN has some bugs around thread_local variables.  We have a destructor in place
# to free the thread contexts, but ASAN complains about the leak before those
# destructors have a chance to run.  So suppress this one specific leak using
# LSAN_OPTIONS.
leak:spdk_fs_alloc_thread_ctx

# Suppress known leaks in fio project
leak:$CONFIG_FIO_SOURCE_DIR/parse.c
leak:$CONFIG_FIO_SOURCE_DIR/iolog.c
leak:$CONFIG_FIO_SOURCE_DIR/init.c
leak:$CONFIG_FIO_SOURCE_DIR/filesetup.c
leak:fio_memalign
leak:spdk_fio_io_u_init
# Suppress leaks in gperftools-libs from fio
leak:libtcmalloc_minimal.so

# Suppress leaks in libiscsi
leak:libiscsi.so
EOL

# Suppress leaks in libfuse3
echo "leak:libfuse3.so" >> "$asan_suppression_file"

export LSAN_OPTIONS=suppressions="$asan_suppression_file"

export DEFAULT_RPC_ADDR="/var/tmp/spdk.sock"

if [ -z "$DEPENDENCY_DIR" ]; then
	export DEPENDENCY_DIR=$HOME/spdk_dependencies
else
	export DEPENDENCY_DIR
fi

# Export location of where all the SPDK binaries are
export SPDK_BIN_DIR="$rootdir/build/bin"
export SPDK_EXAMPLE_DIR="$rootdir/build/examples"

# for vhost, vfio-user tests
export QEMU_BIN=${QEMU_BIN:-}
export VFIO_QEMU_BIN=${VFIO_QEMU_BIN:-}

# pass our valgrind desire on to unittest.sh
if [ $SPDK_RUN_VALGRIND -eq 0 ]; then
	export valgrind=''
fi

if [ "$(uname -s)" = "Linux" ]; then
	HUGEMEM=${HUGEMEM:-4096}
	export CLEAR_HUGE=yes
	if [[ $SPDK_TEST_CRYPTO -eq 1 || $SPDK_TEST_REDUCE -eq 1 ]]; then
		# Make sure that memory is distributed across all NUMA nodes - by default, all goes to
		# node0, but if QAT devices are attached to a different node, all of their VFs will end
		# up under that node too and memory needs to be available there for the tests.
		export HUGE_EVEN_ALLOC=yes
	fi

	MAKE="make"
	MAKEFLAGS=${MAKEFLAGS:--j$(nproc)}
	if [[ $SPDK_TEST_USE_IGB_UIO -eq 1 ]]; then
		export DRIVER_OVERRIDE=igb_uio
		# Building kernel modules requires root privileges
		MAKE="sudo $MAKE"
	fi
elif [ "$(uname -s)" = "FreeBSD" ]; then
	MAKE="gmake"
	MAKEFLAGS=${MAKEFLAGS:--j$(sysctl -a | grep -E -i 'hw.ncpu' | awk '{print $2}')}
	# FreeBSD runs a much more limited set of tests, so keep the default 2GB.
	HUGEMEM=${HUGEMEM:-2048}
elif [ "$(uname -s)" = "Windows" ]; then
	MAKE="make"
	MAKEFLAGS=${MAKEFLAGS:--j$(nproc)}
	# Keep the default 2GB for Windows.
	HUGEMEM=${HUGEMEM:-2048}
else
	echo "Unknown OS \"$(uname -s)\""
	exit 1
fi

export HUGEMEM=$HUGEMEM

if [ -z "$output_dir" ]; then
	mkdir -p "$rootdir/../output"
	export output_dir="$rootdir/../output"
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

# start rpc.py coprocess if it's not started yet
if [[ -z $RPC_PIPE_PID ]] || ! kill -0 "$RPC_PIPE_PID" &> /dev/null; then
	# Include list to all known plugins we use in the tests
	PYTHONPATH+=":$rootdir/test/rpc_plugins"
	coproc RPC_PIPE { PYTHONPATH="$PYTHONPATH" "$rootdir/scripts/rpc.py" --server; }
	exec {RPC_PIPE_OUTPUT}<&${RPC_PIPE[0]} {RPC_PIPE_INPUT}>&${RPC_PIPE[1]}
	# all descriptors will automatically close together with this bash
	# process, this will make rpc.py stop reading and exit gracefully
fi

function set_test_storage() {
	[[ -v testdir ]] || return 0

	local requested_size=$1 # bytes
	local mount target_dir

	local -A mounts fss sizes avails uses
	local source fs size avail mount use

	local storage_fallback storage_candidates

	storage_fallback=$(mktemp -udt spdk.XXXXXX)
	storage_candidates=(
		"$testdir"
		"$storage_fallback/tests/${testdir##*/}"
		"$storage_fallback"
	)

	if [[ -n $ADD_TEST_STORAGE ]]; then
		# List of dirs|mounts separated by whitespaces
		storage_candidates+=($ADD_TEST_STORAGE)
	fi

	if [[ -n $DEDICATED_TEST_STORAGE ]]; then
		# Single, dedicated dir|mount
		storage_candidates=("$DEDICATED_TEST_STORAGE")
	fi

	mkdir -p "${storage_candidates[@]}"

	# add some headroom - 64M
	requested_size=$((requested_size + (64 << 20)))

	while read -r source fs size use avail _ mount; do
		mounts["$mount"]=$source fss["$mount"]=$fs
		avails["$mount"]=$((avail * 1024)) sizes["$mount"]=$((size * 1024))
		uses["$mount"]=$((use * 1024))
	done < <(df -T | grep -v Filesystem)

	printf '* Looking for test storage...\n' >&2

	local target_space new_size
	for target_dir in "${storage_candidates[@]}"; do
		# FreeBSD's df is lacking the --output arg
		# mount=$(df --output=target "$target_dir" | grep -v "Mounted on")
		mount=$(df "$target_dir" | awk '$1 !~ /Filesystem/{print $6}')

		target_space=${avails["$mount"]}
		if ((target_space == 0 || target_space < requested_size)); then
			continue
		fi
		if ((target_space >= requested_size)); then
			# For in-memory fs, and / make sure our requested size won't fill most of the space.
			if [[ ${fss["$mount"]} == tmpfs ]] || [[ ${fss["$mount"]} == ramfs ]] || [[ $mount == / ]]; then
				new_size=$((uses["$mount"] + requested_size))
				if ((new_size * 100 / sizes["$mount"] > 95)); then
					continue
				fi
			fi
		fi
		export SPDK_TEST_STORAGE=$target_dir
		printf '* Found test storage at %s\n' "$SPDK_TEST_STORAGE" >&2
		return 0
	done
	printf '* Test storage is not available\n'
	return 1
}

function get_config_params() {
	xtrace_disable
	config_params='--enable-debug --enable-werror'

	# for options with dependencies but no test flag, set them here
	if [ -f /usr/include/infiniband/verbs.h ]; then
		config_params+=' --with-rdma'
	fi

	if [ $(uname -s) == "FreeBSD" ]; then
		intel="hw.model: Intel"
		cpu_vendor=$(sysctl -a | grep hw.model | cut -c 1-15)
	else
		intel="GenuineIntel"
		cpu_vendor=$(grep -i 'vendor' /proc/cpuinfo --max-count=1)
	fi
	if [[ "$cpu_vendor" != *"$intel"* ]]; then
		config_params+=" --without-idxd"
	else
		config_params+=" --with-idxd"
	fi

	if [[ -d $CONFIG_FIO_SOURCE_DIR ]]; then
		config_params+=" --with-fio=$CONFIG_FIO_SOURCE_DIR"
	fi

	if [ -d ${DEPENDENCY_DIR}/vtune_codes ]; then
		config_params+=' --with-vtune='${DEPENDENCY_DIR}'/vtune_codes'
	fi

	if [ -d /usr/include/iscsi ]; then
		[[ $(< /usr/include/iscsi/iscsi.h) =~ "define LIBISCSI_API_VERSION ("([0-9]+)")" ]] \
			&& libiscsi_version=${BASH_REMATCH[1]}
		if ((libiscsi_version >= 20150621)); then
			config_params+=' --with-iscsi-initiator'
		fi
	fi

	if [[ $SPDK_TEST_UNITTEST -eq 0 && \
		$SPDK_TEST_SCANBUILD -eq 0 && \
		$SPDK_TEST_AUTOBUILD -eq 0 ]]; then
		config_params+=' --disable-unit-tests'
	fi

	if [ $SPDK_TEST_NVME_CUSE -eq 1 ]; then
		config_params+=' --with-nvme-cuse'
	fi

	# for options with both dependencies and a test flag, set them here
	if [ -f /usr/include/libpmemblk.h ] && [ $SPDK_TEST_PMDK -eq 1 ]; then
		config_params+=' --with-pmdk'
	fi

	if [ -f /usr/include/libpmem.h ] && [ $SPDK_TEST_REDUCE -eq 1 ]; then
		if ge "$(nasm --version | awk '{print $3}')" 2.14 && [[ $SPDK_TEST_ISAL -eq 1 ]]; then
			config_params+=' --with-reduce'
		fi
	fi

	if [ -d /usr/include/rbd ] && [ -d /usr/include/rados ] && [ $SPDK_TEST_RBD -eq 1 ]; then
		config_params+=' --with-rbd'
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

	if [ $SPDK_TEST_BLOBFS -eq 1 ]; then
		if [[ -d /usr/include/fuse3 ]] || [[ -d /usr/local/include/fuse3 ]]; then
			config_params+=' --with-fuse'
		fi
	fi

	if [ $SPDK_TEST_RAID5 -eq 1 ]; then
		config_params+=' --with-raid5'
	fi

	if [ $SPDK_TEST_VFIOUSER -eq 1 ] || [ $SPDK_TEST_VFIOUSER_QEMU -eq 1 ]; then
		config_params+=' --with-vfio-user'
	fi

	# Check whether liburing library header exists
	if [ -f /usr/include/liburing/io_uring.h ] && [ $SPDK_TEST_URING -eq 1 ]; then
		config_params+=' --with-uring'
	fi

	if [ -n "$SPDK_RUN_EXTERNAL_DPDK" ]; then
		config_params+=" --with-dpdk=$SPDK_RUN_EXTERNAL_DPDK"
	fi

	echo "$config_params"
	xtrace_restore
}

function rpc_cmd() {
	xtrace_disable
	local rsp rc=1
	local stdin cmd cmds_number=0 status_number=0 status

	if (($#)); then
		cmds_number=1
		echo "$@" >&$RPC_PIPE_INPUT
	elif [[ ! -t 0 ]]; then
		mapfile -t stdin <&0
		cmds_number=${#stdin[@]}
		printf '%s\n' "${stdin[@]}" >&$RPC_PIPE_INPUT
	else
		return 0
	fi

	while read -t 5 -ru $RPC_PIPE_OUTPUT rsp; do
		if [[ $rsp == "**STATUS="* ]]; then
			status[${rsp#*=}]=$rsp
			if ((++status_number == cmds_number)); then
				break
			fi
			continue
		fi
		echo "$rsp"
	done

	rc=${!status[*]}
	xtrace_restore
	[[ $rc == 0 ]]
}

function rpc_cmd_simple_data_json() {

	local elems="$1[@]" elem
	local -gA jq_out=()
	local jq val

	local lvs=(
		"uuid"
		"name"
		"base_bdev"
		"total_data_clusters"
		"free_clusters"
		"block_size"
		"cluster_size"
	)

	local bdev=(
		"name"
		"aliases[0]"
		"block_size"
		"num_blocks"
		"uuid"
		"product_name"
	)

	[[ -v $elems ]] || return 1

	for elem in "${!elems}"; do
		jq="${jq:+$jq,\"\\n\",}\"$elem\",\" \",.[0].$elem"
	done
	jq+=',"\n"'

	shift
	while read -r elem val; do
		jq_out["$elem"]=$val
	done < <(rpc_cmd "$@" | jq -jr "$jq")
	((${#jq_out[@]} > 0)) || return 1
}

function NOT() {
	local es=0

	"$@" || es=$?

	# Logic looks like so:
	#  - return false if command exit successfully
	#  - return false if command exit after receiving a core signal (FIXME: or any signal?)
	#  - return true if command exit with an error

	# This naively assumes that the process doesn't exit with > 128 on its own.
	if ((es > 128)); then
		es=$((es & ~128))
		case "$es" in
			3) ;&       # SIGQUIT
			4) ;&       # SIGILL
			6) ;&       # SIGABRT
			8) ;&       # SIGFPE
			9) ;&       # SIGKILL
			11) es=0 ;; # SIGSEGV
			*) es=1 ;;
		esac
	elif [[ -n $EXIT_STATUS ]] && ((es != EXIT_STATUS)); then
		es=0
	fi

	# invert error code of any command and also trigger ERR on 0 (unlike bash ! prefix)
	((!es == 0))
}

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
			> $output_dir/timing.svg
	fi
}

function create_test_list() {
	xtrace_disable
	# First search all scripts in main SPDK directory.
	completion=$(grep -shI -d skip --include="*.sh" -e "run_test " $rootdir/*)
	# Follow up with search in test directory recursively.
	completion+=$(grep -rshI --include="*.sh" --exclude="autotest_common.sh" -e "run_test " $rootdir/test)
	printf "%s" "$completion" | grep -v "#" \
		| sed 's/^.*run_test/run_test/' | awk '{print $2}' \
		| sed 's/\"//g' | sort > $output_dir/all_tests.txt || true
	xtrace_restore
}

function gdb_attach() {
	gdb -q --batch \
		-ex 'handle SIGHUP nostop pass' \
		-ex 'handle SIGQUIT nostop pass' \
		-ex 'handle SIGPIPE nostop pass' \
		-ex 'handle SIGALRM nostop pass' \
		-ex 'handle SIGTERM nostop pass' \
		-ex 'handle SIGUSR1 nostop pass' \
		-ex 'handle SIGUSR2 nostop pass' \
		-ex 'handle SIGCHLD nostop pass' \
		-ex 'set print thread-events off' \
		-ex 'cont' \
		-ex 'thread apply all bt' \
		-ex 'quit' \
		--tty=/dev/stdout \
		-p $1
}

function process_core() {
	# Note that this always was racy as we can't really sync with the kernel
	# to see if there's any core queued up for writing. We could check if
	# collector is running and wait for it explicitly, but it doesn't seem
	# to be worth the effort. So assume that if we are being called via
	# trap, as in, when some error has occurred, wait up to 10s for any
	# potential cores. If we are called just for cleanup at the very end,
	# don't wait since all the tests ended successfully, hence having any
	# critical cores lying around is unlikely.
	((autotest_es != 0)) && sleep 10

	local coredumps core

	shopt -s nullglob
	coredumps=("$output_dir/coredumps/"*.bt.txt)
	shopt -u nullglob

	((${#coredumps[@]} > 0)) || return 0
	chmod -R a+r "$output_dir/coredumps"

	for core in "${coredumps[@]}"; do
		cat <<- BT
			##### CORE BT ${core##*/} #####

			$(<"$core")

			--
		BT
	done
	return 1
}

function process_shm() {
	type=$1
	id=$2
	if [ "$type" = "--pid" ]; then
		id="pid${id}"
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
	for ((i = 40; i != 0; i--)); do
		# if the process is no longer running, then exit the script
		#  since it means the application crashed
		if ! kill -s 0 $1; then
			echo "ERROR: process (pid: $1) is no longer running"
			ret=1
			break
		fi

		if $rootdir/scripts/rpc.py -t 1 -s "$rpc_addr" rpc_get_methods &> /dev/null; then
			break
		fi

		sleep 0.5
	done

	xtrace_restore
	if ((i == 0)); then
		echo "ERROR: timeout while waiting for process (pid: $1) to start listening on '$rpc_addr'"
		ret=1
	fi
	return $ret
}

function waitfornbd() {
	local nbd_name=$1
	local i

	for ((i = 1; i <= 20; i++)); do
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
	for ((i = 1; i <= 20; i++)); do
		dd if=/dev/$nbd_name of="$SPDK_TEST_STORAGE/nbdtest" bs=4096 count=1 iflag=direct
		size=$(stat -c %s "$SPDK_TEST_STORAGE/nbdtest")
		rm -f "$SPDK_TEST_STORAGE/nbdtest"
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

	$rpc_py bdev_wait_for_examine

	for ((i = 1; i <= 20; i++)); do
		if $rpc_py bdev_get_bdevs | jq -r '.[] .name' | grep -qw $bdev_name; then
			return 0
		fi

		if $rpc_py bdev_get_bdevs | jq -r '.[] .aliases' | grep -qw $bdev_name; then
			return 0
		fi

		sleep 0.1
	done

	return 1
}

function make_filesystem() {
	local fstype=$1
	local dev_name=$2
	local i=0
	local force

	if [ $fstype = ext4 ]; then
		force=-F
	else
		force=-f
	fi

	while ! mkfs.${fstype} $force ${dev_name}; do
		if [ $i -ge 15 ]; then
			return 1
		fi
		i=$((i + 1))
		sleep 1
	done

	return 0
}

function killprocess() {
	# $1 = process pid
	if [ -z "$1" ]; then
		return 1
	fi

	if kill -0 $1; then
		if [ $(uname) = Linux ]; then
			process_name=$(ps --no-headers -o comm= $1)
		else
			process_name=$(ps -c -o command $1 | tail -1)
		fi
		if [ "$process_name" = "sudo" ]; then
			# kill the child process, which is the actual app
			# (assume $1 has just one child)
			local child
			child="$(pgrep -P $1)"
			echo "killing process with pid $child"
			kill $child
		else
			echo "killing process with pid $1"
			kill $1
		fi

		# wait for the process regardless if its the dummy sudo one
		# or the actual app - it should terminate anyway
		wait $1
	else
		# the process is not there anymore
		echo "Process with pid $1 is not found"
	fi
}

function iscsicleanup() {
	echo "Cleaning up iSCSI connection"
	iscsiadm -m node --logout || true
	iscsiadm -m node -o delete || true
	rm -rf /var/lib/iscsi/nodes/*
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

function _start_stub() {
	# Disable ASLR for multi-process testing.  SPDK does support using DPDK multi-process,
	# but ASLR can still be unreliable in some cases.
	# We will reenable it again after multi-process testing is complete in kill_stub().
	# Save current setting so it can be restored upon calling kill_stub().
	_randomize_va_space=$(< /proc/sys/kernel/randomize_va_space)
	echo 0 > /proc/sys/kernel/randomize_va_space
	$rootdir/test/app/stub/stub $1 &
	stubpid=$!
	echo Waiting for stub to ready for secondary processes...
	while ! [ -e /var/run/spdk_stub0 ]; do
		# If stub dies while we wait, bail
		[[ -e /proc/$stubpid ]] || return 1
		sleep 1s
	done
	echo done.
}

function start_stub() {
	if ! _start_stub "$@"; then
		echo "stub failed" >&2
		return 1
	fi
}

function kill_stub() {
	if [[ -e /proc/$stubpid ]]; then
		kill $1 $stubpid
		wait $stubpid
	fi 2> /dev/null || :
	rm -f /var/run/spdk_stub0
	# Re-enable ASLR now that we are done with multi-process testing
	# Note: "1" enables ASLR w/o randomizing data segments, "2" adds data segment
	#  randomizing and is the default on all recent Linux kernels
	echo "${_randomize_va_space:-2}" > /proc/sys/kernel/randomize_va_space
}

function run_test() {
	if [ $# -le 1 ]; then
		echo "Not enough parameters"
		echo "usage: run_test test_name test_script [script_params]"
		exit 1
	fi

	xtrace_disable
	local test_name="$1"
	shift

	if [ -n "$test_domain" ]; then
		export test_domain="${test_domain}.${test_name}"
	else
		export test_domain="$test_name"
	fi

	timing_enter $test_name
	echo "************************************"
	echo "START TEST $test_name"
	echo "************************************"
	xtrace_restore
	time "$@"
	xtrace_disable
	echo "************************************"
	echo "END TEST $test_name"
	echo "************************************"
	timing_exit $test_name

	export test_domain=${test_domain%"$test_name"}
	if [ -n "$test_domain" ]; then
		export test_domain=${test_domain%?}
	fi

	if [ -z "$test_domain" ]; then
		echo "top_level $test_name" >> $output_dir/test_completions.txt
	else
		echo "$test_domain $test_name" >> $output_dir/test_completions.txt
	fi
	xtrace_restore
}

function skip_run_test_with_warning() {
	echo "WARNING: $1"
	echo "Test run may fail if run with autorun.sh"
	echo "Please check your $rootdir/test/common/skipped_tests.txt"
}

function print_backtrace() {
	# if errexit is not enabled, don't print a backtrace
	[[ "$-" =~ e ]] || return 0

	local args=("${BASH_ARGV[@]}")

	xtrace_disable
	# Reset IFS in case we were called from an environment where it was modified
	IFS=" "$'\t'$'\n'
	echo "========== Backtrace start: =========="
	echo ""
	for ((i = 1; i < ${#FUNCNAME[@]}; i++)); do
		local func="${FUNCNAME[$i]}"
		local line_nr="${BASH_LINENO[$((i - 1))]}"
		local src="${BASH_SOURCE[$i]}"
		local bt="" cmdline=()

		if [[ -f $src ]]; then
			bt=$(nl -w 4 -ba -nln $src | grep -B 5 -A 5 "^${line_nr}[^0-9]" \
				| sed "s/^/   /g" | sed "s/^   $line_nr /=> $line_nr /g")
		fi

		# If extdebug set the BASH_ARGC[i], try to fetch all the args
		if ((BASH_ARGC[i] > 0)); then
			# Use argc as index to reverse the stack
			local argc=${BASH_ARGC[i]} arg
			for arg in "${args[@]::BASH_ARGC[i]}"; do
				cmdline[argc--]="[\"$arg\"]"
			done
			args=("${args[@]:BASH_ARGC[i]}")
		fi

		echo "in $src:$line_nr -> $func($(
			IFS=","
			printf '%s\n' "${cmdline[*]:-[]}"
		))"
		echo "     ..."
		echo "${bt:-backtrace unavailable}"
		echo "     ..."
	done
	echo ""
	echo "========== Backtrace end =========="
	xtrace_restore
	return 0
}

function waitforserial() {
	local i=0
	local nvme_device_counter=1 nvme_devices=0
	if [[ -n "$2" ]]; then
		nvme_device_counter=$2
	fi

	# Wait initially for min 4s to make sure all devices are ready for use. It seems
	# that we may be racing with a kernel where in some cases immediate disconnect may
	# leave dangling subsystem with no-op block devices which can't be used nor removed
	# (unless kernel is rebooted) and which start to negatively affect all the tests.
	sleep 4
	while ((i++ <= 15)); do
		nvme_devices=$(lsblk -l -o NAME,SERIAL | grep -c "$1")
		((nvme_devices == nvme_device_counter)) && return 0
		if ((nvme_devices > nvme_device_counter)); then
			echo "$nvme_device_counter device(s) expected, found $nvme_devices" >&2
		fi
		echo "Waiting for devices"
		sleep 1
	done
	return 1
}

function waitforserial_disconnect() {
	local i=0
	while lsblk -o NAME,SERIAL | grep -q -w $1; do
		[ $i -lt 15 ] || break
		i=$((i + 1))
		echo "Waiting for disconnect devices"
		sleep 1
	done

	if lsblk -l -o NAME,SERIAL | grep -q -w $1; then
		return 1
	fi

	return 0
}

function waitforblk() {
	local i=0
	while ! lsblk -l -o NAME | grep -q -w $1; do
		[ $i -lt 15 ] || break
		i=$((i + 1))
		sleep 1
	done

	if ! lsblk -l -o NAME | grep -q -w $1; then
		return 1
	fi

	return 0
}

function waitforblk_disconnect() {
	local i=0
	while lsblk -l -o NAME | grep -q -w $1; do
		[ $i -lt 15 ] || break
		i=$((i + 1))
		sleep 1
	done

	if lsblk -l -o NAME | grep -q -w $1; then
		return 1
	fi

	return 0
}

function waitforfile() {
	local i=0
	while [ ! -e $1 ]; do
		[ $i -lt 200 ] || break
		i=$((i + 1))
		sleep 0.1
	done

	if [ ! -e $1 ]; then
		return 1
	fi

	return 0
}

function fio_config_gen() {
	local config_file=$1
	local workload=$2
	local bdev_type=$3
	local fio_dir=$CONFIG_FIO_SOURCE_DIR

	if [ -e "$config_file" ]; then
		echo "Configuration File Already Exists!: $config_file"
		return 1
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
		cat <<- EOL >> $config_file
			verify=sha1
			verify_backlog=1024
			rw=randwrite
		EOL

		# To avoid potential data race issue due to the AIO device
		# flush mechanism, add the flag to serialize the writes.
		# This is to fix the intermittent IO failure issue of #935
		if [ "$bdev_type" == "AIO" ]; then
			if [[ $($fio_dir/fio --version) == *"fio-3"* ]]; then
				echo "serialize_overlap=1" >> $config_file
			fi
		fi
	elif [ "$workload" == "trim" ]; then
		echo "rw=trimwrite" >> $config_file
	else
		echo "rw=$workload" >> $config_file
	fi
}

function fio_bdev() {
	# Setup fio binary cmd line
	local fio_dir=$CONFIG_FIO_SOURCE_DIR
	local bdev_plugin="$rootdir/build/fio/spdk_bdev"

	# Preload AddressSanitizer library to fio if fio_plugin was compiled with it
	local asan_lib
	asan_lib=$(ldd $bdev_plugin | grep libasan | awk '{print $3}')

	LD_PRELOAD="$asan_lib $bdev_plugin" "$fio_dir"/fio "$@"
}

function fio_nvme() {
	# Setup fio binary cmd line
	local fio_dir=$CONFIG_FIO_SOURCE_DIR
	local nvme_plugin="$rootdir/build/fio/spdk_nvme"

	# Preload AddressSanitizer library to fio if fio_plugin was compiled with it
	asan_lib=$(ldd $nvme_plugin | grep libasan | awk '{print $3}')

	LD_PRELOAD="$asan_lib $nvme_plugin" "$fio_dir"/fio "$@"
}

function get_lvs_free_mb() {
	local lvs_uuid=$1
	local lvs_info
	local fc
	local cs
	lvs_info=$($rpc_py bdev_lvol_get_lvstores)
	fc=$(jq ".[] | select(.uuid==\"$lvs_uuid\") .free_clusters" <<< "$lvs_info")
	cs=$(jq ".[] | select(.uuid==\"$lvs_uuid\") .cluster_size" <<< "$lvs_info")

	# Change to MB's
	free_mb=$((fc * cs / 1024 / 1024))
	echo "$free_mb"
}

function get_bdev_size() {
	local bdev_name=$1
	local bdev_info
	local bs
	local nb
	bdev_info=$($rpc_py bdev_get_bdevs -b $bdev_name)
	bs=$(jq ".[] .block_size" <<< "$bdev_info")
	nb=$(jq ".[] .num_blocks" <<< "$bdev_info")

	# Change to MB's
	bdev_size=$((bs * nb / 1024 / 1024))
	echo "$bdev_size"
}

function autotest_cleanup() {
	local autotest_es=$?
	xtrace_disable

	# catch any stray core files and kill all remaining SPDK processes. Update
	# autotest_es in case autotest reported success but cores and/or processes
	# were left behind regardless.

	process_core || autotest_es=1
	reap_spdk_processes || autotest_es=1

	$rootdir/scripts/setup.sh reset
	$rootdir/scripts/setup.sh cleanup
	if [ $(uname -s) = "Linux" ]; then
		if [[ $SPDK_TEST_USE_IGB_UIO -eq 1 ]]; then
			[[ -e /sys/module/igb_uio ]] && rmmod igb_uio
		else
			modprobe -r uio_pci_generic
		fi
	fi
	rm -rf "$asan_suppression_file"
	if [[ -n $old_core_pattern ]]; then
		echo "$old_core_pattern" > /proc/sys/kernel/core_pattern
	fi
	if [[ -e /proc/$udevadm_pid/status ]]; then
		kill "$udevadm_pid" || :
	fi

	shopt -s nullglob
	local storage_fallback_purge=("${TMPDIR:-/tmp}/spdk."??????)
	shopt -u nullglob

	if ((${#storage_fallback_purge[@]} > 0)); then
		rm -rf "${storage_fallback_purge[@]}"
	fi

	xtrace_restore
	return $autotest_es
}

function freebsd_update_contigmem_mod() {
	if [ $(uname) = FreeBSD ]; then
		kldunload contigmem.ko || true
		if [ -n "$SPDK_RUN_EXTERNAL_DPDK" ]; then
			cp -f "$SPDK_RUN_EXTERNAL_DPDK/kmod/contigmem.ko" /boot/modules/
			cp -f "$SPDK_RUN_EXTERNAL_DPDK/kmod/contigmem.ko" /boot/kernel/
			cp -f "$SPDK_RUN_EXTERNAL_DPDK/kmod/nic_uio.ko" /boot/modules/
			cp -f "$SPDK_RUN_EXTERNAL_DPDK/kmod/nic_uio.ko" /boot/kernel/
		else
			cp -f "$rootdir/dpdk/build/kmod/contigmem.ko" /boot/modules/
			cp -f "$rootdir/dpdk/build/kmod/contigmem.ko" /boot/kernel/
			cp -f "$rootdir/dpdk/build/kmod/nic_uio.ko" /boot/modules/
			cp -f "$rootdir/dpdk/build/kmod/nic_uio.ko" /boot/kernel/
		fi
	fi
}

function get_nvme_name_from_bdf() {
	blkname=()

	nvme_devs=$(lsblk -d --output NAME | grep "^nvme") || true
	if [ -z "$nvme_devs" ]; then
		return
	fi
	for dev in $nvme_devs; do
		link_name=$(readlink /sys/block/$dev/device/device) || true
		if [ -z "$link_name" ]; then
			link_name=$(readlink /sys/block/$dev/device)
		fi
		bdf=$(basename "$link_name")
		if [ "$bdf" = "$1" ]; then
			blkname+=($dev)
		fi
	done

	printf '%s\n' "${blkname[@]}"
}

function get_nvme_ctrlr_from_bdf() {
	bdf_sysfs_path=$(readlink -f /sys/class/nvme/nvme* | grep "$1/nvme/nvme")
	if [[ -z "$bdf_sysfs_path" ]]; then
		return
	fi

	printf '%s\n' "$(basename $bdf_sysfs_path)"
}

# Get BDF addresses of all NVMe drives currently attached to
# uio-pci-generic or vfio-pci
function get_nvme_bdfs() {
	xtrace_disable
	bdfs=$(jq -r .config[].params.traddr <<< $($rootdir/scripts/gen_nvme.sh))
	if [[ -z $bdfs ]]; then
		echo "No devices to test on!"
		exit 1
	fi
	echo "$bdfs"
	xtrace_restore
}

# Same as function above, but just get the first disks BDF address
function get_first_nvme_bdf() {
	head -1 <<< "$(get_nvme_bdfs)"
}

function nvme_namespace_revert() {
	$rootdir/scripts/setup.sh
	sleep 1
	bdfs=$(get_nvme_bdfs)

	$rootdir/scripts/setup.sh reset

	for bdf in $bdfs; do
		nvme_ctrlr=/dev/$(get_nvme_ctrlr_from_bdf ${bdf})
		if [[ -z "$nvme_ctrlr" ]]; then
			continue
		fi

		# Check Optional Admin Command Support for Namespace Management
		oacs=$(nvme id-ctrl ${nvme_ctrlr} | grep oacs | cut -d: -f2)
		oacs_ns_manage=$((oacs & 0x8))

		if [[ "$oacs_ns_manage" -ne 0 ]]; then
			# This assumes every NVMe controller contains single namespace,
			# encompassing Total NVM Capacity and formatted as 512 block size.
			# 512 block size is needed for test/vhost/vhost_boot.sh to
			# successfully run.

			unvmcap=$(nvme id-ctrl ${nvme_ctrlr} | grep unvmcap | cut -d: -f2)
			if [[ "$unvmcap" -eq 0 ]]; then
				# All available space already used
				continue
			fi
			tnvmcap=$(nvme id-ctrl ${nvme_ctrlr} | grep tnvmcap | cut -d: -f2)
			blksize=512

			size=$((tnvmcap / blksize))

			nvme detach-ns ${nvme_ctrlr} -n 0xffffffff -c 0 || true
			nvme delete-ns ${nvme_ctrlr} -n 0xffffffff || true
			nvme create-ns ${nvme_ctrlr} -s ${size} -c ${size} -b ${blksize}
			nvme attach-ns ${nvme_ctrlr} -n 1 -c 0
			nvme reset ${nvme_ctrlr}
			waitforfile "${nvme_ctrlr}n1"
		fi
	done
}

# Get BDFs based on device ID, such as 0x0a54
function get_nvme_bdfs_by_id() {
	local bdfs=()

	for bdf in $(get_nvme_bdfs); do
		device=$(cat /sys/bus/pci/devices/$bdf/device) || true
		if [[ "$device" == "$1" ]]; then
			bdfs+=($bdf)
		fi
	done

	printf '%s\n' "${bdfs[@]}"
}

function opal_revert_cleanup() {
	# The OPAL CI tests is only used for P4510 devices.
	mapfile -t bdfs < <(get_nvme_bdfs_by_id 0x0a54)
	if [[ -z ${bdfs[0]} ]]; then
		return 0
	fi

	$SPDK_BIN_DIR/spdk_tgt &
	spdk_tgt_pid=$!
	waitforlisten $spdk_tgt_pid

	bdf_id=0
	for bdf in "${bdfs[@]}"; do
		$rootdir/scripts/rpc.py bdev_nvme_attach_controller -b "nvme"${bdf_id} -t "pcie" -a ${bdf}
		# Ignore if this fails.
		$rootdir/scripts/rpc.py bdev_nvme_opal_revert -b "nvme"${bdf_id} -p test || true
		((++bdf_id))
	done

	killprocess $spdk_tgt_pid
}

function pap() {
	while read -r file; do
		cat <<- FILE
			--- $file ---
			$(<"$file")
			--- $file ---
		FILE
		rm -f "$file"
	done < <(find "$@" -type f | sort -u)
}

function get_proc_paths() {
	local procs proc
	if [[ $(uname -s) == Linux ]]; then
		for proc in /proc/[0-9]*; do
			[[ -e $proc/exe ]] || continue
			procs[${proc##*/}]=$(readlink -f "$proc/exe")
		done
	elif [[ $(uname -s) == FreeBSD ]]; then
		while read -r proc _ _ path; do
			[[ -e $path ]] || continue
			procs[proc]=$path
		done < <(procstat -ab)
	fi

	for proc in "${!procs[@]}"; do
		echo "$proc" "${procs[proc]}"
	done
}

is_exec_file() { [[ -f $1 && $(file "$1") =~ ELF.+executable ]]; }

function reap_spdk_processes() {
	local bins bin
	local misc_bins

	while read -r bin; do
		is_exec_file "$bin" && misc_bins+=("$bin")
	done < <(find "$rootdir"/test/{app,env,event} -type f)

	mapfile -t bins < <(readlink -f "$SPDK_BIN_DIR/"* "$SPDK_EXAMPLE_DIR/"* "${misc_bins[@]}")

	local spdk_pid spdk_pids path
	while read -r spdk_pid path; do
		if [[ ${bins[*]/$path/} != "${bins[*]}" ]]; then
			echo "$path is still up ($spdk_pid), killing"
			spdk_pids[spdk_pid]=$path
		fi
	done < <(get_proc_paths)

	((${#spdk_pids[@]} > 0)) || return 0

	kill -SIGTERM "${!spdk_pids[@]}" 2> /dev/null || :
	# Wait a bit and then use the stick
	sleep 2
	kill -SIGKILL "${!spdk_pids[@]}" 2> /dev/null || :

	return 1
}

function is_block_zoned() {
	local device=$1

	[[ -e /sys/block/$device/queue/zoned ]] || return 1
	[[ $(< "/sys/block/$device/queue/zoned") != none ]]
}

function get_zoned_devs() {
	local -gA zoned_devs=()
	local nvme bdf

	for nvme in /sys/block/nvme*; do
		if is_block_zoned "${nvme##*/}"; then
			zoned_devs["${nvme##*/}"]=$(< "$nvme/device/address")
		fi
	done
}

# Define temp storage for all the tests. Look for 2GB at minimum
set_test_storage "${TEST_MIN_STORAGE_SIZE:-$((1 << 31))}"

set -o errtrace
shopt -s extdebug
trap "trap - ERR; print_backtrace >&2" ERR

PS4=' \t	-- ${BASH_SOURCE#${BASH_SOURCE%/*/*}/}@${LINENO} -- \$ '
if $SPDK_AUTOTEST_X; then
	# explicitly enable xtraces, overriding any tracking information.
	unset XTRACE_DISABLED
	unset XTRACE_NESTING_LEVEL
	xtrace_fd
	xtrace_enable
else
	xtrace_restore
fi
