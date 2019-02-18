#!/usr/bin/env python3
import json
import time
from rpc.client import JSONRPCClient, JSONRPCException
import pprint


def print_dict(d):
    pp = pprint.PrettyPrinter(indent=2)
    pp.pprint(d)


class TestClient(JSONRPCClient):
    def __init__(self, addr, port=None, timeout=60.0, **kwargs):
        JSONRPCClient.__init__(self, addr, port, timeout)

        self._request_ids = []
        self._notification_ids = []

        self._responses = {}
        self._requests = {}

    def _process_response(self, response):
        # verify if we sent request for this response
        request = self._requests.get(response['id'], False)
        if not request:
            raise JSONRPCException("Unexpected response: id=%s" % (response['id']))

        # extend stored request with response
        request['response'] = response

        # if verify method is set, do the verification of expected result
        if request['verify']:
            request['verify'](request)

        return request

    """Get response for specified 'id'

    Args:
        id: id of request
        wait: wait for response
    """
    def get_response(self, id, wait=True):
        response = self._responses.get(id, False)

        if response:
            return self._process_response(response)

        while True:
            response = self.recv()

            if self._responses.get(response['id'], False):
                raise JSONRPCException("Duplicated response: id=%s" % (response['id']))

            if response:
                self._responses[response['id']] = response
                if response['id'] == id:
                    return self._process_response(response)
            elif not wait:
                break

        return False

    """"Get multiple responses

    Args:
        ids: array of id
        wait: wait until all responses received. elseware return only available
            responses
    """
    def get_responses(self, ids, wait=True):
        responses = []
        for id in ids:
            response = self.get_response(id, wait)
            if response:
                responses.append(response)
        return responses

    """"Create new request

    Args:
        method: RPC call method
        params: method parameters
        info: additional information
        verify: function to verify expected response
    """
    def new_request(self, method, params=None, info="", verify=None):
        id = self.add_request(method, params)
        self._request_ids.append(id)
        self._requests[id] = {'request': {'method': method, 'params': params}, 'info': info, 'verify': verify}
        return id

    """Get an array of already sent requests"""
    def get_request_ids(self):
        request_ids = list(self._request_ids)
        self._request_ids.clear()
        return request_ids

    """"Create new notification request

    Args:
        method: RPC call method
        params: method parameters
        info: additional information
        verify: function to verify expected response
    """
    def new_notification_request(self, params=None, info=None, verify=None):
        id = self.add_request("get_notifications", params)
        self._notification_ids.append(id)
        self._requests[id] = {'request': {'method': "get_notifications", 'params': params}, 'info': info, 'verify': verify}
        return id

    """Get an array of already sent notification requests"""
    def get_notification_ids(self):
        notification_ids = list(self._notification_ids)
        self._notification_ids.clear()
        return notification_ids


def construct_malloc_bdev_verify_success(request):
    if request['request']['params']['name'] != request['response']['result']:
        raise JSONRPCException("Wrong name of bdev in response")


def construct_malloc_bdev_verify_fail(request):
    if not request['response']['error']:
        raise JSONRPCException("This notification should never be sent")


def delete_malloc_bdev_verify_success(request):
    if not request['response']['result']:
        raise JSONRPCException("Deletion of bdev failed (expected success)")


def delete_malloc_bdev_verify_fail(request):
    raise JSONRPCException("This notification should never be sent")


class TestCases(object):

    def __init__(self, rpc_sock):
        self.c1 = TestClient(rpc_sock)
        self.c2 = TestClient(rpc_sock)
        self.c3 = TestClient(rpc_sock)

    def test_case_1(self):
        print("--------------------------------------------")
        print("test_case_1")
        print("--------------------------------------------")

        self.c1.new_notification_request({'max_count': 2})
        self.c1.flush()

        self.c1.new_request("construct_malloc_bdev",
                            {'num_blocks': 64, 'block_size': 1024, 'name': "Malloc0"},
                            verify=construct_malloc_bdev_verify_success)
        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc0"},
                            verify=delete_malloc_bdev_verify_success)
        self.c1.flush()

        responses = self.c1.get_responses(self.c1.get_request_ids())

        notifications = self.c1.get_responses(self.c1.get_notification_ids())
        events = []
        for notification in notifications:
            # verify max_count for each get_notifications request
            if len(notification['response']['result']) > 2:
                sys.exit(0)
            events.extend(notification['response']['result'])

        # We should have exacly 2 notifications
        if len(events) != 2:
            sys.exit(1)

        if events[0]['method'] != 'construct_malloc_bdev':
            sys.exit(1)

        if events[1]['method'] != 'delete_malloc_bdev':
            sys.exit(1)

    def test_case_2(self):
        print("--------------------------------------------")
        print("test_case_2")
        print("--------------------------------------------")

        # Activate notifications for 3 clients
        self.c1.new_notification_request({'max_count': 2, 'timeout_ms': 3000})
        self.c1.flush()
        self.c2.new_notification_request({'max_count': 2, 'timeout_ms': 3000})
        self.c2.flush()
        self.c3.new_notification_request({'max_count': 2, 'timeout_ms': 3000})
        self.c3.flush()

        self.c1.new_request("construct_malloc_bdev", {'num_blocks': 64, 'block_size': 1024, 'name': "Malloc0"})
        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc0"})
        self.c1.flush()

        responses1 = self.c1.get_responses(self.c1.get_request_ids())
        notifications1 = self.c1.get_responses(self.c1.get_notification_ids())
        events1 = []
        for notification in notifications1:
            events1.extend(notification['response']['result'])

        notifications2 = self.c2.get_responses(self.c2.get_notification_ids())
        events2 = []
        for notification in notifications2:
            events2.extend(notification['response']['result'])

        notifications3 = self.c3.get_responses(self.c3.get_notification_ids())
        events3 = []
        for notification in notifications3:
            events3.extend(notification['response']['result'])

        # All clients should receive 2 notifications
        if len(events1) != 2 or len(events2) != 2 or len(events3) != 2:
            sys.exit(1)

        if events1[0]['method'] != 'construct_malloc_bdev':
            sys.exit(1)

        if events2[0]['method'] != 'construct_malloc_bdev':
            sys.exit(1)

        if events3[0]['method'] != 'construct_malloc_bdev':
            sys.exit(1)

        if events1[1]['method'] != 'delete_malloc_bdev':
            sys.exit(1)

        if events2[1]['method'] != 'delete_malloc_bdev':
            sys.exit(1)

        if events3[1]['method'] != 'delete_malloc_bdev':
            sys.exit(1)

    def test_case_3(self):
        print("--------------------------------------------")
        print("test_case_3")
        print("--------------------------------------------")

        self.c1.new_notification_request({'max_count': 2})
        self.c1.flush()

        self.c1.new_request("construct_malloc_bdev",
                            {'num_blocks': 64, 'block_size': 1024, 'name': "Malloc0"},
                            verify=construct_malloc_bdev_verify_success)
        self.c1.flush()
        self.c1.new_request("construct_malloc_bdev",
                            {'num_blocks': 64, 'block_size': 1024, 'name': "Malloc0"},
                            verify=construct_malloc_bdev_verify_fail)
        self.c1.flush()
        self.c1.new_request("delete_malloc_bdev",
                            {'name': "Malloc0"},
                            verify=delete_malloc_bdev_verify_success)
        self.c1.flush()
        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc0"},
                            verify=delete_malloc_bdev_verify_fail)
        self.c1.flush()

        print("Responses:")
        for response in self.c1.get_responses(self.c1.get_request_ids()):
            print_dict(response)

        print("Notifications:")
        for response in self.c1.get_responses(self.c1.get_notification_ids()):
            print_dict(response)

    def test_case_4(self):
        print("--------------------------------------------")
        print("test_case_4")
        print("--------------------------------------------")

        self.c1.new_request("get_notification_types")
        self.c1.new_notification_request({'max_count': 20, 'timeout_ms': 3000})
        self.c1.new_notification_request({'max_count': 20, 'timeout_ms': 3000})
        self.c1.flush()
        self.c2.new_notification_request({'max_count': 10, 'timeout_ms': 3000})
        self.c2.new_notification_request({'max_count': 10, 'timeout_ms': 3000})
        self.c2.flush()
        self.c3.new_request("get_notification_types")
        self.c3.new_notification_request({'max_count': 5, 'timeout_ms': 3000})
        self.c3.new_notification_request({'max_count': 5, 'timeout_ms': 3000})
        self.c3.flush()

        self.c1.new_request("construct_malloc_bdev",
                            {'num_blocks': 64, 'block_size': 1024, 'name': "Malloc0"},
                            verify=construct_malloc_bdev_verify_success)
        self.c1.new_request("construct_malloc_bdev",
                            {'num_blocks': 16, 'block_size': 1024, 'name': "Malloc1"},
                            verify=construct_malloc_bdev_verify_success)
        self.c1.new_request("construct_malloc_bdev",
                            {'num_blocks': 32, 'block_size': 512, 'name': "Malloc2"},
                            verify=construct_malloc_bdev_verify_success)
        self.c1.flush()

        print("Responses:")
        for response in self.c1.get_responses(self.c1.get_request_ids()):
            print_dict(response)

        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc0"})
        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc1"})
        self.c1.new_request("construct_malloc_bdev",
                            {'num_blocks': 32, 'block_size': 512, 'name': "Malloc3"})
        self.c1.flush()

        print("Responses:")
        for response in self.c1.get_responses(self.c1.get_request_ids()):
            print_dict(response)

        self.c1.new_request("construct_malloc_bdev",
                            {'num_blocks': 64, 'block_size': 1024, 'name': "Malloc0"})
        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc2"})
        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc3"})
        self.c1.flush()

        print("Responses:")
        for response in self.c1.get_responses(self.c1.get_request_ids()):
            print_dict(response)

        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc0"})
        self.c1.flush()
        print("Responses:")
        for response in self.c1.get_responses(self.c1.get_request_ids()):
            print_dict(response)

        print("Notifications:")
        print("--- Client 1 ---")
        notification_ids1 = self.c1.get_notification_ids()
        notifications1 = self.c1.get_responses(notification_ids1)
        for response in notifications1:
            print_dict(response)

        print("--- Client 2 ---")
        for response in self.c2.get_responses(self.c2.get_notification_ids()):
            print_dict(response)

        print("--- Client 3 ---")
        for response in self.c3.get_responses(self.c3.get_notification_ids()):
            print_dict(response)


def main():
    rpc_sock = "/var/tmp/spdk.sock"
    tc = TestCases(rpc_sock)
    tc.test_case_1()
    tc.test_case_2()
    # tc.test_case_3()
    # tc.test_case_4()

if __name__ == "__main__":
    main()
