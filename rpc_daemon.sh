./scripts/rpc.py --daemon --dry_run &
rpc_pid=$!
sleep 0.5

time echo -e "get_spdk_version\nget_bdevs\nconstruct_malloc_bdev 8 512 -b Malloc2
get_bdevs\nconstruct_passthru_bdev -b Malloc2 -p Passthru1\nget_bdevs" | nc -U /tmp/socketname
time {
	echo "get_spdk_version" | nc -U /tmp/socketname
	echo "get_bdevs" | nc -U /tmp/socketname
	echo "construct_malloc_bdev 8 512 -b Malloc2" | nc -U /tmp/socketname
	echo "get_bdevs" | nc -U /tmp/socketname
	echo "construct_passthru_bdev -b Malloc2 -p Passthru1" | nc -U /tmp/socketname
	echo "get_bdevs" | nc -U /tmp/socketname
} 

echo "$rpc_pid"
kill $rpc_pid
rm /tmp/socketname
