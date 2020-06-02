import json
import socket
import time
import os
import logging
import copy


def print_dict(d):
    print(json.dumps(d, indent=2))


def print_json(s):
    print(json.dumps(s, indent=2).strip('"'))


class JSONRPCException(Exception):
    def __init__(self, message):
        self.message = message


class JSONRPCClient(object):
    def __init__(self, addr, port=None, timeout=60.0, **kwargs):
        self.sock = None
        ch = logging.StreamHandler()
        ch.setFormatter(logging.Formatter('%(levelname)s: %(message)s'))
        ch.setLevel(logging.DEBUG)
        self._logger = logging.getLogger("JSONRPCClient(%s)" % addr)
        self._logger.addHandler(ch)
        self.log_set_level(kwargs.get('log_level', logging.ERROR))
        connect_retries = kwargs.get('conn_retries', 0)

        self.timeout = timeout
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
            if os.path.exists(addr):
                self._logger.debug("Trying to connect to UNIX socket: %s", addr)
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(addr)
            elif port:
                if ':' in addr:
                    self._logger.debug("Trying to connect to IPv6 address addr:%s, port:%i", addr, port)
                    for res in socket.getaddrinfo(addr, port, socket.AF_INET6, socket.SOCK_STREAM, socket.SOL_TCP):
                        af, socktype, proto, canonname, sa = res
                    self.sock = socket.socket(af, socktype, proto)
                    self.sock.connect(sa)
                else:
                    self._logger.debug("Trying to connect to IPv4 address addr:%s, port:%i'", addr, port)
                    self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    self.sock.connect((addr, port))
            else:
                raise socket.error("Unix socket '%s' does not exist" % addr)
        except socket.error as ex:
            raise JSONRPCException("Error while connecting to %s\n"
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

    def call(self, method, params={}):
        self._logger.debug("call('%s')" % method)
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
