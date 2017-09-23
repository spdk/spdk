#!/usr/local/python3.5

'''pyenv and pyspdk are modules which are converted form C to Python through swig'''
import pyenv
import pyspdk

'''trid is a structure of C'''
trid=pyspdk.spdk_nvme_transport_id();

'''initial DPDK'''
def init_env():
    opts = pyenv.spdk_env_opts();
    pyenv.spdk_env_opts_init(opts);
    opts.name = "py-spdk";
    opts.shm_id = 0;
    pyenv.spdk_env_init(opts);
    printf("Initializing NVMe Controllers\n");

def discover(transtype):
    trid.trtype = transtype;
    cb_ctx=None;
    rc = pyspdk.spdk_nvme_probe(trid, cb_ctx, probe_cb, attach_cb, remove_cb);
    if (rc != 0) {
        print("spdk_nvme_probe() failed\n");
    }

def attach_cb(trantype):
    trid.trtype = transtype;
    pyspdk.attach_cb(cb_ctx, trid, ctrlr, opts);


def probe_cb(trid):
    trid.trtype = transtype;
    pyspdk.probe_cb(cb_ctx, trid, ctrlr, opts);

def remove_cb(trid):
    pyspdk.remove_cb(cb_ctx, trid, ctrlr, opts);

def spdk_nvme_detch(cb_ctx, trid, ctrlr, opts):
    trid.trtype = transtype;
    pyspdk.spdk_nvme_detch(cb_ctx, trid, ctrlr, opts);

def spdk_update():

