#!/bin/sh -e

# create_vbox.sh
#
# Creates a virtual box with vagrant in the $PWD.
#
# This script creates a subdirectory called $PWD/<distro> and copies the Vagrantfile
# into that directory before running 'vagrant up'

VAGRANT_TARGET="$PWD"

DIR="$( cd "$( dirname $0 )" && pwd )"
SPDK_DIR="$( cd "${DIR}/../../" && pwd )"

# The command line help
display_help() {
	echo
	echo " Usage: ${0##*/} [-n <num-cpus>] [-s <ram-size>] [-x <http-proxy>] [-hvr] <distro>"
	echo
	echo "  distro = <centos7 | ubuntu16 | ubuntu18 | fedora26 | fedora27 | freebsd11> "
	echo
	echo "  -s <ram-size> in kb       default: ${SPDK_VAGRANT_VMRAM}"
	echo "  -n <num-cpus> 1 to 4      default: ${SPDK_VAGRANT_VMCPU}"
	echo "  -x <http-proxy>           default: \"${SPDK_VAGRANT_HTTP_PROXY}\""
	echo "  -r dry-run"
	echo "  -h help"
	echo "  -v verbose"
        echo
	echo " Examples:"
	echo
	echo "  $0 -x http://user:password@host:port fedora27"
	echo "  $0 -s 2048 -n 2 ubuntu16"
	echo "  $0 -rv freebsd"
	echo "  $0 fedora26 "
	echo
}

# Set up vagrant proxy. Assumes git-bash on Windows
# https://stackoverflow.com/questions/19872591/how-to-use-vagrant-in-a-proxy-environment
SPDK_VAGRANT_HTTP_PROXY=""

VERBOSE=0
HELP=0
DRY_RUN=0
SPDK_VAGRANT_DISTRO="distro"
SPDK_VAGRANT_VMCPU=4
SPDK_VAGRANT_VMRAM=4096
OPTIND=1

while getopts ":n:s:x:vrh" opt; do
	case "${opt}" in
		x)
			http_proxy=$OPTARG
			https_proxy=$http_proxy
			SPDK_VAGRANT_HTTP_PROXY="${http_proxy}"
		;;
		n)
			SPDK_VAGRANT_VMCPU=$OPTARG
		;;
		s)
			SPDK_VAGRANT_VMRAM=$OPTARG
		;;
		v)
			VERBOSE=1
		;;
		r)
			DRY_RUN=1
		;;
		h)
			display_help >&2
			exit 0
		;;
		*)
			echo "  Invalid argument: -$OPTARG" >&2
			echo "  Try: \"$0 -h\"" >&2
			exit 1
		;;
	esac
done

shift "$((OPTIND-1))"   # Discard the options and sentinel --

SPDK_VAGRANT_DISTRO="$@"

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
		echo "  Invalid argument \"${SPDK_VAGRANT_DISTRO}\""
		echo "  Try: \"$0 -h\"" >&2
		exit 1
	;;
esac

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
	echo SPDK_VAGRANT_HTTP_PROXY=$SPDK_VAGRANT_HTTP_PROXY
	echo
fi

export SPDK_VAGRANT_HTTP_PROXY
export SPDK_VAGRANT_VMCPU
export SPDK_VAGRANT_VMRAM
export SPDK_DIR

if [ ${DRY_RUN} = 1 ]; then
	echo "Environemnt Variables"
	printenv SPDK_VAGRANT_DISTRO
	printenv SPDK_VAGRANT_VMRAM
	printenv SPDK_VAGRANT_VMCPU
	printenv SPDK_VAGRANT_HTTP_PROXY
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
	if [ ! -z "${http_proxy}" ]; then
		export http_proxy
		export https_proxy
		if vagrant plugin list | grep -q vagrant-proxyconf; then
			echo "vagrant-proxyconf already installed... skipping"
		else
			vagrant plugin install vagrant-proxyconf
		fi
	fi
	vagrant up
	echo ""
	echo "  SUCCESS!"
	echo ""
	echo "  cd to ${SPDK_VAGRANT_DISTRO} and type \"vagrant ssh\" to use."
	echo "  Use vagrant \"suspend\" and vagrant \"resume\" to stop and start."
	echo "  Use vagrant \"destroy\" followed by \"rm -rf ${SPDK_VAGRANT_DISTRO}\" to destroy all trace of vm."
	echo ""
fi
