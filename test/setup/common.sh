source "$rootdir/test/common/autotest_common.sh"

setup() {
	if [[ $1 == output ]]; then
		"$rootdir/scripts/setup.sh" "${@:2}"
	else
		"$rootdir/scripts/setup.sh" "$@" &> /dev/null
	fi
}
