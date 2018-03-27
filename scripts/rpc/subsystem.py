def get_subsystems(client, args):
    return client.call('get_subsystems')


def get_subsystem_config(client, args):
    params = {'name': args.name}
    return client.call('get_subsystem_config', params)
