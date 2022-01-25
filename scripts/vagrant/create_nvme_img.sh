#!/usr/bin/env bash
SYSTEM=$(uname -s)
size="1024M"
nvme_disk="/var/lib/libvirt/images/nvme_disk.img"
preallocation="falloc"

function usage() {
	echo "Usage: ${0##*/} [-s <disk_size>] [-n <backing file name>]"
	echo "-s <disk_size> with postfix e.g. 2G        default: 1024M"
	echo "                                    for OCSSD default: 9G"
	echo "-n <backing file name>        backing file path with name"
	echo "           default: /var/lib/libvirt/images/nvme_disk.img"
	echo "-p <mode>              allowed values:[off, falloc, full]"
	echo "                                          default: falloc"
}

while getopts "s:n:p:t:h-:" opt; do
	case "${opt}" in
		-)
			echo "  Invalid argument: $OPTARG"
			usage
			exit 1
			;;
		s)
			size=$OPTARG
			;;
		n)
			nvme_disk=$OPTARG
			;;
		p)
			preallocation=$OPTARG
			;;
		h)
			usage
			exit 0
			;;
		*)
			echo "  Invalid argument: $OPTARG"
			usage
			exit 1
			;;
	esac
done

if [ "${SYSTEM}" != "Linux" ]; then
	echo "This script supports only Linux OS" >&2
	exit 2
fi

WHICH_OS=$(lsb_release -i | awk '{print $3}')
qemu-img create -f raw "$nvme_disk" -o preallocation="$preallocation" $size

case $WHICH_OS in
	"Fedora")
		qemu_user_group="qemu:qemu"

		# Change SE Policy
		sudo chcon -t svirt_image_t "$nvme_disk"
		;;
	"Ubuntu")
		qemu_user_group="libvirt-qemu:kvm"
		;;
	*)
		# That's just a wild guess for now
		# TODO: needs improvement for other distros
		qemu_user_group="libvirt-qemu:kvm"
		;;
esac

chmod 777 "$nvme_disk"
chown $qemu_user_group "$nvme_disk"
