#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import copy
import ctypes
import json
import socket
import unittest
from unittest import mock

from spdk.rpc.client import JSONRPCClient, JSONRPCException, JSONRPCGoClient


class JSONRPCClientTest(unittest.TestCase):
    def setUp(self):
        self.stream, self.peer = socket.socketpair()
        self.addCleanup(self.stream.close)
        self.addCleanup(self.peer.close)
        self.peer.settimeout(1)
        with mock.patch.object(JSONRPCClient, "_connect"):
            self.client = JSONRPCClient(self.id(), timeout=0.1)
        self.client.sock = self.stream

    def respond(self, response):
        self.peer.sendall(json.dumps(response, ensure_ascii=False).encode("utf-8"))

    def respond_error(self):
        self.respond({"jsonrpc": "2.0", "id": self.client._request_id + 1,
                      "error": {"code": -32602, "message": "Invalid params"}})

    def test_error_preserves_caller_params(self):
        params = {"name": "example", "method": "original", "req_id": "original",
                  "nested": {"values": [1, 2]}}
        original = copy.deepcopy(params)
        self.respond_error()
        with self.assertRaises(JSONRPCException) as error:
            self.client.call("test_method", params)
        self.assertEqual(params, original)
        self.assertIn('"method": "test_method"', error.exception.message)
        self.assertIn('"id": 1', error.exception.message)
        self.assertIn("Invalid params", error.exception.message)

    def test_error_preserves_positional_params(self):
        params = ["example", {"value": 1}]
        original = copy.deepcopy(params)
        self.respond_error()
        with self.assertRaises(JSONRPCException):
            self.client.call("test_method", params)
        self.assertEqual(params, original)

    def test_error_without_params(self):
        self.respond_error()
        with self.assertRaises(JSONRPCException) as error:
            self.client.call("test_method")
        self.assertIn("Invalid params", error.exception.message)

    def test_params_can_be_reused_after_error(self):
        params = {"name": "example"}
        self.respond_error()
        with self.assertRaises(JSONRPCException):
            self.client.call("test_method", params)
        self.respond({"jsonrpc": "2.0", "id": 2, "result": True})
        self.assertTrue(self.client.call("test_method", params))
        self.assertEqual(params, {"name": "example"})

    def test_go_error_preserves_caller_params(self):
        params = {"name": "example", "nested": {"values": [1, 2]}}
        original = copy.deepcopy(params)
        payload = ctypes.create_string_buffer(json.dumps({
            "jsonrpc": "2.0", "id": 7,
            "error": {"code": -32602, "message": "Invalid params"},
        }).encode("utf-8"))
        pointer = ctypes.cast(payload, ctypes.POINTER(ctypes.c_char))
        library = mock.Mock()
        library.spdk_gorpc_call.return_value = mock.Mock(error=0, response=pointer)
        client = JSONRPCGoClient(self.id())
        with mock.patch("spdk.rpc.client.ctypes.cdll.LoadLibrary", return_value=library):
            with self.assertRaises(JSONRPCException) as error:
                client.call("test_method", params)
        self.assertEqual(params, original)
        self.assertIn('"id": 7', error.exception.message)
        self.assertIn("Invalid params", error.exception.message)
        library.spdk_gorpc_free_response.assert_called_once_with(pointer)

    def test_matching_response_id(self):
        self.respond({"jsonrpc": "2.0", "id": 1, "result": {"value": 42}})
        self.assertEqual(self.client.call("test_method"), {"value": 42})

    def test_null_result(self):
        self.respond({"jsonrpc": "2.0", "id": 1, "result": None})
        self.assertIsNone(self.client.call("test_method"))

    def test_mismatched_response_ids(self):
        for response_id in (True, None, 0, 999, "1"):
            with self.subTest(response_id=response_id):
                self.respond({"jsonrpc": "2.0", "id": response_id, "result": "wrong"})
                with self.assertRaisesRegex(JSONRPCException, "Response ID"):
                    self.client.call("test_method")

    def test_missing_response_id(self):
        self.respond({"jsonrpc": "2.0", "result": "wrong"})
        with self.assertRaisesRegex(JSONRPCException, "Response ID"):
            self.client.call("test_method")

    def test_non_object_response(self):
        self.respond([{"jsonrpc": "2.0", "id": 1, "result": "wrong"}])
        with self.assertRaisesRegex(JSONRPCException, "Response ID"):
            self.client.call("test_method")

    def test_mismatched_error_response_id(self):
        self.respond({"jsonrpc": "2.0", "id": 99,
                      "error": {"code": -32602, "message": "Another request failed"}})
        with self.assertRaisesRegex(JSONRPCException, "Response ID"):
            self.client.call("test_method")

    def test_late_response_is_not_returned_for_next_call(self):
        self.client.timeout = 0.02
        with self.assertRaisesRegex(JSONRPCException, "Timeout"):
            self.client.call("first_request")
        self.respond({"jsonrpc": "2.0", "id": 1, "result": "first result"})
        with self.assertRaisesRegex(JSONRPCException, "Response ID"):
            self.client.call("second_request")

    def test_batch_response(self):
        self.client._batch_mode = True
        self.respond([{"jsonrpc": "2.0", "id": 1, "result": "first"},
                      {"jsonrpc": "2.0", "id": 2, "result": "second"}])
        self.assertEqual(self.client.call_batch([{"method": "first"}, {"method": "second"}]),
                         (["first", "second"], []))

    def test_fragmented_utf8(self):
        for value in ("caf\u00e9", "\u20ac", "\U0001f600"):
            with self.subTest(value=value):
                response = {"jsonrpc": "2.0", "id": 1, "result": value}
                self.respond(response)
                with mock.patch.object(self.client, "sock", wraps=self.stream) as stream:
                    stream.recv.side_effect = lambda size: self.stream.recv(1)
                    self.assertEqual(self.client.recv(), response)

    def test_utf8_fragment_survives_timeout(self):
        self.client.timeout = 0.02
        response = {"jsonrpc": "2.0", "id": 1, "result": "caf\u00e9"}
        payload = json.dumps(response, ensure_ascii=False).encode("utf-8")
        split = payload.index(b"\xc3\xa9") + 1
        self.peer.sendall(payload[:split])
        with self.assertRaisesRegex(JSONRPCException, "Timeout"):
            self.client.recv()
        self.peer.sendall(payload[split:])
        self.assertEqual(self.client.recv(), response)

    def test_concatenated_responses_with_partial_utf8(self):
        first = {"jsonrpc": "2.0", "id": 1, "result": "first"}
        second = {"jsonrpc": "2.0", "id": 2, "result": "\u20ac"}
        payload = json.dumps(second, ensure_ascii=False).encode("utf-8")
        split = payload.index(b"\xe2\x82\xac") + 1
        self.peer.sendall(json.dumps(first).encode("utf-8") + payload[:split])
        self.assertEqual(self.client.recv(), first)
        self.peer.sendall(payload[split:])
        self.assertEqual(self.client.recv(), second)

    def test_invalid_utf8_reports_protocol_error(self):
        self.peer.sendall(b'{"jsonrpc":"2.0","id":1,"result":"\xff"}')
        with self.assertRaisesRegex(JSONRPCException, "Invalid UTF-8"):
            self.client.recv()

    def test_truncated_utf8_reports_closed_connection(self):
        self.peer.sendall(b'{"jsonrpc":"2.0","id":1,"result":"\xc3')
        self.peer.shutdown(socket.SHUT_WR)
        with self.assertRaisesRegex(JSONRPCException, "Connection closed"):
            self.client.recv()
        self.assertIsNone(self.client.sock)


if __name__ == "__main__":
    unittest.main()
