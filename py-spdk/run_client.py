from py_spdk import pyspdk


def run_client():
    py = pyspdk('nvme')
    if py.is_alive():
        print py.exec_rpc('get_rpc_methods', '10.0.2.15')

run_client()
