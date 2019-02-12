#!/usr/bin/env bash

set -e

function err()
{
	echo "$@" >&2
}

function usage()
{
	err "Detect compiler and linker versions, generate mk/cc.mk"
	err ""
	err "Usage: ./detect_cc.sh [OPTION]..."
	err ""
	err "Defaults for the options are specified in brackets."
	err ""
	err "General:"
	err " -h, --help                Display this help and exit"
	err " --cc=path                 C compiler to use"
	err " --cxx=path                C++ compiler to use"
	err " --ld=path                 Linker to use"
	err " --lto=[y|n]               Attempt to configure for LTO"
}



for i in "$@"; do
	case "$i" in
		-h|--help)
			usage
			exit 0
			;;
		--cc=*)
			if [[ -n "${i#*=}" ]]; then
				CC="${i#*=}"
			fi
			;;
		--cxx=*)
			if [[ -n "${i#*=}" ]]; then
				CXX="${i#*=}"
			fi
			;;
		--lto=*)
			if [[ -n "${i#*=}" ]]; then
				LTO="${i#*=}"
			fi
			;;
		--ld=*)
			if [[ -n "${i#*=}" ]]; then
				LD="${i#*=}"
			fi
			;;
		--)
			break
			;;
		*)
			err "Unrecognized option $i"
			usage
			exit 1
	esac
done

: ${CC=cc}
: ${CXX=c++}
: ${LD=ld}
: ${LTO=n}

CC_TYPE=$($CC -v 2>&1 | grep -o -E '\w+ version' | head -1 | awk '{ print $1 }')
CXX_TYPE=$($CXX -v 2>&1 | grep -o -E '\w+ version' | head -1 | awk '{ print $1 }')
if [ "$CC_TYPE" != "$CXX_TYPE" ]; then
	err "C compiler is $CC_TYPE but C++ compiler is $CXX_TYPE"
	err "This may result in errors"
fi

LD_TYPE=$($LD --version 2>&1 | head -n1 | awk '{print $1, $2}')
case "$LD_TYPE" in
	"GNU ld"*)
		LD_TYPE=bfd
		;;
	"GNU gold"*)
		LD_TYPE=gold
		;;
	"LLD"*)
		LD_TYPE=lld
		;;
	*)
		err "Unsupported linker: $LD"
		exit 1
esac

CCAR="ar"
if [ "$LTO" = "y" ]; then
	if [ "$CC_TYPE" = "clang" ]; then
		if [ "$LD_TYPE" != "gold" ]; then
			err "Using LTO with clang requires the gold linker."
			exit 1
		fi
		CCAR="llvm-ar"
	else
		CCAR="gcc-ar"
	fi
fi

function set_default() {
	echo "ifeq (\$(origin $1),default)"
	echo "$1 = $2"
	echo "endif"
	echo ""
}

set_default CC $CC
set_default CXX $CXX
set_default LD $LD

echo "CCAR=$CCAR"
echo "CC_TYPE=$CC_TYPE"
echo "LD_TYPE=$LD_TYPE"
