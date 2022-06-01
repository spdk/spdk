def framework_get_subsystems(client):
    return client.call('framework_get_subsystems')


def framework_get_config(client, name):
    params = {'name': name}
    return client.call('framework_get_config', params)


def framework_get_pci_devices(client):
    return client.call('framework_get_pci_devices')
