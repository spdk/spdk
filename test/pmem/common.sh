BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"
rpc_py="$TEST_DIR/scripts/rpc.py "

source $TEST_DIR/test/common/autotest_common.sh

# Prints error message and return error code, closes vhost app and remove
# pmem pool file
# input: error message, error code
function error()
{
	local error_code=${2:-1}
	echo "==========="
	echo -e "ERROR: $1"
	echo "error code: $error_code"
	echo "==========="
	vhost_kill
	pmem_clean_pool_file
	return $error_code
}

# check if there is pool file & remove it
# input: path to pool file
# default: $TEST_DIR/test/pmem/pool_file
function pmem_clean_pool_file()
{
	local pool_file=${1:-$TEST_DIR/test/pmem/pool_file}

	if [ -f $pool_file ]; then
		echo "Deleting old pool_file"
		rm $pool_file
	fi
}

# create new pmem file
# input: path to pool file, size in MB, block_size
# default: $TEST_DIR/test/pmem/pool_file 32 512
function pmem_create_pool_file()
{
	local pool_file=${1:-$TEST_DIR/test/pmem/pool_file}
	local size=${2:-32}
	local block_size=${3:-512}

	pmem_clean_pool_file $pool_file
	echo "Creating new pool file"
	if ! $rpc_py create_pmem_pool $pool_file $size $block_size; then
		error "Creating pool_file failed!"
	fi

	if [ ! -f $pool_file ]; then
		error "Creating pool_file failed!"
	fi
}

function pmem_unmount_ramspace
{
	if [ -d "$TEST_DIR/test/pmem/ramspace" ]; then
		if mount | grep -q "$TEST_DIR/test/pmem/ramspace"; then
			umount $TEST_DIR/test/pmem/ramspace
		fi

		rm -rf $TEST_DIR/test/pmem/ramspace
	fi
}

function pmem_print_tc_name
{
	echo ""
	echo "==============================================================="
	echo "Now running: $1"
	echo "==============================================================="
}

function vhost_start()
{
	local vhost_pid

	$TEST_DIR/app/vhost/vhost &
	if [ $? != 0 ]; then
		echo -e "ERROR: Failed to launch vhost!"
		return 1
	fi

	vhost_pid=$!
	echo $vhost_pid > $TEST_DIR/test/pmem/vhost.pid
	waitforlisten $vhost_pid
}

function vhost_kill()
{
	local vhost_pid_file="$TEST_DIR/test/pmem/vhost.pid"
	local vhost_pid="$(cat $vhost_pid_file)"

	if [[ ! -f $TEST_DIR/test/pmem/vhost.pid ]]; then
		echo -e "ERROR: No vhost pid file found!"
		return 1
	fi

	if ! kill -s INT $vhost_pid; then
		echo -e "ERROR: Failed to exit vhost / invalid pid!"
		rm $vhost_pid_file
		return 1
	fi

	sleep 1
	rm $vhost_pid_file
}
