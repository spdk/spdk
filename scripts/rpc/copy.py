def scan_copy_engine_ioat(client, args):
    params = {}
    if args.whitelist:
        whitelist = []
        for w in args.whitelist.strip().split(" "):
            whitelist.append(w)
        params['whitelist'] = whitelist
    return client.call('scan_copy_engine_ioat', params)


def get_copy_engine_ioat(client):
    return client.call('get_copy_engine_ioat')
