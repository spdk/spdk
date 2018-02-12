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
	err " --lto=[y|n]               Attempt to configure for LTO"

}

CC=cc
CXX=c++
LTO=n

for i in "$@"; do
	case "$i" in
		-h|--help)
			usage
			exit 0
			;;
		--cc=*)
			CC="${i#*=}"
			;;
		--cxx=*)
			CXX="${i#*=}"
			;;
		--lto=*)
			LTO="${i#*=}"
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

CC_TYPE=$($CC -v 2>&1 | grep -o -E '\w+ version' | head -1 | awk '{ print $1 }')
CXX_TYPE=$($CXX -v 2>&1 | grep -o -E '\w+ version' | head -1 | awk '{ print $1 }')
LD_TYPE=$(ld -v 2>&1 | awk '{print $2}')

if [ "$CC_TYPE" != "$CXX_TYPE" ]; then
	err "C compiler is $CC_TYPE but C++ compiler is $CXX_TYPE"
	err "This may result in errors"
fi

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

echo "CC?=$CC"
echo "CXX?=$CXX"
echo "CCAR=$CCAR"
echo "CC_TYPE=$CC_TYPE"
