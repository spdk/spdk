import json
import socket
import time
import os
import threading

from threading import Condition, Lock


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
        self.handler = False
        self.handler_lock = Lock()
        self.responses = {}
        self.recv_cond = Condition()
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
        print("Stopping")
        self.is_running = False
        self.recv_cond.acquire()
        self.recv_cond.notify()
        self.recv_cond.release()
        self.thread.join()

    def add_handler(self, handler):
        """ Add new listener for asynchronious RPC

        NOTE: This implementation allows to add only one listener
        """
        self.handler_lock.acquire()
        self.handler = handler
        self.handler_lock.release()
        if not self.is_running:
            self.start()

    def remove_handler(self, handler):
        self.handler_lock.acquire()
        self.handler = False
        self.handler_lock.release()
        self.stop()

    def fire(self, response):
        self.handler_lock.acquire()
        if self.handler:
            self.handler(response)
        self.handler_lock.release()

    def recv(self):
        buf = ''
        closed = False
        response = {}
        start_time = time.clock()

        while not closed:
            try:
                timeout = self.timeout - (time.clock() - start_time)
                if timeout <= 0.0:
                    break

                #self.sock.settimeout(timeout)
                self.sock.settimeout(1)
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

        return response

    def _recv_handler(self):
        self.is_running = True
        while self.is_running:
            response = self.recv()
            if response:
                self.recv_cond.acquire()
                self.responses[response['id']] = response
                self.recv_cond.notify()
                self.recv_cond.release()
                self.fire(response)

    def get_response(self, id=0, wait=True):
        """
        Get response with specified id
        """

        if not self.is_running:
            return self.recv()

        while self.is_running and wait:
            self.recv_cond.acquire()
            response = self.responses.pop(id, False)
            if response:
                self.recv_cond.release()
                return response

            self.recv_cond.wait(self.timeout)
            self.recv_cond.release()
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
