#!/usr/bin/env bash

if [[ $(uname -s) == Darwin ]]; then
	# SPDK is not supported on MacOS, but as a developer
	# convenience we support running the check_format.sh
	# script on MacOS.
	# Running "brew install bash greadlink ggrep" should be
	# sufficient to get the correct versions of these utilities.
	if [[ $(type -t mapfile) != builtin ]]; then
		# We need bash version >= 4.0 for mapfile builtin
		echo "Please install bash version >= 4.0"
		exit 1
	fi
	if ! hash greadlink 2> /dev/null; then
		# We need GNU readlink for -f option
		echo "Please install GNU readlink"
		exit 1
	fi
	if ! hash ggrep 2> /dev/null; then
		# We need GNU grep for -P option
		echo "Please install GNU grep"
		exit 1
	fi
	GNU_READLINK="greadlink"
	GNU_GREP="ggrep"
else
	GNU_READLINK="readlink"
	GNU_GREP="grep"
fi

rootdir=$($GNU_READLINK -f "$(dirname "$0")")/..
source "$rootdir/scripts/common.sh"

cd "$rootdir"

# exit on errors
set -e

if ! hash nproc 2> /dev/null; then

	function nproc() {
		echo 8
	}

fi

function version_lt() {
	[ $(echo -e "$1\n$2" | sort -V | head -1) != "$1" ]
}

function array_contains_string() {
	name="$1[@]"
	array=("${!name}")

	for element in "${array[@]}"; do
		if [ "$element" = "$2" ]; then
			return $(true)
		fi
	done

	return $(false)
}

rc=0

function check_permissions() {
	echo -n "Checking file permissions..."

	local rc=0

	while read -r perm _res0 _res1 path; do
		if [ ! -f "$path" ]; then
			continue
		fi

		# Skip symlinks
		if [[ -L $path ]]; then
			continue
		fi
		fname=$(basename -- "$path")

		case ${fname##*.} in
			c | h | cpp | cc | cxx | hh | hpp | md | html | js | json | svg | Doxyfile | yml | LICENSE | README | conf | in | Makefile | mk | gitignore | go | txt)
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
					# If the file does not have execute permissions, it should not start with a shebang.
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

	return $rc
}

function check_c_style() {
	local rc=0

	if hash astyle; then
		echo -n "Checking coding style..."
		if [ "$(astyle -V)" \< "Artistic Style Version 3" ]; then
			echo -n " Your astyle version is too old so skipping coding style checks. Please update astyle to at least 3.0.1 version..."
		else
			rm -f astyle.log
			touch astyle.log
			# Exclude rte_vhost code imported from DPDK - we want to keep the original code
			#  as-is to enable ongoing work to synch with a generic upstream DPDK vhost library,
			#  rather than making diffs more complicated by a lot of changes to follow SPDK
			#  coding standards.
			git ls-files '*.[ch]' '*.cpp' '*.cc' '*.cxx' '*.hh' '*.hpp' \
				| grep -v rte_vhost | grep -v cpp_headers \
				| xargs -P$(nproc) -n10 astyle --options=.astylerc >> astyle.log
			if grep -q "^Formatted" astyle.log; then
				echo " errors detected"
				git diff --ignore-submodules=all
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
		fi
	else
		echo "You do not have astyle installed so your code style is not being checked!"
	fi
	return $rc
}

function check_comment_style() {
	local rc=0

	echo -n "Checking comment style..."

	git grep --line-number -e '\/[*][^ *-]' -- '*.[ch]' > comment.log || true
	git grep --line-number -e '[^ ][*]\/' -- '*.[ch]' ':!lib/rte_vhost*/*' >> comment.log || true
	git grep --line-number -e '^[*]' -- '*.[ch]' >> comment.log || true
	git grep --line-number -e '\s\/\/' -- '*.[ch]' >> comment.log || true
	git grep --line-number -e '^\/\/' -- '*.[ch]' >> comment.log || true

	if [ -s comment.log ]; then
		echo " Incorrect comment formatting detected"
		cat comment.log
		rc=1
	else
		echo " OK"
	fi
	rm -f comment.log

	return $rc
}

function check_spaces_before_tabs() {
	local rc=0

	echo -n "Checking for spaces before tabs..."
	git grep --line-number $' \t' -- './*' ':!*.patch' > whitespace.log || true
	if [ -s whitespace.log ]; then
		echo " Spaces before tabs detected"
		cat whitespace.log
		rc=1
	else
		echo " OK"
	fi
	rm -f whitespace.log

	return $rc
}

function check_trailing_whitespace() {
	local rc=0

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

	return $rc
}

function check_forbidden_functions() {
	local rc=0

	echo -n "Checking for use of forbidden library functions..."

	git grep --line-number -w '\(atoi\|atol\|atoll\|strncpy\|strcpy\|strcat\|sprintf\|vsprintf\)' -- './*.c' ':!lib/rte_vhost*/**' > badfunc.log || true
	if [ -s badfunc.log ]; then
		echo " Forbidden library functions detected"
		cat badfunc.log
		rc=1
	else
		echo " OK"
	fi
	rm -f badfunc.log

	return $rc
}

function check_cunit_style() {
	local rc=0

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

	return $rc
}

function check_eof() {
	local rc=0

	echo -n "Checking blank lines at end of file..."

	if ! git grep -I -l -e . -z './*' ':!*.patch' \
		| xargs -0 -P$(nproc) -n1 scripts/eofnl > eofnl.log; then
		echo " Incorrect end-of-file formatting detected"
		cat eofnl.log
		rc=1
	else
		echo " OK"
	fi
	rm -f eofnl.log

	return $rc
}

function check_posix_includes() {
	local rc=0

	echo -n "Checking for POSIX includes..."
	git grep -I -i -f scripts/posix.txt -- './*' ':!include/spdk/stdinc.h' ':!include/linux/**' ':!lib/rte_vhost*/**' ':!scripts/posix.txt' ':!*.patch' ':!configure' > scripts/posix.log || true
	if [ -s scripts/posix.log ]; then
		echo "POSIX includes detected. Please include spdk/stdinc.h instead."
		cat scripts/posix.log
		rc=1
	else
		echo " OK"
	fi
	rm -f scripts/posix.log

	return $rc
}

function check_naming_conventions() {
	local rc=0

	echo -n "Checking for proper function naming conventions..."
	# commit_to_compare = HEAD - 1.
	commit_to_compare="$(git log --pretty=oneline --skip=1 -n 1 | awk '{print $1}')"
	failed_naming_conventions=false
	changed_c_libs=()
	declared_symbols=()

	# Build an array of all the modified C libraries.
	mapfile -t changed_c_libs < <(git diff --name-only HEAD $commit_to_compare -- lib/**/*.c module/**/*.c | xargs -r dirname | sort | uniq)
	# Matching groups are 1. qualifiers / return type. 2. function name 3. argument list / comments and stuff after that.
	# Capture just the names of newly added (or modified) function definitions.
	mapfile -t declared_symbols < <(git diff -U0 $commit_to_compare HEAD -- include/spdk*/*.h | sed -En 's/(^[+].*)(spdk[a-z,A-Z,0-9,_]*)(\(.*)/\2/p')

	for c_lib in "${changed_c_libs[@]}"; do
		lib_map_file="mk/spdk_blank.map"
		defined_symbols=()
		removed_symbols=()
		exported_symbols=()
		if ls "$c_lib"/*.map &> /dev/null; then
			lib_map_file="$(ls "$c_lib"/*.map)"
		fi
		# Matching groups are 1. leading +sign. 2, function name 3. argument list / anything after that.
		# Capture just the names of newly added (or modified) functions that start with "spdk_"
		mapfile -t defined_symbols < <(git diff -U0 $commit_to_compare HEAD -- $c_lib | sed -En 's/(^[+])(spdk[a-z,A-Z,0-9,_]*)(\(.*)/\2/p')
		# Capture the names of removed symbols to catch edge cases where we just move definitions around.
		mapfile -t removed_symbols < <(git diff -U0 $commit_to_compare HEAD -- $c_lib | sed -En 's/(^[-])(spdk[a-z,A-Z,0-9,_]*)(\(.*)/\2/p')
		for symbol in "${removed_symbols[@]}"; do
			for i in "${!defined_symbols[@]}"; do
				if [[ ${defined_symbols[i]} = "$symbol" ]]; then
					unset -v 'defined_symbols[i]'
				fi
			done
		done
		# It's possible that we just modified a functions arguments so unfortunately we can't just look at changed lines in this function.
		# matching groups are 1. All leading whitespace 2. function name. Capture just the symbol name.
		mapfile -t exported_symbols < <(sed -En 's/(^[[:space:]]*)(spdk[a-z,A-Z,0-9,_]*);/\2/p' < $lib_map_file)
		for defined_symbol in "${defined_symbols[@]}"; do
			# if the list of defined symbols is equal to the list of removed symbols, then we are left with a single empty element. skip it.
			if [ "$defined_symbol" = '' ]; then
				continue
			fi
			not_exported=true
			not_declared=true
			if array_contains_string exported_symbols $defined_symbol; then
				not_exported=false
			fi

			if array_contains_string declared_symbols $defined_symbol; then
				not_declared=false
			fi

			if $not_exported || $not_declared; then
				if ! $failed_naming_conventions; then
					echo " found naming convention errors."
				fi
				echo "function $defined_symbol starts with spdk_ which is reserved for public API functions."
				echo "Please add this function to its corresponding map file and a public header or remove the spdk_ prefix."
				failed_naming_conventions=true
				rc=1
			fi
		done
	done

	if ! $failed_naming_conventions; then
		echo " OK"
	fi

	return $rc
}

function check_include_style() {
	local rc=0

	echo -n "Checking #include style..."
	git grep -I -i --line-number "#include <spdk/" -- '*.[ch]' > scripts/includes.log || true
	if [ -s scripts/includes.log ]; then
		echo "Incorrect #include syntax. #includes of spdk/ files should use quotes."
		cat scripts/includes.log
		rc=1
	else
		echo " OK"
	fi
	rm -f scripts/includes.log

	return $rc
}

function check_python_style() {
	local rc=0

	if hash pycodestyle 2> /dev/null; then
		PEP8=pycodestyle
	elif hash pep8 2> /dev/null; then
		PEP8=pep8
	fi

	if [ -n "${PEP8}" ]; then
		echo -n "Checking Python style..."

		PEP8_ARGS+=" --max-line-length=140"

		error=0
		git ls-files '*.py' | xargs -P$(nproc) -n1 $PEP8 $PEP8_ARGS > pep8.log || error=1
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

	return $rc
}

function get_bash_files() {
	local sh shebang

	mapfile -t sh < <(git ls-files '*.sh')
	mapfile -t shebang < <(git grep -l '^#!.*bash')

	printf '%s\n' "${sh[@]}" "${shebang[@]}" | sort -u
}

function check_bash_style() {
	local rc=0

	# find compatible shfmt binary
	shfmt_bins=$(compgen -c | grep '^shfmt' || true)
	for bin in $shfmt_bins; do
		if version_lt "$("$bin" --version)" "3.1.0"; then
			shfmt=$bin
			break
		fi
	done

	if [ -n "$shfmt" ]; then
		shfmt_cmdline=() sh_files=()

		mapfile -t sh_files < <(get_bash_files)

		if ((${#sh_files[@]})); then
			printf 'Checking .sh formatting style...'

			shfmt_cmdline+=(-i 0)     # indent_style = tab|indent_size = 0
			shfmt_cmdline+=(-bn)      # binary_next_line = true
			shfmt_cmdline+=(-ci)      # switch_case_indent = true
			shfmt_cmdline+=(-ln bash) # shell_variant = bash (default)
			shfmt_cmdline+=(-d)       # diffOut - print diff of the changes and exit with != 0
			shfmt_cmdline+=(-sr)      # redirect operators will be followed by a space

			diff=${output_dir:-$PWD}/$shfmt.patch

			# Explicitly tell shfmt to not look for .editorconfig. .editorconfig is also not looked up
			# in case any formatting arguments has been passed on its cmdline.
			if ! SHFMT_NO_EDITORCONFIG=true "$shfmt" "${shfmt_cmdline[@]}" "${sh_files[@]}" > "$diff"; then
				# In case shfmt detects an actual syntax error it will write out a proper message on
				# its stderr, hence the diff file should remain empty.
				if [[ -s $diff ]]; then
					diff_out=$(< "$diff")
				fi

				cat <<- ERROR_SHFMT

					* Errors in style formatting have been detected.
					${diff_out:+* Please, review the generated patch at $diff

					# _START_OF_THE_DIFF

					${diff_out:-ERROR}

					# _END_OF_THE_DIFF
					}

				ERROR_SHFMT
				rc=1
			else
				rm -f "$diff"
				printf ' OK\n'
			fi
		fi
	else
		echo "shfmt not detected, Bash style formatting check is skipped"
	fi

	return $rc
}

function check_bash_static_analysis() {
	local rc=0

	if hash shellcheck 2> /dev/null; then
		echo -n "Checking Bash static analysis with shellcheck..."

		shellcheck_v=$(shellcheck --version | grep -P "version: [0-9\.]+" | cut -d " " -f2)

		# SHCK_EXCLUDE contains a list of all of the spellcheck errors found in SPDK scripts
		# currently. New errors should only be added to this list if the cost of fixing them
		# is deemed too high. For more information about the errors, go to:
		# https://github.com/koalaman/shellcheck/wiki/Checks
		# Error descriptions can also be found at: https://github.com/koalaman/shellcheck/wiki
		# SPDK fails some error checks which have been deprecated in later versions of shellcheck.
		# We will not try to fix these error checks, but instead just leave the error types here
		# so that we can still run with older versions of shellcheck.
		SHCK_EXCLUDE="SC1117"
		# SPDK has decided to not fix violations of these errors.
		# We are aware about below exclude list and we want this errors to be excluded.
		# SC1083: This {/} is literal. Check expression (missing ;/\n?) or quote it.
		# SC1090: Can't follow non-constant source. Use a directive to specify location.
		# SC1091: Not following: (error message here)
		# SC2001: See if you can use ${variable//search/replace} instead.
		# SC2010: Don't use ls | grep. Use a glob or a for loop with a condition to allow non-alphanumeric filenames.
		# SC2015: Note that A && B || C is not if-then-else. C may run when A is true.
		# SC2016: Expressions don't expand in single quotes, use double quotes for that.
		# SC2034: foo appears unused. Verify it or export it.
		# SC2046: Quote this to prevent word splitting.
		# SC2086: Double quote to prevent globbing and word splitting.
		# SC2119: Use foo "$@" if function's $1 should mean script's $1.
		# SC2120: foo references arguments, but none are ever passed.
		# SC2128: Expanding an array without an index only gives the first element.
		# SC2148: Add shebang to the top of your script.
		# SC2153: Possible Misspelling: MYVARIABLE may not be assigned, but MY_VARIABLE is.
		# SC2154: var is referenced but not assigned.
		# SC2164: Use cd ... || exit in case cd fails.
		# SC2174: When used with -p, -m only applies to the deepest directory.
		# SC2178: Variable was used as an array but is now assigned a string.
		# SC2206: Quote to prevent word splitting/globbing,
		#         or split robustly with mapfile or read -a.
		# SC2207: Prefer mapfile or read -a to split command output (or quote to avoid splitting).
		# SC2223: This default assignment may cause DoS due to globbing. Quote it.
		SHCK_EXCLUDE="$SHCK_EXCLUDE,SC1083,SC1090,SC1091,SC2010,SC2015,SC2016,SC2034,SC2046,SC2086,\
SC2119,SC2120,SC2128,SC2148,SC2153,SC2154,SC2164,SC2174,SC2178,SC2001,SC2206,SC2207,SC2223"

		SHCK_FORMAT="tty"
		SHCK_APPLY=false
		SHCH_ARGS="-e $SHCK_EXCLUDE -f $SHCK_FORMAT"

		if ge "$shellcheck_v" 0.4.0; then
			SHCH_ARGS+=" -x"
		else
			echo "shellcheck $shellcheck_v detected, recommended >= 0.4.0."
		fi

		get_bash_files | xargs -P$(nproc) -n1 shellcheck $SHCH_ARGS &> shellcheck.log
		if [[ -s shellcheck.log ]]; then
			echo " Bash shellcheck errors detected!"

			cat shellcheck.log
			if $SHCK_APPLY; then
				git apply shellcheck.log
				echo "Bash errors were automatically corrected."
				echo "Please remember to add the changes to your commit."
			fi
			rc=1
		else
			echo " OK"
		fi
		rm -f shellcheck.log
	else
		echo "You do not have shellcheck installed so your Bash static analysis is not being performed!"
	fi

	return $rc
}

function check_changelog() {
	local rc=0

	# Check if any of the public interfaces were modified by this patch.
	# Warn the user to consider updating the changelog any changes
	# are detected.
	echo -n "Checking whether CHANGELOG.md should be updated..."
	staged=$(git diff --name-only --cached .)
	working=$(git status -s --porcelain --ignore-submodules | grep -iv "??" | awk '{print $2}')
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

	return $rc
}

function check_json_rpc() {
	local rc=0

	echo -n "Checking that all RPCs are documented..."
	while IFS='"' read -r _ rpc _; do
		if ! grep -q "^### $rpc" doc/jsonrpc.md; then
			echo "Missing JSON-RPC documentation for ${rpc}"
			rc=1
		fi
	done < <(git grep -h -E "^SPDK_RPC_REGISTER\(" ':!test/*')

	if [ $rc -eq 0 ]; then
		echo " OK"
	fi
	return $rc
}

function check_markdown_format() {
	local rc=0

	if hash mdl 2> /dev/null; then
		echo -n "Checking markdown files format..."
		mdl -g -s $rootdir/mdl_rules.rb . > mdl.log || true
		if [ -s mdl.log ]; then
			echo " Errors in .md files detected:"
			cat mdl.log
			rc=1
		else
			echo " OK"
		fi
		rm -f mdl.log
	else
		echo "You do not have markdownlint installed so .md files not being checked!"
	fi

	return $rc
}

function check_rpc_args() {
	local rc=0

	echo -n "Checking rpc.py argument option names..."
	grep add_argument scripts/rpc.py | $GNU_GREP -oP "(?<=--)[a-z0-9\-\_]*(?=\')" | grep "_" > badargs.log

	if [[ -s badargs.log ]]; then
		echo "rpc.py arguments with underscores detected!"
		cat badargs.log
		echo "Please convert the underscores to dashes."
		rc=1
	else
		echo " OK"
	fi
	rm -f badargs.log
	return $rc
}

rc=0

check_permissions || rc=1
check_c_style || rc=1

GIT_VERSION=$(git --version | cut -d' ' -f3)

if version_lt "1.9.5" "${GIT_VERSION}"; then
	# git <1.9.5 doesn't support pathspec magic exclude
	echo " Your git version is too old to perform all tests. Please update git to at least 1.9.5 version..."
	exit $rc
fi

check_comment_style || rc=1
check_markdown_format || rc=1
check_spaces_before_tabs || rc=1
check_trailing_whitespace || rc=1
check_forbidden_functions || rc=1
check_cunit_style || rc=1
check_eof || rc=1
check_posix_includes || rc=1
check_naming_conventions || rc=1
check_include_style || rc=1
check_python_style || rc=1
check_bash_style || rc=1
check_bash_static_analysis || rc=1
check_changelog || rc=1
check_json_rpc || rc=1
check_rpc_args || rc=1

exit $rc
