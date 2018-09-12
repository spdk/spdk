def get_subsystems(client):
    return client.call('get_subsystems')


def get_subsystem_config(client, name):
    params = {'name': name}
    return client.call('get_subsystem_config', params)
