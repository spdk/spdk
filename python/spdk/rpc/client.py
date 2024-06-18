#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.

import json
import socket
import time
import os
import logging
import copy
import ctypes


def print_dict(d):
    print(json.dumps(d, indent=2))


def print_json(s):
    print(json.dumps(s, indent=2).strip('"'))


def get_addr_type(addr):
    try:
        socket.inet_pton(socket.AF_INET, addr)
        return socket.AF_INET
    except Exception as e:
        pass
    try:
        socket.inet_pton(socket.AF_INET6, addr)
        return socket.AF_INET6
    except Exception as e:
        pass
    if os.path.exists(addr):
        return socket.AF_UNIX
    return None


class JSONRPCException(Exception):
    def __init__(self, message):
        self.message = message


class JSONRPCClient(object):
    def __init__(self, addr, port=None, timeout=None, **kwargs):
        self.sock = None
        ch = logging.StreamHandler()
        ch.setFormatter(logging.Formatter('%(levelname)s: %(message)s'))
        ch.setLevel(logging.DEBUG)
        self._logger = logging.getLogger("JSONRPCClient(%s)" % addr)
        self._logger.addHandler(ch)
        self.log_set_level(kwargs.get('log_level', logging.ERROR))
        connect_retries = kwargs.get('conn_retries', 0)

        self.timeout = timeout if timeout is not None else 60.0
        self._request_id = 0
        self._recv_buf = ""
        self._reqs = []

        for i in range(connect_retries):
            try:
                self._connect(addr, port)
                return
            except Exception as e:
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
                    af, socktype, proto, canonname, sa = res
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
                                   "Error details: %s" % (addr, ex))

    def get_logger(self):
        return self._logger

    """Set logging level

    Args:
        lvl: Log level to set as accepted by logger.setLevel
    """
    def log_set_level(self, lvl):
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
            'id': self._request_id
        }

        if params:
            req['params'] = copy.deepcopy(params)

        self._logger.debug("append request:\n%s\n", json.dumps(req))
        self._reqs.append(req)
        return self._request_id

    def flush(self):
        self._logger.debug("Flushing buffer")
        # TODO: We can drop indent parameter
        reqstr = "\n".join(json.dumps(req, indent=2) for req in self._reqs)
        self._reqs = []
        self._logger.info("Requests:\n%s\n", reqstr)
        self.sock.sendall(reqstr.encode("utf-8"))

    def send(self, method, params=None):
        id = self.add_request(method, params)
        self.flush()
        return id

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

    def call(self, method, params=None):
        self._logger.debug("call('%s')" % method)
        params = {} if params is None else params
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


class JSONRPCGoClient(object):
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
        except OSError:
            raise JSONRPCException(f'Failed to load the Go RPC client at {client_path}')
        lib.spdk_gorpc_free_response.argtypes = [ctypes.POINTER(ctypes.c_char)]
        lib.spdk_gorpc_call.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        lib.spdk_gorpc_call.restype = GoClientResponse

        command_info = {
            "method": method,
            "params": params
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
