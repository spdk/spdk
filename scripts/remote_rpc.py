#!/usr/bin/env python3

import base64
import json
import socket
import sys
try:
    from BaseHTTPServer import HTTPServer, BaseHTTPRequestHandler
except ImportError:
    from http.server import HTTPServer
    from http.server import BaseHTTPRequestHandler

sock = None


def rpc_call(method, params=None):
    global sock
    req = {'jsonrpc': '2.0', 'method': method, 'id': 1}
    if params is not None:
        req['params'] = params
    reqstr = json.dumps(req)
    sock.sendall(reqstr.encode(encoding='ascii'))
    buf = ''
    closed = False
    response = {}
    while not closed:
        newdata = sock.recv(1024)
        if (newdata == b''):
            closed = True
        buf += newdata.decode('ascii')
        try:
            response = json.loads(buf)
        except ValueError:
            continue  # incomplete response; keep buffering
        break
    if not response:
        print("ERROR: SPDK target did not respond")
        return None

    if 'error' in response:
        print("ERROR: SPDK target responded with error")
        return None

    if 'result' not in response.keys():
        return None

    return response['result']


class ServerHandler(BaseHTTPRequestHandler):

    key = ""

    def respone_to_bytes(self, response):
        return bytes(json.dumps(response).encode(encoding='ascii'))

    def do_HEAD(self):
        self.send_response(200)
        self.send_header('Content-type', 'text/html')
        self.end_headers()

    def do_AUTHHEAD(self):
        self.send_response(401)
        self.send_header('WWW-Authenticate', 'text/html')
        self.send_header('Content-type', 'text/html')
        self.end_headers()

    def do_BADREQHEAD(self):
        self.send_response(400)
        self.send_header('Content-type', 'text/html')
        self.end_headers()

    def do_POST(self):
        if self.headers['Authorization'] is None:
            self.do_AUTHHEAD()
            response = {'response': 'Missing user name or password'}
            self.wfile.write(self.respone_to_bytes(response))
        elif self.headers['Authorization'] == 'Basic '+self.key:
            data_string = self.rfile.read(int(self.headers['Content-Length']))
            data = json.loads(data_string.decode('ascii'))
            if 'method' not in data.keys():
                self.do_BADREQHEAD()
                response = {'response': 'Wrong JSON syntax. Please provide \'method\' object'}
            else:
                if 'params' not in data.keys():
                    params = None
                else:
                    params = data['params']
                response = rpc_call(data['method'], params)
                if response is None:
                    self.do_BADREQHEAD()
                    response = {'response': 'Error getting response from SPDK'}
                else:
                    self.do_HEAD()
            self.wfile.write(self.respone_to_bytes(response))
        else:
            self.do_AUTHHEAD()
            response = {'response': 'Wrong user name or password'}
            self.wfile.write(self.respone_to_bytes(response))


def main():
    global sock

    if len(sys.argv) == 1 or sys.argv[1] == '-h':
        print('Usage: remote_rpc.py <server IP> <server port> <user name>' +
              ' <password> <RPC listen address (optional, default: /var/tmp/spdk.sock)>')
        exit()

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    if len(sys.argv) == 6:
        rpc_sock = sys.argv[5]
    else:
        rpc_sock = '/var/tmp/spdk.sock'
    sock.connect(rpc_sock)

    # encoding user name and password
    key = base64.b64encode((sys.argv[3]+':'+sys.argv[4]).encode(encoding='ascii')).decode('ascii')

    try:
        ServerHandler.key = key
        httpd = HTTPServer((sys.argv[1], int(sys.argv[2])), ServerHandler)
        print('Started remote RPC server')
        httpd.serve_forever()
    except KeyboardInterrupt:
        print('Shutting down server')
        httpd.socket.close()
        sock.close()


if __name__ == '__main__':
    main()
