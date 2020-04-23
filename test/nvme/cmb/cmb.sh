#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")
source "$rootdir/test/common/autotest_common.sh"

shopt -s extglob nullglob

decode_cmb_regs() {
	local cmb=$1
	local cmbs
	local cmbl

	mapfile -t cmb < "$cmb"
	cmb=("${cmb[@]##* }")

	cmbl=0${cmb[0]}
	cmbs=0${cmb[1]}

	local szu szu_unit
	szu[0]=4 szu_unit[0]=KiB
	szu[1]=64 szu_unit[1]=KiB
	szu[2]=1 szu_unit[2]=MiB
	szu[3]=16 szu_unit[3]=MiB
	szu[4]=256 szu_unit[4]=MiB
	szu[5]=4 szu_unit[5]=GiB
	szu[6]=64 szu_unit[6]=GiB

	local bool
	bool[0]="not set"
	bool[1]="set"

	local size size_m
	local ofst bir

	size=$(((cmbs >> 12) & 0x0fffff))
	size_m=$(((cmbs >> 8) & 0xf))
	((size *= szu[size_m]))

	size="$size ${szu_unit[size_m]}"
	ofst=$(printf '0x%x' $(((cmbl >> 12) & 0x0fffff)))
	bir=$(printf '0x%x' $((cmbl & 0x7)))

	cat <<- CMBSZ
		  SZ:    $size
		  SZU:   ${szu[size_m]} ${szu_unit[size_m]}
		  WDS:   ${bool[cmbs & 1 << 4 ? 1 : 0]}
		  RDS:   ${bool[cmbs & 1 << 3 ? 1 : 0]}
		  LISTS: ${bool[cmbs & 1 << 2 ? 1 : 0]}
		  CQS:   ${bool[cmbs & 1 << 1 ? 1 : 0]}
		  SQS:   ${bool[cmbs & 1 << 0 ? 1 : 0]}

	CMBSZ

	cat <<- CMBLOC
		  OFST:    $ofst
		  CQDA:    ${bool[cmbl & 1 << 8 ? 1 : 0]}
		  CDMMMS:  ${bool[cmbl & 1 << 7 ? 1 : 0]}
		  CDPCILS: ${bool[cmbl & 1 << 6 ? 1 : 0]}
		  CDPMLS:  ${bool[cmbl & 1 << 5 ? 1 : 0]}
		  CQPDS:   ${bool[cmbl & 1 << 4 ? 1 : 0]}
		  CQMMS:   ${bool[cmbl & 1 << 3 ? 1 : 0]}
		  BIR:     $bir

	CMBLOC
}

"$rootdir/scripts/setup.sh" reset

xtrace_disable

# Look for all the nvme controllers which support CMB. The easiest, and
# fastest, way to do it is by checking if the cmb attr under sysfs has
# been created. If it doesn't exist, than the nvme driver must have
# read 0 from the CMBSZ register (meaning CMB is either not supported
# or the CMBMSC.CRE bit was cleared).

cmb_nvmes=()
for nvme in /sys/class/nvme/nvme+([0-9]); do
	[[ -e $nvme/cmb ]] || continue
	cmb_nvmes+=("$(< "$nvme/address")")
	printf '* %s (%s:%s:%s:%s) CMB:\n' \
		"${nvme##*/}" \
		"$(< "$nvme/address")" \
		"$(< "$nvme/model")" \
		"$(< "$nvme/serial")" \
		"$(< "$nvme/transport")"
	decode_cmb_regs "$nvme/cmb"
done

xtrace_restore

"$rootdir/scripts/setup.sh"

# Check if controllers which we found have been taken out from the nvme
# driver. If yes then we may safely use it for our tests.

for nvme in "${!cmb_nvmes[@]}"; do
	if [[ -e /sys/bus/pci/drivers/nvme/${cmb_nvmes[nvme]} ]]; then
		printf '%s device still in use, skipping\n' "${cmb_nvmes[nvme]}"
		unset -v "cmb_nvmes[nvme]"
	fi >&2
done

if ((${#cmb_nvmes[@]} == 0)); then
	printf 'No suitable nvme devices found, aborting CMB tests\n' >&2
	exit 1
fi

run_test "cmb_copy" "$testdir/cmb_copy.sh" "${cmb_nvmes[@]}"
