#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)

for file in $(ls -1 $curdir)
do
	if [[ "$curdir/$file" == "$BASH_SOURCE" ]]; then
		continue
	fi
	if [[ -x "$curdir/$file" ]]; then
		$curdir/$file || exit 1
	fi
done
