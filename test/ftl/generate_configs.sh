#!/usr/bin/env bash
#
# Replaces FTL_* variables in config files inside the config/ directory.
# The following varaibles are replaced:
#	* FTL_CONF_DIR - config directory
#	* FTL_TRANSPORT_ADDR - SSD's PCIe address (defaults to first lnvm device)
#	* FTL_BDEV_NAME - name of the bdev
#	* FTL_BDEV_PUNITS - bdev's parallel unit range (e.g. 0-3)
#	* FTL_BDEV_UUID - bdev's uuid (used when in restore mode)
#

set -e

testdir=$(readlink -f $(dirname $0))

function usage {
	echo "Usage: $0 -a TRANSPORT_ADDR -n BDEV_NAME -l PUNITS [-u UUID]"
	echo "UUID is required when restoring device state"
	exit 1
}

function replace_file {
	fname=$1
	output=$(echo -n $fname | sed 's/\.in//g')
	temp=$(mktemp)

	cp -a $fname $temp
	for var in ${!vmap[@]}; do
		sed -i "s,$var,${vmap[$var]},g" $temp
	done

	mv $temp $output
}

while getopts ":a:n:l:m:u:" arg; do
	case "$arg" in
		a)	addr=$OPTARG	;;
		n)	name=$OPTARG	;;
		l)	punits=$OPTARG	;;
		u)	uuid=$OPTARG	;;
		h|*)	usage		;;
	esac
done

if [ -z "$addr" ] || [ -z "$name" ] || [ -z "$punits" ]; then
	usage
fi

files=$(find $testdir/config -type f -iname "*.in")

declare -A vmap
vmap[FTL_CONF_DIR]=$testdir/config
vmap[FTL_TRANSPORT_ADDR]=$addr
vmap[FTL_BDEV_NAME]=$name
vmap[FTL_BDEV_PUNITS]=$punits
vmap[FTL_BDEV_UUID]=${uuid:-}

for file in $files; do
	replace_file $file
done
