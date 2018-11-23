#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

if [ -z $http_proxy ] ||[ -z $https_proxy ] || [ -z $SPDK_VAGRANT_HTTP_PROXY ] ; then
echo "Here you should set the right proxy for downloading!"
echo "Please export right proxy http_proxy= ;https_proxy= ;SPDK_VAGRANT_HTTP_PROXY= "
exit 1
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
   # Now only support to creat a fedora26 to run autotest cases
   # for further develop, you can try ubuntu or freeBSD
    if [ $OS = "fedora26" ]; then
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
    vagrant_run_cmd "sudo cp /home/vagrant/spdk_repo/spdk/test/vagrant/$CONFIG /home/vagrant"
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
VM_INFO_HASH["fedora26"]="16-16384-autorun_spdk.conf"
# if support this system
#VM_INFO_HASH["ubuntu18"]="16-16384-autorun_spdk.conf"
#VM_INFO_HASH["freebsd11"]="16-16384-autorun_spdk.conf"
: ${VM_RECREATE:=false}
: ${VM_ERADICATE:=false}
for OS in ${!VM_INFO_HASH[@]}; do
    cd $rootdir
    echo "Start testing autotest cases in the vm of $OS"
    NCPU=$(echo ${VM_INFO_HASH[$OS]} | awk -F'-' '{print $1}')
    MEMORY_SIZE=$(echo ${VM_INFO_HASH[$OS]} | awk -F'-' '{print $2}')
    CONFIG=$(echo ${VM_INFO_HASH[$OS]} | awk -F'-' '{print $3}')
    VAGRANT_DIR="$(readlink -f ../$OS-$PROVIDER)"
    trap "vm_destroy $OS-$PROVIDER; break" SIGINT SIGTERM ERR
    if [ $VM_RECREATE = true ]; then
        if [ -d "$VAGRANT_DIR" ]; then
            VM_UUID=$(find $VAGRANT_DIR/.vagrant -type f -name id -exec cat {} +)
            vm_destroy $OS-$PROVIDER
        fi
        vm_create $MEMORY_SIZE $NCPU $OS
        VM_UUID=$(find $VAGRANT_DIR/.vagrant -type f -name id -exec cat {} +)
        vm_setup_env
        vm_check_nvme_device
    else
        if [ ! -d "$VAGRANT_DIR" ]; then
            vm_create $MEMORY_SIZE $NCPU $OS
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
        vm_destroy $OS-$PROVIDER
    fi
done
