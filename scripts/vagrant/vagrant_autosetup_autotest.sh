#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh


#Checking proxy if you need.
if [ -z $http_proxy ] ||[ -z $https_proxy ] || [ -z $SPDK_VAGRANT_HTTP_PROXY ] ; then
echo "Here you should set the right proxy for downloading!"
echo "Please export right proxy http_proxy= ;https_proxy= ;SPDK_VAGRANT_HTTP_PROXY= "

echo "if you don't need to have a proxy setting ,please overlook this, and continue!"
fi

#Function for editing autorun_cfg, before running this script, we can edit the conf.

#This is left for debug.sometimes we need to check some kind of issue by setting without code commited.
#If you want to debug, please "export DEBUG=true"

DEBUG=${DEBUG:-false}

if [ "$DEBUG" = true ]; then
function config_vagrant_file(){
    local cfg_file="
sudo cat << EOF > ~/autorun-spdk.conf
#Assign a value of 1 to all of the pertinent tests
SPDK_BUILD_DOC=1
SPDK_RUN_CHECK_FORMAT=1
SPDK_RUN_SCANBUILD=0
SPDK_RUN_VALGRIND=0
SPDK_TEST_UNITTEST=1
SPDK_TEST_ISCSI=0
SPDK_TEST_ISCSI_INITIATOR=0
SPDK_TEST_NVME=0
SPDK_TEST_NVME_CLI=0
SPDK_TEST_NVMF=1
SPDK_TEST_RBD=0
SPDK_TEST_CRYPTO=0
#Requires some extra configuration. see TEST_ENV_SETUP_README
SPDK_TEST_VHOST=0
SPDK_TEST_VHOST_INIT=0
SPDK_TEST_BLOCKDEV=1
#Doesn't work on vm
SPDK_TEST_IOAT=0
SPDK_TEST_EVENT=1
SPDK_TEST_BLOBFS=0
SPDK_TEST_PMDK=0
SPDK_TEST_LVOL=0
SPDK_TEST_REDUCE=0
SPDK_RUN_ASAN=0
SPDK_RUN_UBSAN=0
# Reduce the size of the hugepages
HUGEMEM=1024
#Set virtual machine OS
VM_OS=fedora28
#Set number of CPU
NCPU=16
#Set memory size
MEMORY_SIZE=16384
# Set up the DEPENDENCY_DIR
DEPENDENCY_DIR=/home/vagrant
EOF
"
vagrant_run_cmd "$cfg_file"
}
fi

if [ -n "$PROVIDER" ]; then
    provider="--provider=${PROVIDER}"
fi


# Function for vagrant operations
function vagrant_get_id() {
    vagrant global-status | grep $VAGRANT_DIR | awk '{print $1}'
}
function vagrant_up() {
    vagrant up $(vagrant_get_id) --provision
}
function vagrant_reload() {
    vagrant reload $(vagrant_get_id) --provision
}
function vagrant_provision() {
    vagrant provision $(vagrant_get_id)
}
function vagrant_halt() {
    vagrant halt $(vagrant_get_id)
}
function vagrant_remove() {
    vagrant destroy -f $(vagrant_get_id)
}
function vagrant_run_cmd() {
    vagrant ssh $(vagrant_get_id) -c "$@"
}
function usage() {
cat <<EOF
Usage: $0 [-reh] [--recreate|eradicate|help]
  -r|--recreate  : Recreate the virtual machine regardless of its existence.
  -e|--eradicate : After running the case, eradicate the virtual machine.
                 : Without parameters means a VM which already exists.
  -h|--help      : Show help.
EOF
}
function vm_create() {
    cd $rootdir/..
    if [ ! -d $3 ]; then
        spdk/scripts/vagrant/create_vbox.sh -s $1 -n $2 $3
    fi
    cd $rootdir
}
function vm_destroy() {
    vagrant_halt
    #vagrant_remove
    vboxmanage unregistervm $VM_UUID --delete || true
    rm -rf $rootdir/../$1
}
function vm_setup_env() {
    vagrant_run_cmd "sudo spdk_repo/spdk/scripts/vagrant/update.sh"
   # For further developing, you can try ubuntu or freeBSD
    if [ $VM_OS = "fedora26" ] || [ $VM_OS = "fedora28" ]; then
        vagrant_run_cmd "sudo spdk_repo/spdk/test/common/config/vm_setup.sh -i -t librxe,iscsi,rocksdb,fio,flamegraph,libiscsi"
    fi
}
function vm_check_nvme_device() {
    local cmdset="cd spdk_repo/spdk;\
        git submodule update --init;\
        ./configure --enable-debug && make;\
        sudo scripts/setup.sh;\
        cd examples/bdev/hello_world;\
        sudo ./hello_bdev"
    vagrant_run_cmd "$cmdset"
}
function vm_run_autotest() {

    vagrant_run_cmd "sudo rm -rf /home/vagrant/vagrant"
if [ "$DEBUG" = true ]; then
    config_vagrant_file
    vagrant_run_cmd "sudo cat /home/vagrant/autorun-spdk.conf"
    vagrant_run_cmd "sudo cp /home/vagrant/autorun-spdk.conf /root/autorun-spdk.conf"
    vagrant_run_cmd "sudo cat ~/autorun-spdk.conf"
else
    vagrant_run_cmd "sudo cp /home/vagrant/spdk_repo/spdk/scripts/vagrant/autorun-spdk.conf /home/vagrant/autorun-spdk.conf"
    vagrant_run_cmd "sudo cp /home/vagrant/autorun-spdk.conf /root/autorun-spdk.conf"
fi
    vagrant_run_cmd "sudo ~/spdk_repo/spdk/scripts/vagrant/run-autorun.sh -d /home/vagrant/spdk_repo/spdk"

}
set -- $(getopt -o reh --longoptions recreate,eradicate,help -- "$@")
while true; do
    case "$1" in
    -r|--recreate)
        VM_RECREATE=true
        shift 1
        ;;
    -e|--eradicate)
        VM_ERADICATE=true
        shift 1
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    --) break
        ;;
    *)  echo "Invalid argument!"
        usage
        exit 1
        ;;
    esac
done


declare -A VM_INFO_HASH

# The default value is decided by the real cases in VM, confirm the value is  good for auto running.
# We can left this in the config file,VM_OS= ,MEMORY_SIZE= ,NCPU= or export value.

VM_OS=${VM_OS:-fedora28}
MEMORY_SIZE=${MEMORY_SIZE:-16384}
NCPU=${NCPU:-16}

VM_INFO_HASH["fedora28"]="$NCPU-$MEMORY_SIZE-autorun-spdk.conf"
# If support this system
#VM_INFO_HASH["ubuntu18"]="$NCPU-$MEMORY_SIZE-autorun_spdk.conf"
#VM_INFO_HASH["freebsd11"]="$NCPU-$MEMORY_SIZE-autorun_spdk.conf"
: ${VM_RECREATE:=false}
: ${VM_ERADICATE:=false}
#Left for OS in ${!VM_INFO_HASH[@]}; do
#Now Only support fedora28,fedora26 has kernel issue for filesystem, other systems depend on run-autorun.sh,
#Which should solve the installing packages in different systems with different commands.

case "$VM_OS" in
    fedora28)
        cd $rootdir
        echo "Start testing autotest cases in the vm of $VM_OS"
        VAGRANT_DIR="$(readlink -f ../$VM_OS-$PROVIDER)"
        trap "vm_destroy $VM_OS-$PROVIDER; break" SIGINT SIGTERM ERR
        if [ $VM_RECREATE = true ]; then
            if [ -d "$VAGRANT_DIR" ]; then
                VM_UUID=$(find $VAGRANT_DIR/.vagrant -type f -name id -exec cat {} +)
                vm_destroy $VM_OS-$PROVIDER
            fi
            vm_create $MEMORY_SIZE $NCPU $VM_OS
            VM_UUID=$(find $VAGRANT_DIR/.vagrant -type f -name id -exec cat {} +)
            vm_setup_env
            vm_check_nvme_device
        else
            if [ ! -d "$VAGRANT_DIR" ]; then
                vm_create $MEMORY_SIZE $NCPU $VM_OS
                VM_UUID=$(find $VAGRANT_DIR/.vagrant -type f -name id -exec cat {} +)
                vm_setup_env
                vm_check_nvme_device
            else
                VM_UUID=$(find $VAGRANT_DIR/.vagrant -type f -name id -exec cat {} +)
                vagrant_provision
            fi
        fi
        trap - SIGINT SIGTERM ERR
        set +e
          vm_run_autotest
        set -e
        if [ $VM_ERADICATE = true ]; then
            vm_destroy $VM_OS-$PROVIDER
        fi
    ;;
    ubuntu18)
       echo "no ready."
       exit 1
    ;;
    freebsd11)
       echo "no ready."
       exit 1
    ;;
    *)
       echo " no OS selected! "
       exit 1
    ;;
esac
