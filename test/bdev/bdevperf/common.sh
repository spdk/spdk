bdevperf=$rootdir/test/bdev/bdevperf/bdevperf

function create_job() {
	local job_section=$1
	local rw=$2
	local filename=$3

	if [[ $job_section == "global" ]]; then
		cat <<- EOF >> "$testdir"/test.conf
			[global]
			filename=${filename}
		EOF
	fi
	job="[${job_section}]"
	echo $global
	cat <<- EOF >> "$testdir"/test.conf
		${job}
		filename=${filename}
		bs=1024
		rwmixread=70
		rw=${rw}
		iodepth=256
		cpumask=0xff
	EOF
}

function get_num_jobs() {
	echo "$1" | grep -oE "Using job config with [0-9]+ jobs" | grep -oE "[0-9]+"
}

function cleanup() {
	rm -f $testdir/test.conf
}
