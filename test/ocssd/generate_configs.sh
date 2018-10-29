#!/usr/bin/env bash
#
# Replaces OCSSD_* variables in config files inside the config/ directory.
# The following varaibles are replaced:
#	* OCSSD_CONF_DIR - config directory
#	* OCSSD_TRANSPORT_ADDR - SSD's PCIe address (defaults to first lnvm device)
#	* OCSSD_BDEV_NAME - name of the bdev
#	* OCSSD_BDEV_PUNITS - bdev's parallel unit range (e.g. 0-3)
#	* OCSSD_BDEV_MODE - bdev mode (0 - existing instance; 1 - new instance)
#	* OCSSD_BDEV_UUID - bdev's uuid (used when in restore mode)
#

set -e

testdir=$(readlink -f $(dirname $0))

function usage {
	echo "Usage: $0 -a TRANSPORT_ADDR -n BDEV_NAME -l PUNITS -m MODE [-u UUID]"
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
		m)	mode=$OPTARG	;;
		u)	uuid=$OPTARG	;;
		h|*)	usage		;;
	esac
done

if [ -z "$addr" ] || [ -z "$name" ] || [ -z "$punits" ] || [ -z "$mode" ]; then
	usage
fi

if [ $[$mode & 1] -eq 0 ] && [ -z "$uuid" ]; then
	usage
fi

files=$(find $testdir/config -type f -iname "*.in")

declare -A vmap
vmap[OCSSD_CONF_DIR]=$testdir/config
vmap[OCSSD_TRANSPORT_ADDR]=$addr
vmap[OCSSD_BDEV_NAME]=$name
vmap[OCSSD_BDEV_PUNITS]=$punits
vmap[OCSSD_BDEV_MODE]=$mode
vmap[OCSSD_BDEV_UUID]=${uuid:-}

for file in $files; do
	replace_file $file
done
