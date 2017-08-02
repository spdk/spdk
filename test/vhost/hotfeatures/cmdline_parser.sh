#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

dry_run=false
no_shutdown=false
fio_bin="fio"
fio_jobs="$BASE_DIR/fio_jobs/"
test_type=spdk_vhost_scsi
reuse_vms=false
force_build=false
vms=()
used_vms=""
disk_split=""
x=""


function usage()
{
        [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
        echo "Shortcut script for doing automated hotattach/hotdetach test"
        echo "Usage: $(basename $1) [OPTIONS]"
        echo
        echo "-h, --help                print help and exit"
	echo "    --test-type=TYPE      Perform specified test:"
	echo "                          virtio - test host virtio-scsi-pci using file as disk image"
	echo "                          kernel_vhost - use kernel driver vhost-scsi"
	echo "                          spdk_vhost_scsi - use spdk vhost scsi"
	echo "                          spdk_vhost_blk - use spdk vhost block"
	echo "-x                        set -x for script debug"
	echo "    --fio-bin=FIO         Use specific fio binary (will be uploaded to VM)"
	echo "    --qemu-src=QEMU_DIR   Location of the QEMU sources"
	echo "    --dpdk-src=DPDK_DIR   Location of the DPDK sources"
	echo "    --fio-jobs=           Fio configs to use for tests. Can point to a directory or"
	echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: $TEST_DIR]"
	echo "    --force-build         Force SPDK rebuild with the specified DPDK path."
	echo "    --vm=NUM[,OS][,DISKS] VM configuration. This parameter might be used more than once:"
	echo "                          NUM - VM number (mandatory)"
	echo "                          OS - VM os disk path (optional)"
	echo "                          DISKS - VM os test disks/devices path (virtio - optional, kernel_vhost - mandatory)"
	echo "                          If test-type=spdk_vhost_blk then each disk can have additional size parameter, e.g."
	echo "                          --vm=X,os.qcow,DISK_size_35G; unit can be M or G; default - 20G"
        exit 0
}

while getopts 'xh-:' optchar; do
        case "$optchar" in
                -)
                case "$OPTARG" in
                        help) usage $0 ;;
                        work-dir=*) TEST_DIR="${OPTARG#*=}" ;;
                        fio-bin=*) fio_bin="${OPTARG#*=}" ;;
                        qemu-src=*) QEMU_SRC_DIR="${OPTARG#*=}" ;;
                        dpdk-src=*) DPDK_SRC_DIR="${OPTARG#*=}" ;;
                        fio-jobs=*) fio_jobs="${OPTARG#*=}" ;;
			test-type=*) test_type="${OPTARG#*=}" ;;
                        force-build) force_build=true ;;
                        vm=*) vms+=("${OPTARG#*=}") ;;
                        *) usage $0 "Invalid argument '$OPTARG'" ;;
                esac
                ;;
        h) usage $0 ;;
        x) set -x
                x="-x" ;;
        *) usage $0 "Invalid argument '$OPTARG'"
        esac
done
shift $(( OPTIND - 1 ))

. $BASE_DIR/../fiotest/common.sh

rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s 127.0.0.1 "

function check_qemu() {
        echo "==============="
        echo "INFO: checking qemu"

        if [[ ! -x $INSTALL_DIR/bin/qemu-system-x86_64 ]]; then
                echo "INFO: can't find $INSTALL_DIR/bin/qemu-system-x86_64 - building and installing"

                if [[ ! -d $QEMU_SRC_DIR ]]; then
                        echo "ERROR: Cannot find qemu source in $QEMU_SRC_DIR"
                        exit 1
                else
                        echo "INFO: qemu source exists $QEMU_SRC_DIR - building"
                        qemu_build_and_install
                fi
        fi

        echo "==============="
        echo ""
        echo "INFO: checking spdk"
        echo ""
}

function check_spdk() {
        if [[ ! -x $SPDK_BUILD_DIR/app/vhost/vhost ]] || $force_build ; then
                echo "INFO: $SPDK_BUILD_DIR/app/vhost/vhost - building and installing"
                spdk_build_and_install
        fi
}

function print_test_fio_header() {
        echo "==============="
        echo ""
        echo "INFO: Testing..."

        echo "INFO: Running fio jobs ..."
}

function run_vhost() {
        echo "==============="
        echo ""
        echo "INFO: running SPDK"
        echo ""
        spdk_vhost_run
        #$BASE_DIR/../fiotest/run_vhost.sh $x --work-dir=$TEST_DIR
        echo
}


function setup_vms() {
        for vm_conf in ${vms[@]}; do
                IFS=',' read -ra conf <<< "$vm_conf"
                setup_cmd="$BASE_DIR/../fiotest/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
                if [[ x"${conf[0]}" == x"" ]] || ! assert_number ${conf[0]}; then
                        echo "ERROR: invalid VM configuration syntax $vm_conf"
                        exit 1;
                fi

                # Sanity check if VM is not defined twice
                for vm_num in $used_vms; do
                        if [[ $vm_num -eq ${conf[0]} ]]; then
                                echo "ERROR: VM$vm_num defined more than twice ( $(printf "'%s' " "${vms[@]}"))!"
                                exit 1
                        fi
                done

                setup_cmd+=" -f ${conf[0]}"
                used_vms+=" ${conf[0]}"
                [[ x"${conf[1]}" != x"" ]] && setup_cmd+=" --os=${conf[1]}"
                [[ x"${conf[2]}" != x"" ]] && setup_cmd+=" --disk=${conf[2]}"

                $setup_cmd
        done
}

function setup_and_run_vms() {
        setup_vms
        # Run everything
        $BASE_DIR/../fiotest/vm_run.sh $x --work-dir=$TEST_DIR $used_vms
        vm_wait_for_boot 600 $used_vms
}

function prepare_vms() {
        for vm_num in $used_vms; do
                vm_dir=$VM_BASE_DIR/$vm_num

                qemu_mask_param="VM_${vm_num}_qemu_mask"

                host_name="VM-$vm_num-${!qemu_mask_param}"
                echo "INFO: Setting up hostname: $host_name"
                vm_ssh $vm_num "hostname $host_name"
                vm_start_fio_server --fio-bin=$fio_bin $readonly $vm_num
                #vm_check_scsi_location $vm_num
        done
}

function reboot_all_vms() {
        echo "Rebooting all vms"
        for vm_num in $used_vms; do
                vm_ssh $vm_num "reboot" || true
        done

        vm_wait_for_boot 600 $used_vms
}

function check_fio_retcode() {
        fio_retcode=$?
        echo $1
        if test "$fio_retcode" != "0"; then
                echo "    Fio test ended with error."
                exit 1
        else
                echo "    Fio test ended with success."
        fi
}

