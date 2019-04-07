WIN_COMMON_DIR=$(readlink -f $(dirname $0))
ROOT_DIR=$(readlink -f $WIN_COMMON_DIR/../../..)
. $WIN_COMMON_DIR/../common/common.sh
rpc_py="$ROOT_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

function vm_sshpass()
{
        vm_num_is_valid $1 || return 1

        local ssh_cmd="sshpass -p $2 ssh \
                -o UserKnownHostsFile=/dev/null \
                -o StrictHostKeyChecking=no \
                -o User=root \
                -p $(vm_ssh_socket $1) $VM_SSH_OPTIONS 127.0.0.1"

        shift 2
        $ssh_cmd "$@"
}

