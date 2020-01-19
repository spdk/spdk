#!/usr/bin/env bash
SYSTEM=$(uname -s)
size="1024M"
nvme_disk="/var/lib/libvirt/images/nvme_disk.img"
type="nvme"

# shellcheck source=scripts/common.sh
source "$(dirname "$0")/../common.sh"

function usage() {
	echo "Usage: ${0##*/} [-s <disk_size>] [-n <backing file name>]"
	echo "-s <disk_size> with postfix e.g. 2G        default: 1024M"
	echo "                                    for OCSSD default: 9G"
	echo "-n <backing file name>        backing file path with name"
	echo "           default: /var/lib/libvirt/images/nvme_disk.img"
	echo "-t <type>                  default: nvme available: ocssd"
}

while getopts "s:n:t:p:h-:" opt; do
	case "${opt}" in
		-)
			echo "  Invalid argument: $OPTARG"
			usage
			exit 1
		;;
		p)
			provider=$OPTARG
		;;
		s)
			size=$OPTARG
		;;
		n)
			nvme_disk=$OPTARG
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

# libvirt's aa helper will ignore everything that comes through the qemu's
# commandline, including the $nvme_disk image which won't be included in
# the profile that is created for every qemu instance. To workaround it,
# check if apparmor is in place and if libvirtd is confined by it, patch
# libvirt-qemu (it's sourced by every qemu profile) so it includes the
# $nvme_disk.

if [[ $provider == libvirt ]] && is_apparmor; then
	files_to_app=()
	# FIXME: Read the path from TEMPLATE.qemu first?
	libvirt_apparmor=/etc/apparmor.d/abstractions/libvirt-qemu
	if [[ -f $libvirt_apparmor ]]; then
		files_to_app+=("$nvme_disk")
		if [[ $type == ocssd ]]; then
			files_to_app+=("/var/lib/libvirt/images/ocssd*")
		fi

		patch=$(printf '  %s rwk,\n' "${files_to_app[@]}")

		if [[ $(<"$libvirt_apparmor") != *"$patch"* ]] && cp -p "$libvirt_apparmor"{,.spdk}; then
			printf '* AppArmor detected, patching %s (%s)\n' \
			  "$libvirt_apparmor" "${files_to_app[*]}"
			printf '\n%s\n' "$patch" >>"$libvirt_apparmor" || printf '* Failure!\n'
		fi 2>/dev/null
	fi
fi

if [ ! "${SYSTEM}" = "FreeBSD" ]; then
	WHICH_OS=$(lsb_release -i | awk '{print $3}')
	case $type in
		"nvme")
			qemu-img create -f raw $nvme_disk ${size}
		;;
		"ocssd")
			if [ ${size} == "1024M" ]; then
				size="9G"
			fi
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
