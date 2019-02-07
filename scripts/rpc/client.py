import json
import socket
import time
import os
import threading

from threading import Condition


def print_dict(d):
    print(json.dumps(d, indent=2))


class JSONRPCException(Exception):
    def __init__(self, message):
        self.message = message


class JSONRPCClient(object):
    def __init__(self, addr, port=None, verbose=False, timeout=60.0):
        self.verbose = verbose
        self.timeout = timeout
        self.request_id = 0
        self.responses = {}
        self.condition = Condition()
        self.is_running = False
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
        self.sock.close()

    def start(self):
        self.thread = threading.Thread(target=self._recv_handler)
        self.thread.start()

    def stop(self):
        self.is_running = False
        self.condition.acquire()
        self.condition.notify()
        self.condition.release()
        self.thread.join()

    def add_handler(self, handler):
        """ Add new listener for asynchronious RPC

        NOTE: This implementation allows to add only one listener
        """
        self.handler = handler
        if not self.is_running:
            self.start()

    def remove_handler(self, handler):
        self.handler = False
        self.stop()

    def fire(self, response):
        if self.handler:
            self.handler(response)

    def recv_response(self):
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

        if response:
            self.fire(response)

        return response

    def _recv_handler(self):
        self.is_running = True
        while self.is_running:
            response = self.recv_response()
            if response:
                self.condition.acquire()
                self.responses[response['id']] = response
                self.condition.notify()
                self.condition.release()

    def get_response(self, id=0, wait=True):
        """
        Get response with specified id
        """

        if not self.is_running:
            return self.recv_response()

        while self.is_running and wait:
            self.condition.acquire()
            response = self.responses.pop(id, False)
            if response:
                self.condition.release()
                return response

            self.condition.wait(self.timeout)
            self.condition.release()
        return False

    def call(self, method, params={}, verbose=False):
        self.request_id += 1
        req = {}
        req['jsonrpc'] = '2.0'
        req['method'] = method
        req['id'] = self.request_id
        if (params):
            req['params'] = params
        reqstr = json.dumps(req)

        verbose = verbose or self.verbose

        if verbose:
            print("request:")
            print(json.dumps(req, indent=2))

        self.sock.sendall(reqstr.encode("utf-8"))

        response = self.get_response(req['id'])

        if not response:
            if method == "kill_instance":
                return {}
            else:
                msg = "Timeout while waiting for response:"
            raise JSONRPCException(msg)

        if 'error' in response:
            msg = "\n".join(["Got JSON-RPC error response",
                             "request:",
                             json.dumps(req, indent=2),
                             "response:",
                             json.dumps(response['error'], indent=2)])
            raise JSONRPCException(msg)

        return response['result']
