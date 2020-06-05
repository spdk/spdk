#!/usr/bin/env bash
[[ $(uname -s) == Linux ]] || exit 0

shopt -s extglob nullglob

declare -r rdma_rxe=/sys/module/rdma_rxe
declare -r rdma_rxe_add=$rdma_rxe/parameters/add
declare -r rdma_rxe_rm=$rdma_rxe/parameters/remove

declare -r infiniband=/sys/class/infiniband
declare -r net=/sys/class/net

uevent() (
	[[ -e $1/uevent ]] || return 0

	source "$1/uevent"

	if [[ -v $2 ]]; then
		echo "${!2}"
	elif [[ -n $3 ]]; then
		echo "$3"
	fi
)

modprobeq() {
	modprobe -q "$@"
}

get_ipv4() {
	local ip

	# Get only the first ip
	read -r _ _ _ ip _ < <(ip -o -4 addr show dev "$1")
	if [[ -n $ip ]]; then
		echo "${ip%/*}"
	else
		echo "            "
	fi
}

get_rxe_mtu() {
	local rxe=$1
	local mtu

	[[ -c /dev/infiniband/uverbs${rxe/rxe/} ]] || return 0

	[[ $(ibv_devinfo -d "$rxe") =~ active_mtu:(.*\ \(.*\)) ]]
	echo "${BASH_REMATCH[1]:-(?)}"
}

start() {
	local modules module

	modules=(
		"ib_core"
		"ib_uverbs"
		"rdma_ucm"
		"rdma_rxe"
	)

	for module in "${modules[@]}"; do
		[[ -e /sys/module/$module ]] && continue
		if [[ ! -e $(modinfo -F filename "$module") ]]; then
			return 0
		fi
	done 2> /dev/null

	modprobeq -a "${modules[@]}" || return 1
	add_rxe all
}

stop() {
	local rxe

	for rxe in "$infiniband/rxe"+([0-9]); do
		remove_rxe "${rxe##*/}"
	done

	if ! modprobeq -r rdma_rxe \
		|| [[ -e $rdma_rxe ]]; then
		printf 'unable to unload drivers, reboot required\n'
	fi
}

status_header() {
	local header=("Name" "Link" "Driver" "Speed" "NMTU" "IPv4_addr" "RDEV" "RMTU")

	size_print_fields "${header[@]}"
}

status() {
	if [[ ! -e $rdma_rxe ]]; then
		printf 'rdma_rxe module not loaded\n' >&2
	fi

	local dev net_devs
	local link_map

	link_map[0]=no
	link_map[1]=yes

	status_header

	local name link driver speed mtu ip rxe rxe_dev active_mtu
	for dev in "$net/"!(bonding_masters); do
		(($(< "$dev/type") == 1)) || continue

		name="" link="" driver=""
		speed="" mtu="" ip=""
		rxe_dev="" active_mtu=""

		name=${dev##*/}
		for rxe in "$infiniband/rxe"+([0-9]); do
			if [[ $(< "$rxe/parent") == "$name" ]]; then
				rxe_dev=${rxe##*/}
				active_mtu=$(get_rxe_mtu "$rxe_dev")
				break
			fi
		done

		link=${link_map[$(< "$dev/carrier")]}

		if [[ -e $dev/device/driver ]]; then
			driver=$(readlink -f "$dev/device/driver")
			driver=${driver##*/}
		elif [[ -e /sys/devices/virtual/net/${dev##*/} ]]; then
			# Try to be smart and get the type of the device instead
			driver=$(uevent "$dev" "DEVTYPE" "virtual")
		fi

		if [[ $link == yes ]]; then
			speed=$(< "$dev/speed")
			if ((speed >= 1000)); then
				speed=$((speed / 1000))GigE
			elif ((speed > 0)); then
				speed=${speed}Mb/s
			else
				speed=""
			fi
		fi

		mtu=$(< "$dev/mtu")
		ip=$(get_ipv4 "$name")

		size_print_fields \
			"$name" \
			"$link" \
			"$driver" \
			"$speed" \
			"$mtu" \
			"$ip" \
			"$rxe_dev" \
			"$active_mtu"
	done 2> /dev/null
	print_status
}

size_print_fields() {
	local fields=("$@") field
	local -g lengths lines lineno

	for field in "${!fields[@]}"; do
		if [[ -z ${fields[field]} ]]; then
			fields[field]="###"
		fi
		if [[ -z ${lengths[field]} ]]; then
			lengths[field]=${#fields[field]}
		else
			lengths[field]=$((lengths[field] > ${#fields[field]} ? lengths[field] : ${#fields[field]}))
		fi
	done

	eval "local -g _line_$lineno=(\"\${fields[@]}\")"
	lines+=("_line_${lineno}[@]")
	((++lineno))
}

print_status() {
	local field field_ref fieldidx
	local pad

	for field_ref in "${lines[@]}"; do
		printf '  '
		fieldidx=0
		for field in "${!field_ref}"; do
			if [[ -n $field ]]; then
				pad=$((lengths[fieldidx] - ${#field} + 2))
			else
				pad=$((lengths[fieldidx] + 2))
			fi
			if [[ -n $field && $field != "###" ]]; then
				printf '%s' "$field"
			else
				printf '   '
			fi
			printf '%*s' "$pad" ""
			((++fieldidx))
		done
		printf '\n'
	done
}

add_rxe() {
	local dev net_devs

	[[ -e $rdma_rxe/parameters ]] || return 1

	if [[ -z $1 || $1 == all ]]; then
		net_devs=("$net/"!(bonding_masters))
	elif [[ -e $net/$1 ]]; then
		net_devs=("$net/$1")
	else
		printf '%s interface does not exist\n' "$1"
		return 1
	fi

	for dev in "${net_devs[@]}"; do
		(($(< "$dev/type") != 1)) && continue
		echo "${dev##*/}" > "$rdma_rxe_add"
	done 2> /dev/null
}

remove_rxe() {
	[[ -e $infiniband/${1##*/} ]] && echo "${1##*/}" > "$rdma_rxe_rm"
}

link_up_rxes() {
	local rxe parent

	for rxe in "$infiniband/rxe"+([0-9]); do
		parent=$(< /"$rxe/parent")
		link_up "$parent"
	done
}

link_up() {
	[[ -e $net/$1 ]] || return 0

	echo $(($(< "$net/$1/flags") | 0x1)) > "$net/$1/flags"
}

case "${1:-status}" in
	start)
		start
		;;
	stop)
		stop
		;;
	add)
		add_rxe "${2:-all}"
		;;
	remove)
		remove_rxe "$2"
		;;
	status)
		IFS= read -r match < <(
			IFS="|"
			printf '%s\n' "${*:2}"
		)
		status | grep -E "${match:-.}"
		;;
	*)
		printf 'Invalid argument (%s)\n' "$1"
		;;
esac
