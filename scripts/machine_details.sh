#!/usr/bin/env bash
# Run this script as root

INFO_LEVEL=0

function print_usage() {
	echo "usage: $(basename $1) --level|-l LEVEL"
	echo "       LEVEL can be 0, 1, 2 or 3"
	echo ""
	echo "Script displays software and hardware information"
	echo "depending on level of details"
	echo ""
}

case "$1" in
	--help) print_usage $0 && exit 0 ;;
	--level=*) INFO_LEVEL="${1#*=}"; shift 1 ;;
	-h) print_usage $0 && exit 0 ;;
        -l) INFO_LEVEL="$2"; shift 2 ;;
	-*) print_usage $0 && echo "Invalid argument '$OPTARG'" && exit 1 ;;
	*) print_usage $0 && echo "Invalid argument '$OPTARG'" && exit 1 ;;
esac

set -e
trap 'set +e; trap - ERR; echo "Error!"; exit 1;' ERR

scriptsdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $scriptsdir/..)

if [ $INFO_LEVEL -gt 0 ]; then
	echo "Uname -a:" $(uname -a)
fi

if [ $INFO_LEVEL -gt 0 ]; then
	if [ -f /etc/os-release ]; then
		cat /etc/os-release
	fi
fi

if [ $(uname) == "Linux" ]; then
	if [ $INFO_LEVEL -gt 0 ]; then
		grep Huge /proc/meminfo
	fi
	if [ $INFO_LEVEL -gt 1 ]; then
		free -m
		lspci
		lscpu
	fi
	if [ -s /etc/redhat-release ]; then
		if [ $INFO_LEVEL -gt 1 ]; then
			lshw -sanitize -businfo -numeric -C disk,volume,storage,net,cpu,memory
		fi
	elif [ -f /etc/debian_version ]; then
        	# Includes Ubuntu, Debian
		if [ $INFO_LEVEL -gt 1 ]; then
			lshw -sanitize -businfo -numeric -C disk,volume,storage,net,cpu,memory
			lstopo-no-graphics -v --no-caches --no-collapse
		fi
	elif [ -f /etc/SuSE-release ]; then
		if [ $INFO_LEVEL -gt 1 ]; then
			hwinfo
			procinfo
		fi
	fi
elif [ $(uname -s) = "FreeBSD" ] ; then
	if [ $INFO_LEVEL -gt 1 ]; then
		sysctl -a | grep hw.*mem
		sysctl hw.model hw.machine hw.ncpu hw.machine_arch
		camcontrol devlist
	fi
	if [ $INFO_LEVEL -gt 2 ]; then
		kenv
	fi
else
	echo "Machine details: Unknown system type."
fi
if hash astyle 2>/dev/null; then
	echo "Astyle version:" $(astyle -V)
fi
if hash gcc 2>/dev/null; then
	echo "GCC version:" $(gcc -dumpversion)
fi
if hash nasm 2>/dev/null; then
	echo "Nasm version:" $(nasm -v | awk '{ print $NF }')
fi
if hash ofed 2>/dev/null; then
	echo "Ofed version:" $(ofed -n)
fi
