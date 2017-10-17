#!/usr/bin/env bash
set -x

vhost_name=$(shopt -s nullglob; cd /sys/block; echo vd*)
if [ -z $vhost_name ]; then
	echo "no vhost disk found"
fi

if (( $(lsblk -r -n -o RO -d "/dev/$vhost_name") == 1 )); then
	echo "vhost disk is readonly"
fi

echo -e "n\np\n2\n\n\nw" | fdisk /dev/$vhost_name

echo "$vhost_name"
