#!/usr/bin/env python3
import sys
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

        # Counter for reseived requests. It can be used to order
        self.counter = 0

    def _process_response(self, response):
        request = self._requests.get(response['id'], False)

        # verify if we sent request for this response
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
    """
    def get_response(self, id):
        response = self._responses.get(id, False)

        if response:
            return self._process_response(response)

        while True:
            response = self.recv()

            if response['id'] in self._responses:
                raise JSONRPCException("Duplicated response: id=%s" % (response['id']))

            if response:
                self.counter += 1
                response['counter'] = self.counter
                self._responses[response['id']] = response
                if response['id'] == id:
                    return self._process_response(response)

        return False

    """Get multiple responses

    Args:
        ids: array of id
    """
    def get_responses(self, ids):
        errstr = ""
        responses = []
        for id in ids:
            try:
                response = self.get_response(id)
                if response:
                    responses.append(response)
            except JSONRPCException as err:
                request = self._requests.get(id, False)
                errstr += "Error: request id %s ('%s'): request: %s : " % (id, request['info'], request['request'])
                if "response" in request:
                    errstr += "result: " + str(request['response']['result']) + ": "
                errstr += str(err) + "\n"
                pass
        if errstr:
            raise JSONRPCException("Error while waiting for responses to " + str(ids) + ":\n\n" + errstr)
        return responses

    """Create new request

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

    """Create new notification request

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
    if "error" not in request['response']:
        raise JSONRPCException("This request should fail")


def delete_malloc_bdev_verify_success(request):
    if not request['response']['result']:
        raise JSONRPCException("Deletion of bdev failed (expected success)")


def delete_malloc_bdev_verify_fail(request):
    if "error" not in request['response']:
        raise JSONRPCException("This request should fail")


def case_message(func):
    def inner(*args, **kwargs):
        test_name = {
            1: 'get_notifications_malloc_construct_delete_one_client',
            2: 'get_notifications_malloc_construct_delete_one_client_errors',
            3: 'get_notifications_malloc_construct_delete_three_clients',
            4: 'get_notifications_malloc_construct_delete_three_clients_multirequests',
        }
        num = int(func.__name__.strip('test_case')[:])
        print("************************************")
        print("START TEST CASE {name}".format(name=test_name[num]))
        print("************************************")
        fail_count = func(*args, **kwargs)
        print("************************************")
        if not fail_count:
            print("END TEST CASE {name} PASS".format(name=test_name[num]))
            print("************************************")
        else:
            print("END TEST CASE {name} FAIL".format(name=test_name[num]))
            print("************************************")
            sys.exit(1)
        return fail_count
    return inner


class TestCases(object):

    def __init__(self, rpc_sock):
        self.rpc_sock = rpc_sock
        self.c1 = TestClient(self.rpc_sock)
        self.c2 = TestClient(self.rpc_sock)
        self.c3 = TestClient(self.rpc_sock)

    def check(self):

        self.c1.new_request("get_bdevs")
        self.c1.flush()
        responses = self.c1.get_responses(self.c1.get_request_ids())
        if len(responses[0]['response']['result']) != 0:
            return 1
        self.c2.new_request("get_bdevs")
        self.c2.flush()
        responses = self.c2.get_responses(self.c2.get_request_ids())
        if len(responses[0]['response']['result']) != 0:
            return 1
        self.c3.new_request("get_bdevs")
        self.c3.flush()
        responses = self.c3.get_responses(self.c3.get_request_ids())
        if len(responses[0]['response']['result']) != 0:
            return 1

        return 0

    @case_message
    def test_case1(self):
        """
        get_notifications_malloc_construct_delete_one_client
        """

        fail_count = 0

        self.c1.new_notification_request({'max_count': 2, 'timeout_ms': 3000})
        self.c1.flush()

        # We need to wait a moment to be sure that all notification requests took effect
        time.sleep(2)

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

        return self.check()

    @case_message
    def test_case2(self):
        """
        get_notifications_malloc_construct_delete_one_client_errors
        """

        self.c1.new_notification_request({'max_count': 2, 'timeout_ms': 6000})
        self.c1.flush()

        # We need to wait a moment to be sure that all notification requests took effect
        time.sleep(2)

        # First construct should success
        self.c1.new_request("construct_malloc_bdev",
                            {'num_blocks': 64, 'block_size': 1024, 'name': "Malloc0"},
                            info="First construct")

        # Second construct should fail
        self.c1.new_request("construct_malloc_bdev",
                            {'num_blocks': 64, 'block_size': 1024, 'name': "Malloc0"},
                            info="Second construct")

        # First delete should success
        self.c1.new_request("delete_malloc_bdev",
                            {'name': "Malloc0"},
                            info="First delete")

        # Second delete should fail
        self.c1.new_request("delete_malloc_bdev",
                            {'name': "Malloc0"},
                            info="Second delete")
        self.c1.flush()

        response_ids = self.c1.get_request_ids()
        responses = self.c1.get_responses(response_ids)

        # We should receive only notifications about one construct and one
        # delete
        notification_ids = self.c1.get_notification_ids()
        notifications = self.c1.get_responses(notification_ids)

        if len(notifications) != 1:
            return 1

        events = notifications[0]['response']['result']
        if events[0]['method'] != 'construct_malloc_bdev':
            return 1
        if events[1]['method'] != 'delete_malloc_bdev':
            return 1

        return self.check()

    @case_message
    def test_case3(self):
        """
        get_notifications_malloc_construct_delete_three_clients
        """

        # Activate notifications for 3 clients
        self.c1.new_notification_request({'max_count': 1, 'timeout_ms': 5000})
        self.c1.new_notification_request({'max_count': 1, 'timeout_ms': 5000})
        self.c1.flush()
        self.c2.new_notification_request({'max_count': 2, 'timeout_ms': 10000})
        self.c2.flush()
        self.c3.new_notification_request({'max_count': 10, 'timeout_ms': 15000})
        self.c3.flush()

        # We need to wait a moment to be sure that all notification requests took effect
        time.sleep(2)

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
            print_dict(events1)
            print_dict(events2)
            print_dict(events3)
            return 1

        if events1[0]['method'] != 'construct_malloc_bdev':
            return 1

        if events2[0]['method'] != 'construct_malloc_bdev':
            return 1

        if events3[0]['method'] != 'construct_malloc_bdev':
            return 1

        if events1[1]['method'] != 'delete_malloc_bdev':
            return 1

        if events2[1]['method'] != 'delete_malloc_bdev':
            return 1

        if events3[1]['method'] != 'delete_malloc_bdev':
            return 1

        return self.check()

    @case_message
    def test_case4(self):
        """
        get_notifications_malloc_construct_delete_tree_clients_multirequests
        """
        self.c1.new_notification_request({'max_count': 20, 'timeout_ms': 3000},
                                         info="First notification request for client 1")
        self.c1.new_notification_request({'max_count': 20, 'timeout_ms': 3000},
                                         info="Second notification request for client 1")
        self.c1.flush()
        self.c2.new_notification_request({'max_count': 10, 'timeout_ms': 3000},
                                         info="First notification request for client 2")
        self.c2.flush()
        self.c3.new_notification_request({'max_count': 5, 'timeout_ms': 3000},
                                         info="First notification request for client 3")
        self.c3.new_notification_request({'max_count': 5, 'timeout_ms': 3000},
                                         info="Second notification request for client 3")
        self.c3.flush()

        # We need to wait a moment to be sure that all notification requests took effect
        time.sleep(2)

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

        responses1 = self.c1.get_responses(self.c1.get_request_ids())

        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc0"},
                            verify=delete_malloc_bdev_verify_success)
        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc1"},
                            verify=delete_malloc_bdev_verify_success)
        self.c1.new_request("construct_malloc_bdev",
                            {'num_blocks': 32, 'block_size': 512, 'name': "Malloc3"})
        self.c1.flush()

        responses1.extend(self.c1.get_responses(self.c1.get_request_ids()))

        self.c1.new_request("construct_malloc_bdev",
                            {'num_blocks': 64, 'block_size': 1024, 'name': "Malloc0"})
        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc2"},
                            verify=delete_malloc_bdev_verify_success)
        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc3"},
                            verify=delete_malloc_bdev_verify_success)
        self.c1.flush()

        responses1.extend(self.c1.get_responses(self.c1.get_request_ids()))

        self.c1.new_request("delete_malloc_bdev", {'name': "Malloc0"},
                            verify=delete_malloc_bdev_verify_success)
        self.c1.flush()

        responses1.extend(self.c1.get_responses(self.c1.get_request_ids()))

        notification_ids1 = self.c1.get_notification_ids()
        notifications1 = self.c1.get_responses(notification_ids1)
        events1 = []
        for notification in notifications1:
            events1.extend(notification['response']['result'])

        events2 = []
        for notification in self.c2.get_responses(self.c2.get_notification_ids()):
            events2.extend(notification['response']['result'])

        events3 = []
        for notification in self.c3.get_responses(self.c3.get_notification_ids()):
            events3.extend(notification['response']['result'])

        if len(events1) != 10 or len(events2) != 10 or len(events3) != 10:
            print("ERROR: Expected 10 notifications for each client")
            print_dict(events1)
            print_dict(events2)
            print_dict(events3)
            return 1

        # Each client should receive the same notifications
        if events1 != events2 or events1 != events3:
            print("ERROR: Each client should receive the same notifications")
            print_dict(events1)
            print_dict(events2)
            print_dict(events3)
            return 1

        return self.check()


if __name__ == "__main__":
    rpc_sock = sys.argv[1]

    tc = TestCases(rpc_sock)
    tc.test_case1()
    tc.test_case2()
    tc.test_case3()
    tc.test_case4()
