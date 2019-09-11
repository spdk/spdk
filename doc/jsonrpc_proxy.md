# JSON-RPC Remote access {#jsonrpc_proxy}

SPDK provides a sample python script `rpc_http_proxy.py`, that provides http server which listens for JSON objects from users. It uses HTTP POST method to receive JSON objects including methods and parameters described in this chapter.

## Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
server IP               | Required | string      | IP address that JSON objects shall be received on
server port             | Required | number      | Port number that JSON objects shall be received on
user name               | Required | string      | User name that will be used for authentication
password                | Required | string      | Password that will be used for authentication
RPC listen address      | Optional | string      | Path to SPDK JSON RPC socket. Default: /var/tmp/spdk.sock

## Example usage

`spdk/scripts/rpc_http_proxy.py 192.168.0.2 8000 user password`

## Returns

Error 401 - missing or incorrect user and/or password.

Error 400 - wrong JSON syntax or incorrect JSON method

Status 200 with resultant JSON object included on success.

## Client side

Below is a sample python script acting as a client side. It sends `bdev_get_bdevs` method with optional `name` parameter and prints JSON object returned from remote_rpc script.

~~~
import json
import requests

if __name__ == '__main__':
	payload = {'id':1, 'method': 'bdev_get_bdevs', 'params': {'name': 'Malloc0'}}
	url = 'http://192.168.0.2:8000/'
	req = requests.post(url,
                        data=json.dumps(payload),
                        auth=('user', 'password'),
                        verify=False,
                        timeout=30)
	print (req.json())
~~~

Output:

~~~
python client.py
[{u'num_blocks': 2621440, u'name': u'Malloc0', u'uuid': u'fb57e59c-599d-42f1-8b89-3e46dbe12641', u'claimed': True, u'driver_specific': {}, u'supported_io_types': {u'reset': True, u'nvme_admin': False, u'unmap': True, u'read': True, u'nvme_io': False, u'write': True, u'flush': True, u'write_zeroes': True}, u'qos_ios_per_sec': 0, u'block_size': 4096, u'product_name': u'Malloc disk', u'aliases': []}]
~~~
