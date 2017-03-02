#!/usr/bin/env bash

readonly BASEDIR=$(readlink -f $(dirname $0))/..
cd $BASEDIR

# exit on errors
set -e

rc=0

if hash astyle; then
	echo -n "Checking coding style..."
	rm -f astyle.log
	touch astyle.log
	# Exclude rte_vhost code imported from DPDK - we want to keep the original code
	#  as-is to enable ongoing work to synch with a generic upstream DPDK vhost library,
	#  rather than making diffs more complicated by a lot of changes to follow SPDK
	#  coding standards.
	astyle --options=.astylerc "*.c" --exclude="rte_vhost" >> astyle.log
	astyle --options=.astylerc --exclude=test/cpp_headers "*.cpp" >> astyle.log
	astyle --options=.astylerc "*.h" --exclude="rte_vhost" >> astyle.log
	if grep -q "^Formatted" astyle.log; then
		echo " errors detected"
		git diff
		sed -i -e 's/  / /g' astyle.log
		grep --color=auto "^Formatted.*" astyle.log
		echo "Incorrect code style detected in one or more files."
		echo "The files have been automatically formatted."
		echo "Remember to add the files to your commit."
		rc=1
	else
		echo " OK"
	fi
	rm -f astyle.log
else
	echo "You do not have astyle installed so your code style is not being checked!"
fi

echo -n "Checking blank lines at end of file..."

if ! git grep -I -l -e . -z | \
	xargs -0 -P8 -n1 scripts/eofnl > eofnl.log; then
	echo " Incorrect end-of-file formatting detected"
	cat eofnl.log
	rc=1
else
	echo " OK"
fi
rm -f eofnl.log

if hash pep8; then
	echo -n "Checking Python style..."

	PEP8_ARGS+=" --ignore=E302" # ignore 'E302 expected 2 blank lines, found 1'
	PEP8_ARGS+=" --max-line-length=140"

	error=0
	git ls-files '*.py' | xargs -n1 pep8 $PEP8_ARGS > pep8.log || error=1
	if [ $error -ne 0 ]; then
		echo " Python formatting errors detected"
		cat pep8.log
		rc=1
	else
		echo " OK"
	fi
	rm -f pep8.log
fi

exit $rc
