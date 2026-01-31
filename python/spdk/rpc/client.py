#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.

import copy
import ctypes
import json
import logging
import os
import socket
import time
from abc import ABC, abstractmethod
from typing import Any, Mapping, Optional

from .cmd_parser import remove_null


def get_addr_type(addr):
    try:
        socket.inet_pton(socket.AF_INET, addr)
        return socket.AF_INET
    except Exception:
        pass
    try:
        socket.inet_pton(socket.AF_INET6, addr)
        return socket.AF_INET6
    except Exception:
        pass
    if os.path.exists(addr):
        return socket.AF_UNIX
    return None


class JSONRPCAbstractClient(ABC):
    @abstractmethod
    def call(self, method: str, params: Optional[Mapping[str, Any]] = None) -> Any: ...


class JSONRPCException(Exception):
    def __init__(self, message):
        self.message = message


class JSONRPCDryRunClient(JSONRPCAbstractClient):
    def __init__(self, batch_mode=False):
        self._batch_mode = batch_mode
        self._reqs = []

    def __getattr__(self, name):
        return lambda **kwargs: self.call(name, remove_null(kwargs))

    def call(self, method, params=None):
        req = {"method": method, "params": params}
        if self._batch_mode:
            self._reqs.append(req)
            return None
        print("Request:\n" + json.dumps(req, indent=2))

    def send_batch(self):
        if not self._reqs:
            return False
        print("Request:\n" + json.dumps(self._reqs, indent=2))
        self._reqs = []
        return False


class JSONRPCClient(JSONRPCAbstractClient):
    def __init__(self, addr, port=None, timeout=None, **kwargs):
        self.sock = None
        ch = logging.StreamHandler()
        ch.setFormatter(logging.Formatter('%(levelname)s: %(message)s'))
        ch.setLevel(logging.DEBUG)
        self._logger = logging.getLogger("JSONRPCClient(%s)" % addr)
        self._logger.addHandler(ch)
        self.set_log_level(kwargs.get('log_level', logging.ERROR))
        connect_retries = kwargs.get('conn_retries', 0)

        self.timeout = timeout if timeout is not None else 60.0
        self._request_id = 0
        self._recv_buf = ""
        self._reqs = []
        self._batch_mode = kwargs.get('batch_mode', False)

        for _ in range(connect_retries):
            try:
                self._connect(addr, port)
                return
            except Exception:
                # ignore and retry in 200ms
                time.sleep(0.2)

        # try one last time without try/except
        self._connect(addr, port)

    def __enter__(self):
        return self

    def __exit__(self, exception_type, exception_value, traceback):
        self.close()

    def _connect(self, addr, port):
        try:
            addr_type = get_addr_type(addr)

            if addr_type == socket.AF_UNIX:
                self._logger.debug("Trying to connect to UNIX socket: %s", addr)
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(addr)
            elif addr_type == socket.AF_INET6:
                self._logger.debug("Trying to connect to IPv6 address addr:%s, port:%i", addr, port)
                for res in socket.getaddrinfo(addr, port, socket.AF_INET6, socket.SOCK_STREAM, socket.SOL_TCP):
                    af, socktype, proto, _, sa = res
                self.sock = socket.socket(af, socktype, proto)
                self.sock.connect(sa)
            elif addr_type == socket.AF_INET:
                self._logger.debug("Trying to connect to IPv4 address addr:%s, port:%i'", addr, port)
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.connect((addr, port))
            else:
                raise socket.error("Invalid or non-existing address: '%s'" % addr)
        except socket.error as ex:
            raise JSONRPCException("Error while connecting to %s\n"
                                   "Is SPDK application running?\n"
                                   "Error details: %s" % (addr, ex),
                                   ) from ex

    def get_logger(self):
        return self._logger

    """Set logging level

    Args:
        lvl: Log level to set as accepted by logger.setLevel
    """
    def set_log_level(self, lvl):
        self._logger.info("Setting log level to %s", lvl)
        self._logger.setLevel(lvl)
        self._logger.info("Log level set to %s", lvl)

    def close(self):
        if getattr(self, "sock", None):
            self.sock.shutdown(socket.SHUT_RDWR)
            self.sock.close()
            self.sock = None

    def add_request(self, method, params):
        self._request_id += 1
        req = {
            'jsonrpc': '2.0',
            'method': method,
            'id': self._request_id,
        }

        if params:
            req['params'] = copy.deepcopy(params)

        self._logger.debug("append request:\n%s\n", json.dumps(req))
        self._reqs.append(req)
        return self._request_id

    def flush(self):
        self._logger.debug("Flushing buffer")
        if self._batch_mode:
            # Send as JSON-RPC batch array
            reqstr = json.dumps(self._reqs, indent=2)
        else:
            # TODO: We can drop indent parameter
            reqstr = "\n".join(json.dumps(req, indent=2) for req in self._reqs)
        self._logger.info("Requests:\n%s\n", reqstr)
        self.sock.sendall(reqstr.encode("utf-8"))
        self._reqs = []

    def send(self, method, params=None):
        req_id = self.add_request(method, params)
        self.flush()
        return req_id

    def send_batch(self):
        if not self._reqs:
            return False
        self.flush()
        return True

    def decode_one_response(self):
        try:
            self._logger.debug("Trying to decode response '%s'", self._recv_buf)
            buf = self._recv_buf.lstrip()
            obj, idx = json.JSONDecoder().raw_decode(buf)
            self._recv_buf = buf[idx:]
            return obj
        except ValueError:
            self._logger.debug("Partial response")
            return None

    def recv(self):
        start_time = time.process_time()
        response = self.decode_one_response()
        while not response:
            try:
                timeout = self.timeout - (time.process_time() - start_time)
                self.sock.settimeout(timeout)
                newdata = self.sock.recv(4096)
                if not newdata:
                    self.sock.close()
                    self.sock = None
                    raise JSONRPCException("Connection closed with partial response:\n%s\n" % self._recv_buf)
                self._recv_buf += newdata.decode("utf-8")
                response = self.decode_one_response()
            except socket.timeout:
                break  # throw exception after loop to avoid Python freaking out about nested exceptions
            except ValueError:
                continue  # incomplete response; keep buffering

        if not response:
            raise JSONRPCException("Timeout while waiting for response:\n%s\n" % self._recv_buf)

        self._logger.info("response:\n%s\n", json.dumps(response, indent=2))
        return response

    def __getattr__(self, name):
        """Dynamically handle unknown attributes as JSON-RPC methods"""
        def rpc_method(**kwargs):
            if self._batch_mode:
                # In batch mode, collect requests instead of executing
                self.add_request(name, remove_null(kwargs))
                return None
            return self.call(name, remove_null(kwargs))
        return rpc_method

    def call(self, method, params=None):
        self._logger.debug("call('%s')" % method)
        params = {} if params is None else params
        if self._batch_mode:
            self.add_request(method, params)
            return None
        if self.timeout <= 0:
            raise JSONRPCException("Timeout value is invalid: %s\n" % self.timeout)
        req_id = self.send(method, params)
        try:
            response = self.recv()
        except JSONRPCException as e:
            """ Don't expect response to kill """
            if not self.sock and method == "spdk_kill_instance":
                self._logger.info("Connection terminated but ignoring since method is '%s'" % method)
                return {}
            else:
                raise e

        if 'error' in response:
            params["method"] = method
            params["req_id"] = req_id
            msg = "\n".join(["request:", "%s" % json.dumps(params, indent=2),
                             "Got JSON-RPC error response",
                             "response:",
                             json.dumps(response['error'], indent=2)])
            raise JSONRPCException(msg)

        return response['result']

    @staticmethod
    def handle_batch_response(response):
        if not isinstance(response, list):
            raise JSONRPCException("Expected batch response array, got: %s" % type(response))
        errors = []
        results = []
        for resp in response:
            if 'error' in resp:
                errors.append("\n".join(["Got JSON-RPC error response",
                                         "response:", json.dumps(resp['error'], indent=2)]))
                continue
            results.append(resp.get('result'))
        if len(errors) > 0:
            raise JSONRPCException("\n".join(errors))
        return results

    def call_batch(self, batch):
        self._logger.debug("call_batch() with %d requests" % len(batch))
        if self.timeout <= 0:
            raise JSONRPCException("Timeout value is invalid: %s\n" % self.timeout)

        response = [self.call(**item) for item in batch]
        if not self._batch_mode:
            return response

        self.send_batch()
        response = self.recv()
        return self.handle_batch_response(response)


class JSONRPCGoClient(JSONRPCAbstractClient):
    INVALID_PARAMETER_ERROR = 1
    CONNECTION_ERROR = 2
    JSON_RPC_CALL_ERROR = 3
    INVALID_RESPONSE_ERROR = 4

    def __init__(self, addr, **kwargs):
        self.addr = addr
        ch = logging.StreamHandler()
        ch.setFormatter(logging.Formatter('Go client - %(levelname)s: %(message)s'))
        ch.setLevel(logging.DEBUG)
        self._logger = logging.getLogger("JSONRPCGoClient(%s)" % addr)
        self._logger.addHandler(ch)
        self._logger.setLevel(kwargs.get('log_level', logging.ERROR))

    def call(self, method, params=None):
        self._logger.debug("call('%s')" % method)
        params = {} if params is None else params

        class GoClientResponse(ctypes.Structure):
            _fields_ = [("response", ctypes.POINTER(ctypes.c_char)), ("error", ctypes.c_int)]

        client_path = os.path.normpath(os.path.abspath(os.path.join(os.path.dirname(__file__),
                                                                    os.path.pardir))
                                       + '/../../build/go/rpc/libspdk_gorpc.so')
        try:
            lib = ctypes.cdll.LoadLibrary(client_path)
        except OSError as err:
            raise JSONRPCException(f'Failed to load the Go RPC client at {client_path}') from err
        lib.spdk_gorpc_free_response.argtypes = [ctypes.POINTER(ctypes.c_char)]
        lib.spdk_gorpc_call.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        lib.spdk_gorpc_call.restype = GoClientResponse

        command_info = {
            "method": method,
            "params": params,
        }
        resp = lib.spdk_gorpc_call(json.dumps(command_info).encode('utf-8'),
                                   self.addr.encode('utf-8'))
        if resp.error > 0:
            rpc_error = "\n".join(["request:", "%s" % json.dumps(command_info, indent=2),
                                   "Got JSON-RPC error response"])
            if resp.error == self.INVALID_PARAMETER_ERROR:
                rpc_error = "\n".join([rpc_error, "GoRPCClient: error when decoding "
                                                  "function arguments"])
            elif resp.error == self.CONNECTION_ERROR:
                rpc_error = "\n".join([rpc_error, "GoRPCClient: Error while connecting to "
                                                  f"{self.addr}. Is SPDK application running?"])
            elif resp.error == self.JSON_RPC_CALL_ERROR:
                rpc_error = "\n".join([rpc_error, "GoRPCClient: error on JSON-RPC call"])
            elif resp.error == self.INVALID_RESPONSE_ERROR:
                rpc_error = "\n".join([rpc_error, "GoRPCClient: error on creating json "
                                                  "representation of response"])
            raise JSONRPCException(rpc_error)

        try:
            json_resp = json.loads(ctypes.c_char_p.from_buffer(resp.response).value)
        finally:
            lib.spdk_gorpc_free_response(resp.response)

        if 'error' in json_resp:
            params["method"] = method
            params["req_id"] = json_resp['id']
            msg = "\n".join(["request:", "%s" % json.dumps(params, indent=2),
                             "Got JSON-RPC error response",
                             "response:",
                             json.dumps(json_resp['error'], indent=2)])
            raise JSONRPCException(msg)

        return json_resp['result']
