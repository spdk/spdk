from py_spdk import PySPDK
from nvmf_client import NVMfTgt


pyspdk = PySPDK('nvmf_tgt')
if pyspdk.is_alive():
    nvmf_tgt = NVMfTgt()
    nvmf_tgt = NVMfTgt('10.0.2.15')
    subsystems = nvmf_tgt.get_nvmf_subsystems()
    print type(subsystems)
else:
    pyspdk.init_hugepages('/home/wewe/spdk/scripts/')
    pyspdk.start_server('/home/wewe/spdk/app/')
