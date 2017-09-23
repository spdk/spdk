# py-spdk

## Dependencies

* The py-spdk is currently in favour of SPDK v17.07 and DPDK v17.08.
* The py-spdk uses Protobuf v3.2.0 to write spdk.proto description of the data structure you wish to store which implements automatic encoding and parsing of the protobuf data with an effecient binary format.

## Problem description

As we all know, SPDK is a high-performance kit written in c. It is hard for management-level applications written in python to connect with SPDK-based app (as: nvmf_tgt, etc.) directly. For this reason, developers should be able to provide a client for callers to make convenient use of SPDK-based app. The py-spdk is such a python client that is designed for the upper-level management applications to communicate more fully and get data returned by SPDK-based app. 

## Use case

The py-spdk is designed for all of the management-level applications to employ SPDK-based app better. For example:

* As an acceleration framework, the OpenStack Cyborg should be able to find and report the ability of backend accelerators including hardware  (like FPGA) and software (like DPDK, SPDK) acceleration resources. Based on this reason, the py-spdk can make the Cyborg interact with SPDK-based app (as: nvmf_tgt, vhost, etc.). According to the configuration file provided by the Cyborg, the py-spdk will to be informed whether the SPDK is installed. If so, then it will judge further whether the nvmf_tgt process is started. Suppose the nvmf_tgt server is not alive, it will execute the functions of initialization and startup for server. Once the server is successfully started, the py-spdk can obtain what it requires, and then do other operations. 

## Proposed change

In general, the goal is to develop the py-spdk that supports the management and the use of SPDK.

### Design workflow

* Modify the rpc.py provided by SPDK, and use init() to encapsulate most of its original functions, and then execute rpc.py when it’s invoked by pyspdk.py.
* The pyspdk.py will communicate with the SPDK-based app (such as: nvmf_tgt, vhost, iscsi_tgt, etc.) through the rpc.py. The pyspdk.py provides some fundamental functions including the judgment of process status, hugepages initialization and server connection.  

		class pyspdk(object):
            def start_server(self, spdk_dir, server_name)
        
            def init_hugepages(self, spdk_dir)
        
            def search_file(self, spdk_dir, file_name)
        
            def get_process_id(self, pname)
        
            def is_alive(self)
        
            def exec_rpc(self, method, server='127.0.0.1', port=5260, sub_args=None)
        
            def close_server(self, spdk_dir, server_name)
                pass

* The py-spdk has been implemented two kinds of client to obtain information (such as: get_luns, get_interfaces, get_vhost_blk_controller, etc) from SPDK-based app which are nvmf_client and vhost_client. The third SPDK-based app (iscsi_client) will be added later.

  1. The nvmf_client has exposed a set of functions to the upper management application (such as: OpenStack Cyborg). If required, they can call the nvmf_client to do some operations of nvmf_tgt.

         class NvmfTgt(object):

             def get_rpc_methods(self)
        
             def get_bdevs(self)
       
             def delete_bdev(self, name)

             def kill_instance(self, sig_name)

             def construct_aio_bdev(self, filename, name, block_size)
       
             def construct_error_bdev(self, basename)
        
             def construct_nvme_bdev(self, name, trtype, traddr, adrfam=None, trsvcid=None,subnqn=None)
        
             def construct_null_bdev(self, name, total_size, block_size):
        
             def construct_malloc_bdev(self, total_size, block_size):

             def delete_nvmf_subsystem(self, nqn):
        
             def construct_nvmf_subsystem(self, nqn, listen, hosts, serial_number, namespaces)

             def get_nvmf_subsystems(self)

  2. The vhost_client has exposed a set of functions to the upper management application (such as: OpenStack Cyborg). If required, they can call the vhost_client to do some operations of vhost.

         class VhostTgt(object):
	 
             def get_rpc_methods(self)
	     
	         def get_scsi_devices(self)
	     
	         def get_luns(self)
	     
	         def add_ip_address(self, ifc_index, ip_addr)
	     
	         def delete_ip_address(self, ifc_index, ip_addr)
	     
	         def get_bdevs(self)
	     
             def delete_bdev(self, name)
	     
             def kill_instance(self, sig_name)
	     
             def construct_aio_bdev(self, filename, name, block_size)
	     
             def construct_error_bdev(self, basename)
	     
             def construct_nvme_bdev(self, name, trtype, traddr, adrfam=None, trsvcid=None,subnqn=None)
	     
             def construct_null_bdev(self, name, total_size, block_size):
	     
             def construct_malloc_bdev(self, total_size, block_size):

### Returned result
#### Start nvmf_tgt server example: get_bdevs()

![py-spdk](https://github.com/hellowaywewe/py-spdk/blob/master/get_bdevs.png)

#### Start vhost server example: get_luns()

![py-spdk](https://github.com/hellowaywewe/py-spdk/blob/master/get_luns.png)


