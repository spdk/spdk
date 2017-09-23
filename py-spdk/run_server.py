from py_spdk import pyspdk


def run_server():
    py = pyspdk('nvme')
    py.start_server("/home/wewe/spdk/", 'nvmf_tgt')

run_server()
