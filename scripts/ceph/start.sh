#!/usr/bin/env bash
# create mon

set -x
set -e

script_dir=$(readlink -f $(dirname $0))

base_dir=/var/tmp/ceph
mon_ip=$1
mon_dir=${base_dir}/mon.a/
pid_dir=${base_dir}/pid
ceph_conf=${base_dir}/ceph.conf
mnt_dir=${base_dir}/mnt
dev_backend=/dev/ceph
image=/var/tmp/ceph_raw.img
dev=/dev/loop200

umount $dev || true
losetup -d $dev_backend || true

# partition osd
if [ -d $base_dir ]; then
	rm -rf $base_dir
fi
mkdir ${base_dir}
cp ${script_dir}/ceph.conf $ceph_conf

if [ ! -e $image ]; then
	fallocate -l 10G $image
fi

mknod ${dev_backend} b 7 200 || true
losetup ${dev_backend} ${image} || true

PARTED="parted -s"
SGDISK="sgdisk"

echo "Partitioning ${dev}"
${PARTED} ${dev} mktable gpt
sleep 2
${PARTED} ${dev} mkpart primary    0%    5GiB
${PARTED} ${dev} mkpart primary   5GiB  100%


partno=0
echo "Setting name on ${dev}"
${SGDISK} -c 1:osd-device-${partno}-journal ${dev}
${SGDISK} -c 2:osd-device-${partno}-data ${dev}
kpartx ${dev}

# prep osds

mnt_pt=${mnt_dir}/osd-device-0-data/
mkdir -p ${mnt_pt}
mkfs.xfs -f /dev/disk/by-partlabel/osd-device-0-data
mount /dev/disk/by-partlabel/osd-device-0-data ${mnt_pt}
echo -e "\tosd data = ${mnt_pt}" >> "$ceph_conf"
echo -e "\tosd journal = /dev/disk/by-partlabel/osd-device-0-journal" >> "$ceph_conf"

# add mon address
echo -e "\t[mon.a]" >> "$ceph_conf"
echo -e "\tmon addr = ${mon_ip}:12046" >> "$ceph_conf"

# create mon
rm -rf ${mon_dir}/*
mkdir -p ${mon_dir}
mkdir -p ${pid_dir}

ceph-authtool --create-keyring --gen-key --name=mon. ${base_dir}/keyring --cap mon 'allow *'
ceph-authtool --gen-key --name=client.admin --set-uid=0 --cap mon 'allow *' --cap osd 'allow *' --cap mds 'allow *' --cap mgr 'allow *' ${base_dir}/keyring

monmaptool --create --clobber --add a ${mon_ip}:12046 --print ${base_dir}/monmap

sh -c "ulimit -c unlimited && exec ceph-mon --mkfs -c ${ceph_conf} -i a --monmap=${base_dir}/monmap --keyring=${base_dir}/keyring --mon-data=${mon_dir}"

cp ${base_dir}/keyring ${mon_dir}/keyring

cp $ceph_conf /etc/ceph/ceph.conf

cp ${base_dir}/keyring /etc/ceph/keyring

ceph-run sh -c "ulimit -n 16384 && ulimit -c unlimited && exec ceph-mon -c ${ceph_conf} -i a --keyring=${base_dir}/keyring --pid-file=${base_dir}/pid/root@`hostname`.pid --mon-data=${mon_dir}" || true

# create osd

i=0

mkdir -p ${mnt_dir}

uuid=`uuidgen`
ceph -c ${ceph_conf} osd create ${uuid} $i
ceph-osd -c ${ceph_conf} -i $i --mkfs --mkkey --osd-uuid ${uuid}
ceph -c ${ceph_conf} osd crush add osd.${i} 1.0 host=`hostname` root=default
ceph -c ${ceph_conf} -i ${mnt_dir}/osd-device-${i}-data/keyring auth add osd.${i} osd "allow *" mon "allow profile osd" mgr "allow"

# start osd
pkill -9 ceph-osd || true
sleep 2

mkdir -p ${pid_dir}
env -i TCMALLOC_MAX_TOTAL_THREAD_CACHE_BYTES=134217728 ceph-osd -c ${ceph_conf} -i 0 --pid-file=${pid_dir}/ceph-osd.0.pid
