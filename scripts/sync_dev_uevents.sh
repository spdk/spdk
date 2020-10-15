#!/usr/bin/env bash
shopt -s extglob

exec {err}>&2

help() {
	cat <<- HELP
		${0##*/}: subsystem dev [..devN]

		Env:
		  UEVENT_TIMEOUT    - how long to wait for sync - ${UEVENT_TIMEOUT:-10}s
		  UEVENT_ACTION     - uevent action to match on - ${UEVENT_ACTION:-add}
		  DEVPATH_LOOKUP    - check if given dev matches inside DEVPATH
		  DEVPATH_SUBSYSTEM - subsystem given dev should match in DEVPATH
	HELP
}

get_uevent_attr() (
	source "$1"

	[[ -v $2 ]] && echo "${!2}"
)

filter_devs() {
	local dev p_dev
	local maj min type sub

	for dev in "${!devs[@]}"; do
		[[ -e /dev/${devs[dev]} ]] || continue
		[[ -c /dev/${devs[dev]} ]] && type=char
		[[ -b /dev/${devs[dev]} ]] && type=block
		maj=$((0x$(stat --printf="%t" "/dev/${devs[dev]}")))
		min=$((0x$(stat --printf="%T" "/dev/${devs[dev]}")))

		p_dev=/sys/dev/$type/$maj:$min
		if [[ -e $p_dev ]]; then
			printf '/dev/%s\n' "${devs[dev]}"

			type=$(get_uevent_attr "$p_dev/uevent" DEVTYPE)
			sub=$(readlink -f "$p_dev/subsystem") sub=${sub##*/}
			if [[ $sub != "${subsystem%%/*}" ]]; then
				printf '  wrong subsystem specified (%s != %s)\n' \
					"${subsystem%%/*}" "$sub"
			fi >&2

			if [[ ${subsystem##*/} != "$subsystem" && -n $type ]]; then
				if [[ ${subsystem##*/} != "$type" ]]; then
					printf '  wrong devtype specified (%s != %s)\n' \
						"${subsystem##*/}" "$type"
				fi
			fi >&2

			unset -v "devs[dev]"
		fi
	done
}

look_in_devpath() {
	local find=$1
	local path=$2
	local sub

	[[ -v DEVPATH_LOOKUP ]] || return 1

	if [[ -z $path ]]; then
		return 1
	fi

	if [[ -e $path/subsystem ]]; then
		sub=$(readlink -f "$path/subsystem")
		sub=${sub##*/}
	fi

	if [[ ${path##*/} == "$find" ]]; then
		if [[ -n $DEVPATH_SUBSYSTEM ]]; then
			[[ $DEVPATH_SUBSYSTEM == "$sub" ]] || return 1
		fi
		return 0
	fi
	look_in_devpath "$find" "${path%/*}"
}

if (($# < 2)); then
	help
	exit 1
fi

subsystem=$1 devs=("${@:2}")
timeout=${UEVENT_TIMEOUT:-10}
action=${UEVENT_ACTION:-add}

devs=("${devs[@]#/dev/}")
[[ $action == add ]] && filter_devs

((${#devs[@]})) || exit 0

if [[ -S /run/udev/control ]]; then
	# systemd-udevd realm

	# If devtmpfs is in place then all, e.g., block subsystem devices are going to
	# be handled directly by the kernel. Otherwise, link to udev events in case we
	# have some old udevd on board which is meant to mknod them instead.
	if [[ $(< /proc/mounts) == *"/dev devtmpfs"* ]]; then
		events+=(--kernel)
	else
		events+=(--udev)
	fi

	if [[ $subsystem != all ]]; then
		events+=("--subsystem-match=$subsystem")
	fi

	# This trap targets a subshell which forks udevadm monitor. Since
	# process substitution works in an async fashion, $$ won't wait
	# for it, leaving it's child unattended after the main loop breaks
	# (udevadm won't exit on its own either).
	trap '[[ -e /proc/$!/status ]] && pkill -P $!' EXIT
	# Also, this will block while reading through a pipe with a timeout
	# after not receiving any input. stdbuf is used since udevadm always
	# line buffers the monitor output.
	while ((${#devs[@]} > 0)) && IFS="=" read -t"$timeout" -r k v; do
		if [[ $k == ACTION && $v == "$action" ]]; then
			look_for_devname=1
			continue
		fi
		if ((look_for_devname == 1)); then
			for dev in "${!devs[@]}"; do
				# Explicitly allow globbing of the rhs to allow more open matching.
				# shellcheck disable=SC2053
				if [[ ${v#/dev/} == ${devs[dev]} || ${v##*/} == ${devs[dev]##*/} ]] \
					|| look_in_devpath "${devs[dev]}" "/sys/$v"; then
					unset -v "devs[dev]"
					look_for_devname=0
				fi
			done
		fi
	done < <(stdbuf --output=0 udevadm monitor --property "${events[@]}")
	if ((${#devs[@]} > 0)); then
		printf '* Events for some %s devices (%s) were not caught, they may be missing\n' \
			"$subsystem" "${devs[*]}"
	fi >&"$err"
	exit 0
elif [[ -e /sys/kernel/uevent_helper ]]; then
	# Check if someones uses mdev to serialize uevents. If yes, simply check
	# if they are in sync, no need to lookup specific devices in this case.
	# If not, fall through to plain sleep.
	# To quote some wisdom from gentoo:
	# "Even the craziest scenario deserves a fair chance".

	helper=$(< /sys/kernel/uevent_helper)
	if [[ ${helper##*/} == mdev && -e /dev/mdev.seq ]]; then
		# mdev keeps count of the seqnums on its own on each execution
		# and saves the count under /dev/mdev.seq. This is then set to
		# + 1 after the uevents finally settled.
		while ((timeout-- && $(< /sys/kernel/uevent_seqnum) + 1 != $(< /dev/mdev.seq))); do
			sleep 1s
		done
		if ((timeout < 0)); then
			printf '* Events not synced in time, %s devices (%s) may be missing\n' \
				"$subsystem" "${devs[*]}"
		fi
		exit 0
	fi >&"$err"
fi 2> /dev/null

# Fallback, sleep and hope for the best
sleep "${timeout}s"
