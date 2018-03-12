def get_subsystems(args):
    return args.client.call('get_subsystems')


def get_subsystem_config(args):
    params = {'name': args.name}
    return args.client.call('get_subsystem_config', params)
