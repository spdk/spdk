# Prints error message and return error code, closes vhost app and remove
# pmem pool file
# input: error message, error code
function error() {
	local error_code=${2:-1}
	echo "==========="
	echo -e "ERROR: $1"
	echo "error code: $error_code"
	echo "==========="
	vhost_kill 0
	pmem_clean_pool_file
	return $error_code
}

# check if there is pool file & remove it
# input: path to pool file
# default: $default_pool_file
function pmem_clean_pool_file() {
	local pool_file=${1:-$default_pool_file}

	if [ -f $pool_file ]; then
		echo "Deleting old pool_file"
		rm $pool_file
	fi
}

# create new pmem file
# input: path to pool file, size in MB, block_size
# default: $default_pool_file 32 512
function pmem_create_pool_file() {
	local pool_file=${1:-$default_pool_file}
	local size=${2:-32}
	local block_size=${3:-512}

	pmem_clean_pool_file $pool_file
	echo "Creating new pool file"
	if ! $rpc_py bdev_pmem_create_pool $pool_file $size $block_size; then
		error "Creating pool_file failed!"
	fi

	if [ ! -f $pool_file ]; then
		error "Creating pool_file failed!"
	fi
}

function pmem_unmount_ramspace() {
	if [ -d "$testdir/ramspace" ]; then
		if mount | grep -q "$testdir/ramspace"; then
			umount $testdir/ramspace
		fi

		rm -rf $testdir/ramspace
	fi
}

function pmem_print_tc_name() {
	echo ""
	echo "==============================================================="
	echo "Now running: $1"
	echo "==============================================================="
}

function vhost_start() {
	local vhost_pid

	$SPDK_BIN_DIR/vhost &

	vhost_pid=$!
	echo $vhost_pid > $testdir/vhost.pid
	waitforlisten $vhost_pid
}

function vhost_kill() {
	local vhost_pid_file="$testdir/vhost.pid"
	local vhost_pid
	vhost_pid="$(cat $vhost_pid_file)"

	if [[ ! -f $vhost_pid_file ]]; then
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
