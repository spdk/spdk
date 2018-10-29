#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..

function usage {
	echo "Replaces FTL_* variables in config files inside the config/ directory."
	echo "The following varaibles are replaced:"
	echo "- FTL_CONF_DIR - config directory"
	echo "- FTL_TRANSPORT_ADDR - SSD's PCIe address (defaults to first lnvm device)"
	echo "- FTL_BDEV_NAME - name of the bdev"
	echo "- FTL_BDEV_PUNITS - bdev's parallel unit range (e.g. 0-3)"
	echo "- FTL_BDEV_UUID - bdev's uuid (used when in restore mode)"
	echo
	echo "Usage: $0 -a TRANSPORT_ADDR -n BDEV_NAME -l PUNITS [-u UUID]"
	echo "UUID is required when restoring device state"
}

function generate_config {
	fname=$1
	output=${1%.in}

	cp $fname $output
	for var in ${!vmap[@]}; do
		sed -i "s,$var,${vmap[$var]},g" $output
	done
}

while getopts ":a:n:l:m:u:" arg; do
	case "$arg" in
		a)	addr=$OPTARG	;;
		n)	name=$OPTARG	;;
		l)	punits=$OPTARG	;;
		u)	uuid=$OPTARG	;;
		h)	usage
			exit 0		;;
		*)	usage
			exit 1		;;
	esac
done

if [[ -z "$addr" || -z "$name" || -z "$punits" ]]; then
	usage
	exit 1
fi

declare -A vmap
vmap[FTL_CONF_DIR]=$rootdir/test/ftl/config
vmap[FTL_TRANSPORT_ADDR]=$addr
vmap[FTL_BDEV_NAME]=$name
vmap[FTL_BDEV_PUNITS]=$punits
vmap[FTL_BDEV_UUID]=${uuid:-}

for file in $(find $rootdir/test/ftl/config -type f -iname "*.in"); do
	generate_config $file
done
