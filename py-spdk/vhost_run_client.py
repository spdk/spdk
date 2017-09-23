from py_spdk import pyspdk
from vhost_client import VhostTgt


py = pyspdk('vhost')
if py.is_alive():
    vhost_tgt = VhostTgt(py)
    my_luns = vhost_tgt.get_luns()
    for index in range(len(my_luns)):
        my_lun = my_luns[index]
        if(index == 0):
            print my_lun
else:
    raise Exception('vhost_tgt server is dead.')


