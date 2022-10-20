#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

curdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$curdir/../../../")

source "$rootdir/test/vhost/common.sh"

# Allow for the fio_conf() to slurp extra config from the stdin.
exec {fio_extra_conf}<&0

fio_conf() {
	cat <<- FIO
		[global]
		ioengine=${ioengine:-libaio}
		thread=1
		group_reporting=1
		direct=1
		verify=0
		norandommap=1
	FIO

	if [[ $ioengine == io_uring ]]; then
		cat <<- FIO_URING
			fixedbufs=${fixedbufs:-1}
			hipri=${hipri:-1}
			registerfiles=${registerfiles:-1}
			sqthread_poll=${sqthread_poll:-1}
		FIO_URING
	fi

	if [[ -e $fio_extra_conf ]]; then
		# Overridden through cmdline|env
		cat "$fio_extra_conf"
	elif [[ ! -t $fio_extra_conf ]]; then
		# Attached to stdin
		cat <&"$fio_extra_conf"
	fi

	cat <<- FIO
		[perf_test]
		stonewall
		description="Vhost performance test for a given workload"
		bs=${blksize:-4k}
		rw=${rw:-randread}
		rwmixread=${rwmixread:-70}
		iodepth=${iodepth:-128}
		time_based=1
		ramp_time=${ramptime:-10}
		runtime=${runtime:-10}
		numjobs=${numjobs:-1}
		# This option is meant to be sed'ed by the vhost's run_fio()
		filename=
	FIO
}

(($#)) && eval "$*"

perf_args+=("--vm-image=${vm_image:-$VM_IMAGE}")
perf_args+=("--ctrl-type=${ctrl_type:-spdk_vhost_scsi}")
perf_args+=(${split:+--use-split})
perf_args+=(${disk_map:+--disk-map="$disk_map"})
perf_args+=(${cpu_cfg:+--custom-cpu-cfg="$cpu_cfg"})

if [[ $auto_cfg == yes || $auto_cfg_print == yes ]]; then
	if [[ $auto_cfg_print == yes ]]; then
		"$curdir/conf-generator" -p all || exit 1
		exit 0
	fi
	cpu_out=$curdir/auto-cpu.conf disk_out=$curdir/auto-disk.conf \
		"$curdir/conf-generator" -s || exit 1
	perf_args+=("--disk-map=$disk_out")
	perf_args+=("--custom-cpu-cfg=$cpu_out")
fi

if [[ -n $extra_params ]]; then
	perf_args+=($extra_params)
fi

out_fio_conf=${out_fio_conf:-$curdir/fio.conf}
fio_conf > "$out_fio_conf"

[[ -z $only_fio_conf ]] || exit 0

trap 'rm -f "$out_fio_conf"' EXIT

"$rootdir/test/vhost/perf_bench/vhost_perf.sh" \
	"${perf_args[@]}" --fio-jobs="$out_fio_conf"${fio_jobs:+",$fio_jobs"}
