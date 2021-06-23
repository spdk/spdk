#!/usr/bin/env python3

import argparse
import base64
import errno
import json
import socket
import ssl
import sys
try:
    from BaseHTTPServer import HTTPServer, BaseHTTPRequestHandler
except ImportError:
    from http.server import HTTPServer
    from http.server import BaseHTTPRequestHandler
from rpc.client import print_json

rpc_sock = None

parser = argparse.ArgumentParser(description='http(s) proxy for SPDK RPC calls')
parser.add_argument('host', help='Host name / IP representing proxy server')
parser.add_argument('port', help='Port number', type=int)
parser.add_argument('user', help='User name used for authentication')
parser.add_argument('password', help='Password used for authentication')
parser.add_argument('-s', dest='sock', help='RPC domain socket path', default='/var/tmp/spdk.sock')
parser.add_argument('-c', dest='cert', help='SSL certificate')


def print_usage_and_exit(status):
    print('Usage: rpc_http_proxy.py <server IP> <server port> <user name>' +
          ' <password> <SPDK RPC socket (optional, default: /var/tmp/spdk.sock)>')
    sys.exit(status)


def rpc_call(req):
    global rpc_sock

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(rpc_sock)
    sock.sendall(req)

    if 'id' not in json.loads(req.decode('ascii')):
        sock.close()
        return None

    buf = ''
    closed = False
    response = None

    print_json(req.decode('ascii'))

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

    sock.close()

    if not response and len(buf) > 0:
        raise

    print_json(buf)

    return buf


class ServerHandler(BaseHTTPRequestHandler):

    key = ""

    def do_HEAD(self):
        self.send_response(200)
        self.send_header('Content-type', 'text/html')
        self.end_headers()

    def do_AUTHHEAD(self):
        self.send_response(401)
        self.send_header('WWW-Authenticate', 'text/html')
        self.send_header('Content-type', 'text/html')
        self.end_headers()

    def do_INTERNALERROR(self):
        self.send_response(500)
        self.send_header('Content-type', 'text/html')
        self.end_headers()

    def do_POST(self):
        if self.headers['Authorization'] != 'Basic ' + self.key:
            self.do_AUTHHEAD()
        else:
            data_string = self.rfile.read(int(self.headers['Content-Length']))

            try:
                response = rpc_call(data_string)
                if response is not None:
                    self.do_HEAD()
                    self.wfile.write(bytes(response.encode(encoding='ascii')))
            except ValueError:
                self.do_INTERNALERROR()


def main():
    global rpc_sock

    args = parser.parse_args()
    rpc_sock = args.sock

    # encoding user name and password
    key = base64.b64encode((args.user+':'+args.password).encode(encoding='ascii')).decode('ascii')

    try:
        ServerHandler.key = key
        httpd = HTTPServer((args.host, args.port), ServerHandler)
        if args.cert is not None:
            httpd.socket = ssl.wrap_socket(httpd.socket, certfile=args.cert, server_side=True)
        print('Started RPC http proxy server')
        httpd.serve_forever()
    except KeyboardInterrupt:
        print('Shutting down server')
        httpd.socket.close()


if __name__ == '__main__':
    main()
