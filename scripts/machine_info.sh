
#!/usr/bin/env bash
# Please run this script as root.

set -e
trap 'set +e; trap - ERR; echo "Error!"; exit 1;' ERR

scriptsdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $scriptsdir/..)

uname_a=$(uname -a)
echo "Uname -a: $uname_a"

if [ -s /etc/redhat-release ]; then
	:
elif [ -f /etc/debian_version ]; then
        # Includes Ubuntu, Debian
	:
elif [ -f /etc/SuSE-release ]; then
	:
elif [ $(uname -s) = "FreeBSD" ] ; then
	:
else
	echo "Machine info: unknown system type."
fi
astyle_v=$(astyle -V)
echo "Astyle version: $astyle_v"
gcc_ver=$(gcc -dumpversion)
echo "GCC version: $gcc_ver"
nasm_ver=$(nasm -v | sed 's/[^0-9]*//g' | awk '{print substr ($0, 0, 5)}')
echo "Nasm version: $nasm_ver"
