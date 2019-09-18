from .helpers import deprecated_alias


@deprecated_alias('get_subsystems')
def framework_get_subsystems(client):
    return client.call('framework_get_subsystems')


def get_subsystem_config(client, name):
    params = {'name': name}
    return client.call('get_subsystem_config', params)
