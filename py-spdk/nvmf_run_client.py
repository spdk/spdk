from nvmf_client import NvmfTgt
from py_spdk import pyspdk


py = pyspdk('nvmf_tgt')
if py.is_alive():
    nvmf_tgt = NvmfTgt(py)
    my_bdevs = nvmf_tgt.get_bdevs()
    for index in range(len(my_bdevs)):
        my_bdev = my_bdevs[index]
        if(index == 0):
            print my_bdev
else:
    raise Exception('nvmf_tgt server is dead.')

