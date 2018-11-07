#!/usr/bin/env bash

set -x

if [ $# -ne 2 ]; then
	echo "This script need exactly two arguments"
	exit 1
fi

rootdir=$(readlink -f $(dirname $0)/../..)
# FIXME: Remove this when python3 will on FeeBSD machines
if [ $(uname -s) = "FreeBSD" ]; then
	python_cmd=python
else
	python_cmd=""
fi

# Compare two JSON files.
#
# NOTE: Order of objects in JSON can change by just doing loads -> dumps so all JSON objects (not arrays) are sorted by
# config_filter.py script. Sorted output is used to compare JSON output.
#

tmp_file_1=$(mktemp /tmp/$(basename ${1}).XXX)
tmp_file_2=$(mktemp /tmp/$(basename ${2}).XXX)
ret=0

cat $1 | $python_cmd $rootdir/test/json_config/config_filter.py -method "sort" > $tmp_file_1
cat $2 | $python_cmd $rootdir/test/json_config/config_filter.py -method "sort" > $tmp_file_2

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
