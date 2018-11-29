import json
import socket
import time
import os


def print_dict(d):
    print(json.dumps(d, indent=2))


class JSONRPCException(Exception):
    def __init__(self, message):
        self.message = message


class JSONRPCClient(object):
    def __init__(self, addr, port=None, verbose=False, timeout=60.0):
        self.sock = None
        self.verbose = verbose
        self.timeout = timeout
        self.request_id = 0
        try:
            if os.path.exists(addr):
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(addr)
            elif ':' in addr:
                for res in socket.getaddrinfo(addr, port, socket.AF_INET6, socket.SOCK_STREAM, socket.SOL_TCP):
                    af, socktype, proto, canonname, sa = res
                self.sock = socket.socket(af, socktype, proto)
                self.sock.connect(sa)
            else:
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.connect((addr, port))
        except socket.error as ex:
            raise JSONRPCException("Error while connecting to %s\n"
                                   "Error details: %s" % (addr, ex))

    def __del__(self):
        if self.sock:
            self.sock.close()

    def send(self, method, params=None):
        self.request_id += 1
        req = {
            'jsonrpc': '2.0',
            'method': method,
            'id': self.request_id
        }

        if params:
            req['params'] = params

        reqstr = json.dumps(req)
        if self.verbose:
            print("request:")
            print(json.dumps(req, indent=2))

        self.sock.sendall(reqstr.encode("utf-8"))
        return req

    def recv(self):
        start_time = time.clock()
        response = None
        buf = ''
        while not response:
            try:
                timeout = self.timeout - (time.clock() - start_time)
                self.sock.settimeout(timeout)
                newdata = self.sock.recv(4096)
                if not newdata:
                    self.sock.close()
                    self.sock = None
                    raise JSONRPCException("Connection closed with partial response:\n%s\n" % buf)
                buf += newdata.decode("utf-8")
                response = json.loads(buf)
            except socket.timeout:
                break  # throw exception after loop to avoid Python freaking out about nested exceptions
            except ValueError:
                continue  # incomplete response; keep buffering

        if not response:
            raise JSONRPCException("Timeout while waiting for response:\n%s\n" % buf)

        if self.verbose:
            print("response:")
            print(json.dumps(response, indent=2))

        if 'error' in response:
            msg = "\n".join(["Got JSON-RPC error response",
                             "response:",
                             json.dumps(response['error'], indent=2)])
            raise JSONRPCException(msg)
        return response

    def call(self, method, params=None):
        self.send(method, params)
        try:
            return self.recv()['result']
        except JSONRPCException as e:
            """ Don't expect response to kill """
            if not self.sock and method == "kill_instance":
                return {}
            else:
                raise e
