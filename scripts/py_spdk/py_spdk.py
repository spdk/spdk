import subprocess
import psutil
import re
import os


def _search_file(search_dir, search_file):
    for dirpath, dirnames, filenames in os.walk(search_dir):
        for filename in filenames:
            if filename == search_file:
                return dirpath
    return None


class PySPDK(object):
    def __init__(
            self,
            target_server='nvmf_tgt'):
        super(PySPDK, self).__init__()
        self.target_server = target_server

    def is_alive(self):
        for proc in psutil.process_iter():
            pinfo = proc.as_dict(attrs=['pid', 'cmdline'])
            if re.search(self.target_server, str(pinfo.get('cmdline'))):
                p = psutil.Process(pinfo.get('pid'))
                if p.is_running():
                    return True
        return False

    def init_hugepages(self, hugepages_dir='/home/wewe/spdk/scripts/'):
        file_dir = _search_file(hugepages_dir, 'setup.sh')
        try:
            os.chdir(file_dir)
        except BaseException:
            raise Exception('the hugepages dir %s not found.' % hugepages_dir)
        cmd = "sudo ./setup.sh"
        cmd_list = cmd.split(" ")
        p = subprocess.Popen(
            cmd_list,
            shell=False, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out, err = p.communicate()
        return out

    def start_server(self, app_dir='home/wewe/spdk/app/', conf_dir=None):
        file_dir = _search_file(app_dir, self.target_server)
        try:
            os.chdir(file_dir)
        except BaseException:
            raise Exception('the app dir %s not found.' % app_dir)
        if conf_dir:
            cmd = 'sudo ./%s -c %s' % (self.target_server, conf_dir)
        else:
            cmd = 'sudo ./%s' % self.target_server
        cmd_list = cmd.split(" ")
        p = subprocess.Popen(
            cmd_list,
            shell=False, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out, err = p.communicate()
        return out

    def stop_server(self):
        for proc in psutil.process_iter():
            pinfo = proc.as_dict(attrs=['pid', 'cmdline'])
            if re.search(self.target_server, str(pinfo.get('cmdline'))):
                p = psutil.Process(pinfo.get('pid'))
                if p.is_running():
                    try:
                        p.kill()
                    except psutil.AccessDenied:
                        print('Non-root user does not have permission to operate.')
                    return True
                else:
                    return False
        return False
