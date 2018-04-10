from bdev import clear_bdev_subsystem
from iscsi import clear_scsi_subsystem
from iscsi import clear_iscsi_subsystem
from nbd import clear_nbd_subsystem
from vhost import clear_vhost_subsystem
from net import clear_net_framework_subsystem


def get_subsystems(client):
    return client.call('get_subsystems')


def get_subsystem_config(client, name):
    params = {'name': name}
    return client.call('get_subsystem_config', params)


def clear_subsystem(client, args):
    config = client.call('get_subsystem_config', {"name": args.subsystem})
    if config is None:
        return
    if args.verbose:
        print "Calling clear_%s_subsystem" % args.subsystem
    globals()["clear_%s_subsystem" % args.subsystem](args, config)


def clear_copy_subsystem(args, copy_config):
    pass


def clear_interface_subsystem(args, interface_config):
    pass
