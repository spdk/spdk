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
	git ls-files '*.[ch]' '*.cpp' '*.cc' '*.cxx' '*.hh' '*.hpp' | \
		grep -v rte_vhost | grep -v rte_virtio | grep -v cpp_headers | \
		xargs astyle --options=.astylerc >> astyle.log
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

echo -n "Checking comment style..."

git grep --line-number -e '/[*][^ *-]' -- '*.[ch]' > comment.log || true
git grep --line-number -e '[^ ][*]/' -- '*.[ch]' ':!lib/vhost/rte_vhost*/*' ':!lib/bdev/virtio/rte_virtio*/*' >> comment.log || true
git grep --line-number -e '^[*]' -- '*.[ch]' >> comment.log || true

if [ -s comment.log ]; then
	echo " Incorrect comment formatting detected"
	cat comment.log
	rc=1
else
	echo " OK"
fi
rm -f comment.log

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

echo -n "Checking for POSIX includes..."
git grep -I -i -f scripts/posix.txt -- './*' ':!include/spdk/stdinc.h' ':!lib/vhost/rte_vhost*/**' ':!lib/bdev/virtio/rte_virtio*/**' ':!scripts/posix.txt' > scripts/posix.log || true
if [ -s scripts/posix.log ]; then
	echo "POSIX includes detected. Please include spdk/stdinc.h instead."
	cat scripts/posix.log
	rc=1
else
	echo " OK"
fi
rm -f scripts/posix.log

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

# Check if any of the public interfaces were modified by this patch.
# Warn the user to consider updating the changelog any changes
# are detected.
echo -n "Checking whether CHANGELOG.md should be updated..."
staged=$(git diff --name-only --cached .)
working=$(git status -s --porcelain | grep -iv "??" | awk '{print $2}')
files="$staged $working"
if [[ "$files" = " " ]]; then
	files=$(git diff-tree --no-commit-id --name-only -r HEAD)
fi

has_changelog=0
for f in $files; do
	if [[ $f == CHANGELOG.md ]]; then
		# The user has a changelog entry, so exit.
		has_changelog=1
		break
	fi
done

needs_changelog=0
if [ $has_changelog -eq 0 ]; then
	for f in $files; do
		if [[ $f == include/spdk/* ]] || [[ $f == scripts/rpc.py ]] || [[ $f == etc/* ]]; then
			echo ""
			echo -n "$f was modified. Consider updating CHANGELOG.md."
			needs_changelog=1
		fi
	done
fi

if [ $needs_changelog -eq 0 ]; then
	echo " OK"
else
	echo ""
fi

exit $rc
