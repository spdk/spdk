#!/usr/bin/env bash

set -x

if [ $# -ne 2 ]; then
	echo "This script need exactly two arguments"
	exit 1
fi

rootdir=$(readlink -f $(dirname $0)/../..)

# Compare two JSON files.
#
# NOTE: Order of objects in JSON can change by just doing loads -> dumps so all JSON objects (not arrays) are sorted by
# config_filter.py script. Sorted output is used to compare JSON output.
#

tmp_file_1=$(mktemp /tmp/$(basename ${1}).XXX)
tmp_file_2=$(mktemp /tmp/$(basename ${2}).XXX)
ret=0

$rootdir/test/json_config/config_filter.py -method "sort" < $1 > $tmp_file_1
$rootdir/test/json_config/config_filter.py -method "sort" < $2 > $tmp_file_2

if ! diff -u $tmp_file_1 $tmp_file_2; then
	ret=1

	echo "=== Start of file: $tmp_file_1 ==="
	cat $tmp_file_1
	echo "=== End of file: $tmp_file_1 ==="
	echo ""
	echo "=== Start of file: $tmp_file_2 ==="
	cat $tmp_file_2
	echo "=== End of file: $tmp_file_2 ==="
	echo ""
else
	echo "INFO: JSON config files are the same"
fi

rm $tmp_file_1 $tmp_file_2
exit $ret
