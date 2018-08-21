#!/usr/bin/env bash

# create_vhost_vm.sh
#
# Creates a virtual machine image used as a dependency for running vhost tests

set -e

VAGRANT_TARGET="$PWD"

DIR="$( cd "$( dirname $0 )" && pwd )"
SPDK_DIR="$( cd "${DIR}/../../" && pwd )"
USE_SSH_DIR=""
MOVE_TO_DEFAULT_DIR=false
INSTALL_DEPS=false

# The command line help
display_help() {
	echo
	echo " Usage: ${0##*/} <distro>"
	echo
	echo "  distro = <ubuntu16 | ubuntu18> "
	echo
	echo "  --use-ssh-dir=<dir path>    Use existing spdk_vhost_id_rsa keys from specified directory"
	echo "                              for setting up SSH key pair on VM"
	echo "  --install-deps              Install SPDK build dependencies on VM. Needed by some of the"
	echo "                              vhost and vhost initiator tests. Default: false."
	echo "  --move-to-default-dir           Move generated files to default directories used by vhost test scripts."
	echo "                              Default: false."
	echo "  --http-proxy                Default: \"${SPDK_VAGRANT_HTTP_PROXY}\""
	echo "  -h help"
	echo
	echo " Examples:"
	echo
}

while getopts ":h-:" opt; do
	case "${opt}" in
		-)
		case "${OPTARG}" in
			use-ssh-dir=*) USE_SSH_DIR="${OPTARG#*=}" ;;
			move-to-default-dir) MOVE_TO_DEFAULT_DIR=true ;;
			install-deps) INSTALL_DEPS=true ;;
			http-proxy=*)
				http_proxy=$OPTARG
				https_proxy=$http_proxy
				SPDK_VAGRANT_HTTP_PROXY="${http_proxy}"
			;;
			*)
				echo "  Invalid argument -$OPTARG" >&2
				echo "  Try \"$0 -h\"" >&2
				exit 1
				;;
		esac
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
export SPDK_DIR
export SPDK_VAGRANT_HTTP_PROXY
export INSTALL_DEPS


shift "$((OPTIND-1))"   # Discard the options and sentinel --
SPDK_VAGRANT_DISTRO="$@"

case "$SPDK_VAGRANT_DISTRO" in
	ubuntu16)
		export SPDK_VAGRANT_DISTRO
	;;
	ubuntu18)
		export SPDK_VAGRANT_DISTRO
	;;
	*)
		echo "  Invalid argument \"${SPDK_VAGRANT_DISTRO}\""
		echo "  Try: \"$0 -h\"" >&2
		exit 1
	;;
esac

mkdir -vp "${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}"
cp ${DIR}/Vagrantfile_vhost_vm ${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}/Vagrantfile

# Copy or generate SSH keys to the VM
mkdir -vp "${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}/ssh_keys"

if [[ -n $USE_SSH_DIR ]]; then
	cp ${USE_SSH_DIR}/spdk_vhost_id_rsa* "${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}/ssh_keys"
else
	ssh-keygen -f "${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}/ssh_keys/spdk_vhost_id_rsa" -N "" -q
fi
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
VBoxManage setproperty machinefolder "${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}"
vagrant up
vagrant halt
VBoxManage setproperty machinefolder default

# Convert Vbox .vmkd image to qcow2
vmdk_img=$(find ${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO} -name "*.vmdk")
qemu-img convert -f vmdk -O qcow2 ${vmdk_img} ${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}/vhost_vm_image.qcow2

if $MOVE_TO_DEFAULT_DIR; then
	sudo mkdir -p /home/sys_sgsw
	sudo mv -f ${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}/vhost_vm_image.qcow2 /home/sys_sgsw/vhost_vm_image.qcow2
	sudo mv -f ${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}/ssh_keys/spdk_vhost_id_rsa* ~/.ssh/
fi

echo ""
echo "  SUCCESS!"
echo ""
