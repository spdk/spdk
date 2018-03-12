import subprocess
import psutil
import re
import os


class PySPDK(object):

    def __init__(
            self,
            target_server='nvmf_tgt',
            spdk_dir='/home/wewe/spdk/'):
        super(PySPDK, self).__init__()
        self.target_server = target_server
        self.spdk_dir = spdk_dir

    def is_alive(self):
        for proc in psutil.process_iter():
            pinfo = proc.as_dict(attrs=['pid', 'cmdline'])
            if re.search(self.target_server, str(pinfo.get('cmdline'))):
                p = psutil.Process(pinfo.get('pid'))
                if p.is_running():
                    return True
        return False

    def start_server(self):
        self._init_hugepages()
        server_dir = os.path.join(self.spdk_dir, 'app/')
        file_dir = self._search_file(server_dir, self.target_server)
        os.chdir(file_dir)
        p = subprocess.Popen(
            'sudo ./%s' % self.target_server,
            shell=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out, err = p.communicate()
        return out

    def _init_hugepages(self):
        huge_dir = os.path.join(self.spdk_dir, 'scripts/')
        file_dir = self._search_file(huge_dir, 'setup.sh')
        os.chdir(file_dir)
        p = subprocess.Popen(
            'sudo ./setup.sh',
            shell=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out, err = p.communicate()
        return out

    @staticmethod
    def _search_file(search_dir, search_file):
        for dirpath, dirnames, filenames in os.walk(search_dir):
            for filename in filenames:
                if filename == search_file:
                    return dirpath
        else:
            raise Exception('%s not found.' % search_file)
