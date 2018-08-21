#!/usr/bin/env bash

# create_vhost_vm.sh
#
# Creates a virtual machine image for running vhost tests

VAGRANT_TARGET="$PWD"

DIR="$( cd "$( dirname $0 )" && pwd )"
SPDK_DIR="$( cd "${DIR}/../../" && pwd )"

# The command line help
display_help() {
	echo
	echo " Usage: ${0##*/} <distro>"
	echo
	echo "  distro = <ubuntu16 | ubuntu18 | fedora26 | fedora27 | fedora28> "
	echo
	echo "  -x <http-proxy>           default: \"${SPDK_VAGRANT_HTTP_PROXY}\""
	echo "  -o <path>                 directory where created files will be moved to"
	echo "  -h help"
	echo
	echo " Examples:"
	echo
}

SPDK_VAGRANT_DISTRO="$@"

case "$SPDK_VAGRANT_DISTRO" in
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
	*)
		echo "  Invalid argument \"${SPDK_VAGRANT_DISTRO}\""
		echo "  Try: \"$0 -h\"" >&2
		exit 1
	;;
esac

export SPDK_VAGRANT_HTTP_PROXY



mkdir -vp "${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}"
cp ${DIR}/Vagrantfile_vhst ${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}/Vagrantfile

mkdir -vp "${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}/ssh_keys"
ssh-keygen -f "${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}/ssh_keys/spdk_vhost_id_rsa" -N "" -q
export SPDK_VAGRANT_SSH_KEY="${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}/ssh_keys/spdk_vhost_id_rsa"

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
vagrant up --provider=libvirt
vagrant halt
echo ""
echo "  SUCCESS!"
echo ""
