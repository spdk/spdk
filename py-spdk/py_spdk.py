import psutil
import re
import os
import subprocess


class pyspdk(object):

    def __init__(self, pname):
        super(pyspdk, self).__init__()
        self.pid = None
        self.pname = pname

    def start_server(self, spdk_dir, server_name):
        if self.is_alive():
            self.init_hugepages(spdk_dir)
            server_dir = os.path.join(spdk_dir, 'app/')
            file_dir = self.search_file(server_dir, server_name)
            print file_dir
            os.chdir(file_dir)
            p = subprocess.Popen(
                'sudo ./%s' % server_name,
                shell=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE)
            out, err = p.communicate()
            return out

    def init_hugepages(self, spdk_dir):
        huge_dir = os.path.join(spdk_dir, 'scripts/')
        file_dir = self.search_file(huge_dir, 'setup.sh')
        print file_dir
        os.chdir(file_dir)
        p = subprocess.Popen(
            'sudo ./setup.sh',
            shell=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out, err = p.communicate()
        return out

    def search_file(self, spdk_dir, file_name):
        for dirpath, dirnames, filenames in os.walk(spdk_dir):
            for filename in filenames:
                if filename == file_name:
                    return dirpath

    def get_process_id(self, pname):
        for proc in psutil.process_iter():
            try:
                pinfo = proc.as_dict(attrs=['pid', 'name'])
                if re.search(self.pname, pinfo.get('name')):
                    self.pid = pinfo.get('pid')
                    return self.pid
            except psutil.NoSuchProcess:
                print "NoSuchProcess:%s" % self.pname
        print "NoSuchProcess:%s" % self.pname
        return self.pid

    def is_alive(self):
        self.pid = self.get_process_id(self.pname)
        if self.pid:
            p = psutil.Process(self.pid)
            if p.is_running():
                return True
        return False

    def exec_rpc(self, method, server='127.0.0.1', port=5260):
        p = subprocess.Popen(
            './rpc.py -s %s -p %s %s' % (server, port, method),
            shell=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out, err = p.communicate()
        return out
