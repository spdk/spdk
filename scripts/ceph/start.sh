#!/usr/bin/env bash
# create mon

set -x
set -e

script_dir=$(readlink -f $(dirname $0))

base_dir=/var/tmp/ceph
mon_ip=$1
mon_dir=${base_dir}/mon.a
pid_dir=${base_dir}/pid
ceph_conf=${base_dir}/ceph.conf
mnt_dir=${base_dir}/mnt
image=/var/tmp/ceph_raw.img
dev=/dev/loop200

modprobe loop
umount ${dev}p2 || true
losetup -d $dev || true

# partition osd
if [ -d $base_dir ]; then
	rm -rf $base_dir
fi
mkdir ${base_dir}
cp ${script_dir}/ceph.conf $ceph_conf

if [ ! -e $image ]; then
	fallocate -l 4G $image
fi

mknod ${dev} b 7 200 || true
losetup ${dev} ${image} || true

PARTED="parted -s"
SGDISK="sgdisk"

echo "Partitioning ${dev}"
${PARTED} ${dev} mktable gpt
sleep 2

${PARTED} ${dev} mkpart primary 0% 2GiB
${PARTED} ${dev} mkpart primary 2GiB 100%

partno=0
echo "Setting name on ${dev}"
${SGDISK} -c 1:osd-device-${partno}-journal ${dev}
${SGDISK} -c 2:osd-device-${partno}-data ${dev}
kpartx ${dev}

# later versions of ceph-12 have a lot of changes, to compatible with the new
# version of ceph-deploy.
ceph_version=$(ceph -v | awk '{print $3}')
ceph_maj=${ceph_version%%.*}
if [ $ceph_maj -gt 12 ]; then
	update_config=true
	rm -f /var/log/ceph/ceph-mon.a.log || true
	set_min_mon_release="--set-min-mon-release 14"
	ceph_osd_extra_config="--check-needs-journal --no-mon-config"
else
	update_config=false
	set_min_mon_release=""
	ceph_osd_extra_config=""
fi

# prep osds

mnt_pt=${mnt_dir}/osd-device-0-data
mkdir -p ${mnt_pt}
mkfs.xfs -f /dev/disk/by-partlabel/osd-device-0-data
mount /dev/disk/by-partlabel/osd-device-0-data ${mnt_pt}
cat << EOL >> $ceph_conf
osd data = ${mnt_pt}
osd journal = /dev/disk/by-partlabel/osd-device-0-journal

# add mon address
[mon.a]
mon addr = ${mon_ip}:12046
EOL

# create mon
rm -rf "${mon_dir:?}/"*
mkdir -p ${mon_dir}
mkdir -p ${pid_dir}
rm -f /etc/ceph/ceph.client.admin.keyring

ceph-authtool --create-keyring --gen-key --name=mon. ${base_dir}/keyring --cap mon 'allow *'
ceph-authtool --gen-key --name=client.admin --cap mon 'allow *' --cap osd 'allow *' --cap mds 'allow *' --cap mgr 'allow *' ${base_dir}/keyring

monmaptool --create --clobber --add a ${mon_ip}:12046 --print ${base_dir}/monmap $set_min_mon_release

sh -c "ulimit -c unlimited && exec ceph-mon --mkfs -c ${ceph_conf} -i a --monmap=${base_dir}/monmap --keyring=${base_dir}/keyring --mon-data=${mon_dir}"

if [ $update_config = true ]; then
	sed -i 's/mon addr = /mon addr = v2:/g' $ceph_conf
fi

cp ${base_dir}/keyring ${mon_dir}/keyring

cp $ceph_conf /etc/ceph/ceph.conf

cp ${base_dir}/keyring /etc/ceph/keyring
cp ${base_dir}/keyring /etc/ceph/ceph.client.admin.keyring
chmod a+r /etc/ceph/ceph.client.admin.keyring

ceph-run sh -c "ulimit -n 16384 && ulimit -c unlimited && exec ceph-mon -c ${ceph_conf} -i a --keyring=${base_dir}/keyring --pid-file=${base_dir}/pid/root@$(hostname).pid --mon-data=${mon_dir}" || true

# after ceph-mon creation, ceph -s should work.
if [ $update_config = true ]; then
	# start to get whole log.
	ceph-conf --name mon.a --show-config-value log_file

	# add fsid to ceph config file.
	fsid=$(ceph -s | grep id | awk '{print $2}')
	sed -i 's/perf = true/perf = true\n\tfsid = '$fsid' \n/g' $ceph_conf

	# unify the filesystem with the old versions.
	sed -i 's/perf = true/perf = true\n\tosd objectstore = filestore\n/g' $ceph_conf
	cat ${ceph_conf}
fi

# create osd

i=0

mkdir -p ${mnt_dir}

uuid=$(uuidgen)
ceph -c ${ceph_conf} osd create ${uuid} $i
ceph-osd -c ${ceph_conf} -i $i --mkfs --mkkey --osd-uuid ${uuid} ${ceph_osd_extra_config}
ceph -c ${ceph_conf} osd crush add osd.${i} 1.0 host=$(hostname) root=default
ceph -c ${ceph_conf} -i ${mnt_dir}/osd-device-${i}-data/keyring auth add osd.${i} osd "allow *" mon "allow profile osd" mgr "allow *"

# start osd
pkill -9 ceph-osd || true
sleep 2

mkdir -p ${pid_dir}
env -i TCMALLOC_MAX_TOTAL_THREAD_CACHE_BYTES=134217728 ceph-osd -c ${ceph_conf} -i 0 --pid-file=${pid_dir}/ceph-osd.0.pid
