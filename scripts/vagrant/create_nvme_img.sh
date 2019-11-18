#!/usr/bin/env bash
SYSTEM=$(uname -s)
size="1024M"
name="nvme_disk.img"

function usage() {
	echo "Usage: ${0##*/} [-s <disk_size>] [-n <backing file name>]"
	echo "-s <disk_size> with postfix e.g. 2G        default: 1024M"
	echo "-n <backing file name>             default: nvme_disk.img"
}

while getopts "s:n:h-:" opt; do
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
			name=$OPTARG
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

if [ ! "${SYSTEM}" = "FreeBSD" ]; then
	WHICH_OS=$(lsb_release -i | awk '{print $3}')
	nvme_disk="/var/lib/libvirt/images/$name"

	qemu-img create -f raw "$nvme_disk" "${size}"
	#Change SE Policy on Fedora
	if [ "$WHICH_OS" == "Fedora" ]; then
		sudo chcon -t svirt_image_t "$nvme_disk"
	fi

	chmod 777 "$nvme_disk"
	chown qemu:qemu "$nvme_disk"
fi
