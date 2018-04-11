import json
import socket
import time


def print_dict(d):
    print(json.dumps(d, indent=2))


class JSONRPCException(Exception):
    def __init__(self, message):
        self.message = message


class JSONRPCClient(object):
    def __init__(self, addr, port=None, verbose=False, timeout=60.0):
        self.verbose = verbose
        self.timeout = timeout
        try:
            if addr.startswith('/'):
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
        self.sock.close()

    def call(self, method, params={}, verbose=False):
        req = {}
        req['jsonrpc'] = '2.0'
        req['method'] = method
        req['id'] = 1
        if (params):
            req['params'] = params
        reqstr = json.dumps(req)

        verbose = verbose or self.verbose

        if verbose:
            print("request:")
            print(json.dumps(req, indent=2))

        self.sock.sendall(reqstr.encode("utf-8"))
        buf = ''
        closed = False
        response = {}
        start_time = time.clock()

        while not closed:
            try:
                timeout = self.timeout - (time.clock() - start_time)
                if timeout <= 0.0:
                    break

                self.sock.settimeout(timeout)
                newdata = self.sock.recv(4096)
                if (newdata == b''):
                    closed = True

                buf += newdata.decode("utf-8")
                response = json.loads(buf)
            except socket.timeout:
                break
            except ValueError:
                continue  # incomplete response; keep buffering
            break

        if not response:
            if method == "kill_instance":
                return {}
            if closed:
                msg = "Connection closed with partial response:"
            else:
                msg = "Timeout while waiting for response:"
            msg = "\n".join([msg, buf])
            raise JSONRPCException(msg)

        if 'error' in response:
            msg = "\n".join(["Got JSON-RPC error response",
                             "request:",
                             json.dumps(req, indent=2),
                             "response:",
                             json.dumps(response['error'], indent=2)])
            raise JSONRPCException(msg)

        if verbose:
            print("response:")
            print(json.dumps(response, indent=2))

        return response['result']
