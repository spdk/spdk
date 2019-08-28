#!/usr/bin/env bash
SYSTEM=$(uname -s)
size="1024M"
name="nvme_disk.img"
type="nvme"

function usage() {
	echo "Usage: ${0##*/} [-s <disk_size>] [-n <backing file name>]"
	echo "-s <disk_size> with postfix e.g. 2G        default: 1024M"
	echo "-n <backing file name>             default: nvme_disk.img"
	echo "-t <type>                  default: nvme available: ocssd"
}

while getopts "s:n:t:h-:" opt; do
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
		t)
			type=$OPTARG
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
	case $type in
		"nvme")
			qemu-img create -f raw $nvme_disk ${size}
		;;
		"ocssd")
			fallocate -l ${size} $nvme_disk
			touch /var/lib/libvirt/images/ocssd_md
		;;
		*)
			echo "We support only nvme and ocssd disks types"
			exit 1
		;;
	esac
	#Change SE Policy on Fedora
	if [ $WHICH_OS == "Fedora" ]; then
		sudo chcon -t svirt_image_t $nvme_disk
	fi

	chmod 777 $nvme_disk
	chown qemu:qemu $nvme_disk
fi
