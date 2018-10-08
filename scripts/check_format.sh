#!/usr/bin/env bash

readonly BASEDIR=$(readlink -f $(dirname $0))/..
cd $BASEDIR

# exit on errors
set -e

rc=0

echo -n "Checking file permissions..."

while read -r perm _res0 _res1 path; do
	if [ ! -f "$path" ]; then
		continue
	fi

	fname=$(basename -- "$path")

	case ${fname##*.} in
		c|h|cpp|cc|cxx|hh|hpp|md|html|js|json|svg|Doxyfile|yml|LICENSE|README|conf|in|Makefile|mk|gitignore|go|txt)
			# These file types should never be executable
			if [ "$perm" -eq 100755 ]; then
				echo "ERROR: $path is marked executable but is a code file."
				rc=1
			fi
		;;
		*)
			shebang=$(head -n 1 $path | cut -c1-3)

			# git only tracks the execute bit, so will only ever return 755 or 644 as the permission.
			if [ "$perm" -eq 100755 ]; then
				# If the file has execute permission, it should start with a shebang.
				if [ "$shebang" != "#!/" ]; then
					echo "ERROR: $path is marked executable but does not start with a shebang."
					rc=1
				fi
			else
				# If the file doesnot have execute permissions, it should not start with a shebang.
				if [ "$shebang" = "#!/" ]; then
					echo "ERROR: $path is not marked executable but starts with a shebang."
					rc=1
				fi
			fi
		;;
	esac

done <<< "$(git grep -I --name-only --untracked -e . | git ls-files -s)"

if [ $rc -eq 0 ]; then
	echo " OK"
fi

if hash astyle; then
	echo -n "Checking coding style..."
	if [ "$(astyle -V)" \< "Artistic Style Version 3" ]
	then
		echo -n " Your astyle version is too old. This may cause failure on patch verification performed by CI. Please update astyle to at least 3.0.1 version..."
	fi
	rm -f astyle.log
	touch astyle.log
	# Exclude rte_vhost code imported from DPDK - we want to keep the original code
	#  as-is to enable ongoing work to synch with a generic upstream DPDK vhost library,
	#  rather than making diffs more complicated by a lot of changes to follow SPDK
	#  coding standards.
	git ls-files '*.[ch]' '*.cpp' '*.cc' '*.cxx' '*.hh' '*.hpp' | \
		grep -v rte_vhost | grep -v cpp_headers | \
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
git grep --line-number -e '[^ ][*]/' -- '*.[ch]' ':!lib/vhost/rte_vhost*/*' >> comment.log || true
git grep --line-number -e '^[*]' -- '*.[ch]' >> comment.log || true

if [ -s comment.log ]; then
	echo " Incorrect comment formatting detected"
	cat comment.log
	rc=1
else
	echo " OK"
fi
rm -f comment.log

echo -n "Checking for spaces before tabs..."
git grep --line-number $' \t' -- > whitespace.log || true
if [ -s whitespace.log ]; then
	echo " Spaces before tabs detected"
	cat whitespace.log
	rc=1
else
	echo " OK"
fi
rm -f whitespace.log

echo -n "Checking trailing whitespace in output strings..."

git grep --line-number -e ' \\n"' -- '*.[ch]' > whitespace.log || true

if [ -s whitespace.log ]; then
	echo " Incorrect trailing whitespace detected"
	cat whitespace.log
	rc=1
else
	echo " OK"
fi
rm -f whitespace.log

echo -n "Checking for use of forbidden library functions..."

git grep --line-number -w '\(strncpy\|strcpy\|strcat\|sprintf\|vsprintf\)' -- './*.c' ':!lib/vhost/rte_vhost*/**' > badfunc.log || true
if [ -s badfunc.log ]; then
	echo " Forbidden library functions detected"
	cat badfunc.log
	rc=1
else
	echo " OK"
fi
rm -f badfunc.log

echo -n "Checking for use of forbidden CUnit macros..."

git grep --line-number -w 'CU_ASSERT_FATAL' -- 'test/*' ':!test/spdk_cunit.h' > badcunit.log || true
if [ -s badcunit.log ]; then
	echo " Forbidden CU_ASSERT_FATAL usage detected - use SPDK_CU_ASSERT_FATAL instead"
	cat badcunit.log
	rc=1
else
	echo " OK"
fi
rm -f badcunit.log

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
git grep -I -i -f scripts/posix.txt -- './*' ':!include/spdk/stdinc.h' ':!include/linux/**' ':!lib/vhost/rte_vhost*/**' ':!scripts/posix.txt' > scripts/posix.log || true
if [ -s scripts/posix.log ]; then
	echo "POSIX includes detected. Please include spdk/stdinc.h instead."
	cat scripts/posix.log
	rc=1
else
	echo " OK"
fi
rm -f scripts/posix.log

if hash pycodestyle 2>/dev/null; then
	PEP8=pycodestyle
elif hash pep8 2>/dev/null; then
	PEP8=pep8
fi

if [ ! -z ${PEP8} ]; then
	echo -n "Checking Python style..."

	PEP8_ARGS+=" --max-line-length=140"

	error=0
	git ls-files '*.py' | xargs -n1 $PEP8 $PEP8_ARGS > pep8.log || error=1
	if [ $error -ne 0 ]; then
		echo " Python formatting errors detected"
		cat pep8.log
		rc=1
	else
		echo " OK"
	fi
	rm -f pep8.log
else
	echo "You do not have pycodestyle or pep8 installed so your Python style is not being checked!"
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
