import json
import socket
import time

try:
    from shlex import quote
except ImportError:
    from pipes import quote


def print_dict(d):
    print json.dumps(d, indent=2)


def print_array(a):
    print " ".join((quote(v) for v in a))


class JSONRPCClient(object):
    def __init__(self, addr, port=None, verbose=False, timeout=60.0):
        self.verbose = verbose
        self.timeout = timeout
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

        self.sock.sendall(reqstr)
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

                buf += newdata
                response = json.loads(buf)
            except socket.timeout:
                break
            except ValueError:
                continue  # incomplete response; keep buffering
            break

        if not response:
            if method == "kill_instance":
                exit(0)
            if closed:
                print "Connection closed with partial response:"
            else:
                print "Timeout while waiting for response:"
            print buf
            exit(1)

        if 'error' in response:
            print "Got JSON-RPC error response"
            print "request:"
            print_dict(json.loads(reqstr))
            print "response:"
            print_dict(response['error'])
            exit(1)

        if verbose:
            print("response:")
            print(json.dumps(response, indent=2))

        return response['result']
