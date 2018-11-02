#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)

for file in $(ls -1 $curdir)
do
	if [ $file == 'all.sh' ] || [ $file == 'common.sh' ]; then
		continue
	fi
	$curdir/$file || exit 1
done
