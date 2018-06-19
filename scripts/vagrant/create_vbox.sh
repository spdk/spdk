#!/bin/bash -e

# create_vbox.sh
#
# Creates a virtual box with vagrant in the $PWD.
#
# This script creates a subdirectory called $PWD/<distro> and copies the Vagrantfile
# into that directory before running 'vagrant up'

VAGRANT_TARGET="$PWD"

# The linux readlink -f utility is not supported with BSD or OSX so we need a different
# soluiton for obtaining the location of the spdk repository. See the following notes:
#
# https://stackoverflow.com/questions/59895/getting-the-source-directory-of-a-bash-script-from-within/246128#246128

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SPDK_DIR="$( cd "${DIR}/../../" && pwd )"

# The command line help
display_help() {
	echo
	echo "Usage: $0 -d <distro>  [-n <num-cpu>] [-s <ram-size>] [-h]  [-v] | [-r]"
	echo
	echo "  -d | --distro <centos7 | ubuntu16 | ubuntu18 | fedora26 | fedora27 | freebsd11> "
	echo
	echo "  -s | --vram-size <bytes> default: ${SPDK_VAGRANT_VMRAM}"
	echo "  -n | --vmcpu-num <int>   default: ${SPDK_VAGRANT_VMCPU}"
	echo "  -v | --verbose"
	echo "  -r | --dry-run"
	echo "  -h | --help"
        echo
	echo "  Examples:"
	echo
	echo "      create_vbox.sh --distro ubuntu16 -s 2048 -n 2 "
	echo "      create_vbox.sh -d freebsd -r -v "
	echo "      create_vbox.sh -d fedora26 "
	echo
}

OPTS=`getopt -o vrhd:n:s: --long verbose,dry-run,help,distro:,vmcpu-num:,vram-size: -n 'parse-options' -- "$@"`

if [ $? != 0 ] ; then echo "Failed parsing options. Try $0 -h" >&2 ; exit 1 ; fi

#echo "$OPTS"
eval set -- "$OPTS"

VERBOSE=0
HELP=0
DRY_RUN=0
SPDK_VAGRANT_DISTRO="distro"
SPDK_VAGRANT_VMCPU=4
SPDK_VAGRANT_VMRAM=4096

while true; do
  case "$1" in
    -v | --verbose ) VERBOSE=1; shift ;;
    -h | --help )    HELP=1; shift ;;
    -r | --dry-run ) DRY_RUN=1; shift ;;
    -d | --distro ) SPDK_VAGRANT_DISTRO="$2"; shift; shift ;;
    -n | --vmcpu-num ) SPDK_VAGRANT_VMCPU="$2"; shift; shift ;;
    -s | --vram-size ) SPDK_VAGRANT_VMRAM="$2"; shift; shift ;;
    -- ) shift; break ;;
    * ) break ;;
  esac
done

if [ ${VERBOSE} = 1 ]; then
	echo
	echo DIR=${DIR}
	echo SPDK_DIR=${SPDK_DIR}
	echo VAGRANT_TARGET=${VAGRANT_TARGET}
	echo HELP=$HELP
	echo DRY_RUN=$DRY_RUN
	echo SPDK_VAGRANT_DISTRO=$SPDK_VAGRANT_DISTRO
	echo SPDK_VAGRANT_VMCPU=$SPDK_VAGRANT_VMCPU
	echo SPDK_VAGRANT_VMRAM=$SPDK_VAGRANT_VMRAM
	echo
fi

if [ ${HELP} = 1 ]; then
	display_help
	exit 0
fi

case "$SPDK_VAGRANT_DISTRO" in
	centos7)
		export SPDK_VAGRANT_DISTRO
	;;
	ubuntu16)
		export SPDK_VAGRANT_DISTRO
	;;
	ubuntu18)
		export SPDK_VAGRANT_DISTRO
	;;
	fedora26)
		export SPDK_VAGRANT_DISTRO
	;;
	fedora27)
		export SPDK_VAGRANT_DISTRO
	;;
        freebsd11)
                export SPDK_VAGRANT_DISTRO
        ;;
	*)
		echo
		echo "Invalid argument \"${SPDK_VAGRANT_DISTRO}\""
		display_help
		exit 1
	;;
esac

export SPDK_VAGRANT_VMCPU
export SPDK_VAGRANT_VMRAM
export SPDK_DIR

if [ ${DRY_RUN} = 1 ]; then
	echo "Set Environemnt Variables"
	printenv SPDK_VAGRANT_DISTRO
	printenv SPDK_VAGRANT_VMRAM
	printenv SPDK_VAGRANT_VMCPU
	printenv SPDK_DIR
fi

if [ -d "${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}" ]; then
	echo "Error: ${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO} already exists!"
	exit 1
fi

if [ ${DRY_RUN} != 1 ]; then
	mkdir -vp "${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}"
	cp ${DIR}/Vagrantfile ${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}
	pushd "${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}"
	vagrant up
	echo ""
	echo "  SUCCESS!"
	echo ""
	echo "  cd to ${SPDK_VAGRANT_DISTRO} and type \"vagrant ssh\" to use."
	echo "  Use vagrant \"suspend\" and vagrant \"resume\" to stop and start."
	echo "  Use vagrant \"destroy\" followed by \"rm -rf ${SPDK_VAGRANT_DISTRO}\" to destroy all trace of vm."
	echo ""
fi
