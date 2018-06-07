INIT=0
while getopts ":i" opt; do
	case ${opt} in
	i) INIT=1
	;;
	\?) echo "usage: $(basename $0) [-i]"
	exit
	;;
	esac
done

NR_DEV=4
NR_PU=4
NR_CHK=32
NR_SECT=4096 # per chunk
SZ_SECT=4096
SZ_META=16
SZ_CHK=$((NR_SECT * SZ_SECT))

# struct LnvmChunkState 32
# lnvm_init_meta() 20
# https://github.com/OpenChannelSSD/linux.git (for-4.18/core)
LINUXVMFILE=/var/lib/libvirt/images/centos7.qcow2

unset NVME_OPT
if ((INIT == 1)); then
	rm -f /tmp/*.qemu /tmp/blknvme?
fi

for ((i = 0; i < NR_DEV; i++)); do
	if ((INIT == 1)); then
		dd if=/dev/zero of=/tmp/blknvme${i} bs=${SZ_CHK} count=$((NR_PU * NR_CHK))
		dd if=/dev/zero of=/tmp/chk${i}.qemu bs=32 count=$((NR_PU * NR_CHK))
		dd if=/dev/zero of=/tmp/meta${i}.qemu bs=20 count=$((NR_PU * NR_CHK * NR_SECT))
	fi

	OPT=(
		lchunktable=/tmp/chk${i}.qemu
		lmetadata=/tmp/meta${i}.qemu
		lnum_pu=${NR_PU}
		ldebug=1
	)
	IFS=, eval 'LNVMOPT="${OPT[*]}"'
	NVME_OPT+="-drive file=/tmp/blknvme${i},if=none,id=mynvme${i} "
	NVME_OPT+="-device nvme,drive=mynvme${i},serial=deadbeef${i},${LNVMOPT} "
done
echo ${NVME_OPT}

# qemu-nvme ocssd2
qemu-system-x86_64 -m 8G -smp 4 -cpu host,migratable=off --enable-kvm \
-hda ${LINUXVMFILE} ${NVME_OPT} \
-device e1000,netdev=net0 -netdev user,id=net0,hostfwd=tcp::5555-:22 \
-virtfs local,path=/github,security_model=none,mount_tag=github &
