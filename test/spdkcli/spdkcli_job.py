#!/usr/bin/env python3
import pexpect
import os
import sys
import threading
import socket
import re
import time

class SpdkcliThread(threading.Thread):
    def __init__(self, sock):
        threading.Thread.__init__(self)
        self.data = ""
        self.sock = sock

    def run(self):
        while True:
            self.conn, address = s.accept()
            self.data = self.conn.recv(1024).decode('utf-8')
            #print("data: %s" % self.data)
            if self.data.endswith(u"\n"):
                self.data = self.data.strip()
                p = re.compile('\"(.+?)\"')
                cmd = p.findall(self.data)
                if self.data[-1] != "\"":
                    cmd.append(self.data.rsplit(" ", 1)[1].strip())
                    if cmd[-1] == "False":
                        cmd[-1] = False
                    else:
                        cmd[-1] = True
                #print("cmd: %s" % cmd)
                execute_command(*cmd[0:3])
                self.conn.send("asdas".encode())
                self.conn.close()

    def close(self):
        if self.conn:
            self.conn.close()


def execute_command(cmd, element=None, element_exists=False):
    child.sendline(cmd)
    child.expect("/>")
    if "error response" in child.before.decode():
        print("Error in cmd: %s" % cmd)
        exit(1)
    ls_tree = cmd.split(" ")[0]
    if ls_tree and element:
        child.sendline("ls %s" % ls_tree)
        child.expect("/>")
        print("child: %s" % child.before.decode())
        if element_exists:
            if element not in child.before.decode():
                print("Element %s not in list" % element)
                exit(1)
        else:
            if element in child.before.decode():
                print("Element %s is in list" % element)
                exit(1)


sock = "/var/tmp/spdk.sock"
testdir = os.path.dirname(os.path.realpath(sys.argv[0]))
child = pexpect.spawn(os.path.join(testdir, "../../scripts/spdkcli.py") + " -s %s" % sock)
child.expect(">")
child.sendline("cd /")
child.expect("/>")

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM, 0)
try:
    os.remove("/tmp/spdkcli.sock")
except:
    pass
try:
    s.bind("/tmp/spdkcli.sock")
    s.listen(5)
except Exception as e:
    print("Failed to create socket: %s" % e)
    sys.exit()
spdkclithread = SpdkcliThread(s)
spdkclithread.start()
