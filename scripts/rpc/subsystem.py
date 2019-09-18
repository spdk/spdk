from .helpers import deprecated_alias


@deprecated_alias('get_subsystems')
def framework_get_subsystems(client):
    return client.call('framework_get_subsystems')


@deprecated_alias('get_subsystem_config')
def framework_get_config(client, name):
    params = {'name': name}
    return client.call('framework_get_config', params)
