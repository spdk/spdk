Introduction
============

This is a vagrant environment for SPDK with support for Ubuntu 16.04 and
Centos 7.2. Be sure to use vagrant 1.9.4 or higher.

The VM builds SPDK and DPDK from source which can be located at /spdk and /dpdk.

Note: If you are behind a corporate firewall, set http_proxy and https_proxy in
your environment before trying to start up the VM.  Also make sure that you
have installed the optional vagrant module 'vagrant-proxyconf'.

VM Configuration
================

This vagrant environment creates a VM based on environment variables found in ./env.sh
To use, edit env.sh then

    cd scripts/vagrant
    source ./env.sh
    vagrant up

At this point you can use "vagrant ssh" to ssh into the VM. The /spdk directory is
sync'd from the host system and the build is automatically done.  Other notable files:

    build.sh : is executed on the VM automatically when provisioned
    Vagrantfile : startup parameters/commands for the VM

The idea behind our use of vagrant is to provide a quick way to get a basic NVMe enabled
sandbox going without the need for any special hardware. The few commands we mention
here are enough to get you up and running, for additional support just use the help
function to learn how to destroy, restart, etc.  Further below is sample output from
a successful VM launch and execution of the NVMe hello world example application.

    vagrant --help

By default, the VM created is/has:
- Ubuntu 16.04
- 2 vCPUs
- 4G of RAM
- 2 NICs (1 x NAT - host access, 1 x private network)

Providers
=========

Currently only the Virtualbox provider is supported.

Hello World
===========

user@dev-system:~/spdk/scripts/vagrant$ vagrant up
Bringing machine 'default' up with 'virtualbox' provider...
==> default: Importing base box 'puppetlabs/ubuntu-16.04-64-nocm'...
==> default: Matching MAC address for NAT networking...
==> default: Setting the name of the VM: vagrant_default_1495560811852_61170
==> default: Fixed port collision for 22 => 2222. Now on port 2200.
==> default: Clearing any previously set network interfaces...
==> default: Preparing network interfaces based on configuration...
    default: Adapter 1: nat
    default: Adapter 2: hostonly
==> default: Forwarding ports...
    default: 22 (guest) => 2200 (host) (adapter 1)
==> default: Running 'pre-boot' VM customizations...
==> default: Booting VM...
==> default: Waiting for machine to boot. This may take a few minutes...
    default: SSH address: 127.0.0.1:2200
    default: SSH username: vagrant
    default: SSH auth method: private key
    default:
    default: Vagrant insecure key detected. Vagrant will automatically replace
    default: this with a newly generated keypair for better security.
    default:
    default: Inserting generated public key within guest...
    default: Removing insecure key from the guest if it's present...
    default: Key inserted! Disconnecting and reconnecting using new SSH key...
==> default: Machine booted and ready!
==> default: Checking for guest additions in VM...
==> default: Configuring and enabling network interfaces...
==> default: Configuring proxy for Apt...
==> default: Configuring proxy environment variables...
==> default: Rsyncing folder: /home/peluse/spdk/ => /spdk
==> default: Mounting shared folders...
    default: /vagrant => /home/peluse/spdk/scripts/vagrant
==> default: Running provisioner: shell...
    default: Running: /tmp/vagrant-shell20170523-8047-1o9amtc.sh
==> default: vm.nr_hugepages = 1024
==> default: Get:1 http://us.archive.ubuntu.com/ubuntu xenial InRelease [247 kB]
<<<< whole bunch of these like previous line deleted >>>>
==> default: Fetched 2,569 kB in 2s (1,053 kB/s)
==> default: Reading package lists...
==> default: Reading package lists...
==> default: Building dependency tree...
==> default:
==> default: Reading state information...
==> default: Calculating upgrade...
==> default: The following packages have been kept back:
==> default:   linux-generic linux-headers-generic linux-image-generic
==> default: The following packages will be upgraded:
==> default:   accountsservice apparmor apt apt-transport-https apt-utils base-files bash
<<<< whole bunch of these like previous line deleted >>>>
==> default: 167 upgraded, 0 newly installed, 0 to remove and 3 not upgraded.
==> default: Need to get 124 MB of archives.
==> default: After this operation, 39.5 MB of additional disk space will be used.
==> default: Get:1 http://us.archive.ubuntu.com/ubuntu xenial-updates/main amd64 base-files amd64 9.4ubuntu4.4 [60.2 kB]
<<<< whole bunch of these like previous line deleted >>>>
==> default: Processing triggers for libc-bin (2.23-0ubuntu3) ...
==> default: Processing triggers for man-db (2.7.5-1) ...
==> default: Setting up libitm1:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up libcc1-0:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up binutils (2.26.1-1ubuntu1~16.04.3) ...
==> default: Setting up libgomp1:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up libatomic1:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up libasan2:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up liblsan0:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up libtsan0:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up libubsan0:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up libcilkrts5:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up libmpx0:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up libquadmath0:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up libgcc-5-dev:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up cpp-5 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up gcc-5 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up libssl1.0.0:amd64 (1.0.2g-1ubuntu4.6) ...
==> default: Setting up libstdc++-5-dev:amd64 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up g++-5 (5.4.0-6ubuntu1~16.04.4) ...
==> default: Setting up g++ (4:5.3.1-1ubuntu1) ...
==> default: update-alternatives:
==> default: using /usr/bin/g++ to provide /usr/bin/c++ (c++) in auto mode
==> default: Setting up libaio1:amd64 (0.3.110-2) ...
==> default: Setting up libaio-dev (0.3.110-2) ...
==> default: Setting up libcunit1:amd64 (2.1-3-dfsg-2) ...
==> default: Setting up libcunit1-dev (2.1-3-dfsg-2) ...
==> default: Setting up zlib1g-dev:amd64 (1:1.2.8.dfsg-2ubuntu4.1) ...
==> default: Setting up libssl-dev:amd64 (1.0.2g-1ubuntu4.6) ...
==> default: Setting up libssl-doc (1.0.2g-1ubuntu4.6) ...
==> default: Processing triggers for libc-bin (2.23-0ubuntu3) ...
==> default: Creating CONFIG.local...
==> default: done.
==> default: Type 'make' to build.
==> default: Configuration done
==> default: make[3]: Entering directory '/spdk/dpdk'
==> default: == Build lib
==> default: == Build lib/librte_compat
==> default: == Build lib/librte_eal
==> default: == Build lib/librte_eal/common
==> default:   SYMLINK-FILE include/rte_compat.h
==> default:   SYMLINK-FILE include/generic/rte_atomic.h
==> default:   SYMLINK-FILE include/generic/rte_byteorder.h
==> default:   SYMLINK-FILE include/generic/rte_cycles.h
==> default:   SYMLINK-FILE include/generic/rte_prefetch.h
==> default:   SYMLINK-FILE include/generic/rte_spinlock.h
==> default:   SYMLINK-FILE include/generic/rte_memcpy.h
==> default:   SYMLINK-FILE include/generic/rte_cpuflags.h
==> default:   SYMLINK-FILE include/generic/rte_rwlock.h
==> default:   SYMLINK-FILE include/generic/rte_vect.h
==> default:   SYMLINK-FILE include/generic/rte_io.h
==> default:   SYMLINK-FILE include/rte_branch_prediction.h
==> default:   SYMLINK-FILE include/rte_common.h
==> default:   SYMLINK-FILE include/rte_debug.h
==> default:   SYMLINK-FILE include/rte_eal.h
==> default:   SYMLINK-FILE include/rte_errno.h
==> default:   SYMLINK-FILE include/rte_launch.h
==> default:   SYMLINK-FILE include/rte_lcore.h
==> default:   SYMLINK-FILE include/rte_log.h
==> default:   SYMLINK-FILE include/rte_memory.h
==> default:   SYMLINK-FILE include/rte_memzone.h
==> default:   SYMLINK-FILE include/rte_pci.h
==> default:   SYMLINK-FILE include/rte_per_lcore.h
==> default:   SYMLINK-FILE include/rte_random.h
==> default:   SYMLINK-FILE include/rte_tailq.h
==> default:   SYMLINK-FILE include/rte_interrupts.h
==> default:   SYMLINK-FILE include/rte_alarm.h
==> default:   SYMLINK-FILE include/rte_string_fns.h
==> default:   SYMLINK-FILE include/rte_version.h
==> default:   SYMLINK-FILE include/rte_eal_memconfig.h
==> default:   SYMLINK-FILE include/rte_malloc_heap.h
==> default:   SYMLINK-FILE include/rte_hexdump.h
==> default:   SYMLINK-FILE include/rte_devargs.h
==> default:   SYMLINK-FILE include/rte_bus.h
==> default:   SYMLINK-FILE include/rte_dev.h
==> default:   SYMLINK-FILE include/rte_vdev.h
==> default:   SYMLINK-FILE include/rte_pci_dev_feature_defs.h
==> default:   SYMLINK-FILE include/rte_pci_dev_features.h
==> default:   SYMLINK-FILE include/rte_malloc.h
==> default:   SYMLINK-FILE include/rte_keepalive.h
==> default:   SYMLINK-FILE include/rte_time.h
==> default:   SYMLINK-FILE include/rte_rwlock.h
==> default:   SYMLINK-FILE include/rte_memcpy.h
==> default:   SYMLINK-FILE include/rte_cycles.h
==> default:   SYMLINK-FILE include/rte_spinlock.h
==> default:   SYMLINK-FILE include/rte_atomic_32.h
==> default:   SYMLINK-FILE include/rte_vect.h
==> default:   SYMLINK-FILE include/rte_prefetch.h
==> default:   SYMLINK-FILE include/rte_byteorder_32.h
==> default:   SYMLINK-FILE include/rte_rtm.h
==> default:   SYMLINK-FILE include/rte_atomic_64.h
==> default:   SYMLINK-FILE include/rte_cpuflags.h
==> default:   SYMLINK-FILE include/rte_byteorder_64.h
==> default:   SYMLINK-FILE include/rte_atomic.h
==> default:   SYMLINK-FILE include/rte_io.h
==> default:   SYMLINK-FILE include/rte_byteorder.h
==> default: == Build lib/librte_eal/linuxapp
==> default: == Build lib/librte_eal/linuxapp/eal
==> default:   CC eal.o
==> default:   CC eal_hugepage_info.o
==> default:   CC eal_memory.o
==> default:   CC eal_thread.o
==> default:   CC eal_log.o
==> default:   CC eal_vfio.o
==> default:   CC eal_vfio_mp_sync.o
==> default:   CC eal_pci.o
==> default:   CC eal_pci_uio.o
==> default:   CC eal_pci_vfio.o
==> default:   CC eal_debug.o
==> default:   CC eal_lcore.o
==> default:   CC eal_timer.o
==> default:   CC eal_interrupts.o
==> default:   CC eal_alarm.o
==> default:   CC eal_common_lcore.o
==> default:   CC eal_common_timer.o
==> default:   CC eal_common_memzone.o
==> default:   CC eal_common_log.o
==> default:   CC eal_common_launch.o
==> default:   CC eal_common_vdev.o
==> default:   CC eal_common_pci.o
==> default:   CC eal_common_pci_uio.o
==> default:   CC eal_common_memory.o
==> default:   CC eal_common_tailqs.o
==> default:   CC eal_common_errno.o
==> default:   CC eal_common_cpuflags.o
==> default:   CC eal_common_string_fns.o
==> default:   CC eal_common_hexdump.o
==> default:   CC eal_common_devargs.o
==> default:   CC eal_common_bus.o
==> default:   CC eal_common_dev.o
==> default:   CC eal_common_options.o
==> default:   CC eal_common_thread.o
==> default:   CC eal_common_proc.o
==> default:   CC rte_malloc.o
==> default:   CC malloc_elem.o
==> default:   CC malloc_heap.o
==> default:   CC rte_keepalive.o
==> default:   CC rte_cpuflags.o
==> default:   CC rte_spinlock.o
==> default:   SYMLINK-FILE include/exec-env/rte_interrupts.h
==> default:   SYMLINK-FILE include/exec-env/rte_kni_common.h
==> default:   SYMLINK-FILE include/exec-env/rte_dom0_common.h
==> default:   AR librte_eal.a
==> default:   INSTALL-LIB librte_eal.a
==> default: == Build lib/librte_ring
==> default: == Build lib/librte_timer
==> default:   CC rte_ring.o
==> default:   CC rte_timer.o
==> default:   SYMLINK-FILE include/rte_ring.h
==> default:   AR librte_ring.a
==> default:   INSTALL-LIB librte_ring.a
==> default: == Build lib/librte_mempool
==> default:   SYMLINK-FILE include/rte_timer.h
==> default:   AR librte_timer.a
==> default:   INSTALL-LIB librte_timer.a
==> default:   CC rte_mempool.o
==> default:   CC rte_mempool_ops.o
==> default:   CC rte_mempool_ring.o
==> default:   CC rte_mempool_stack.o
==> default:   SYMLINK-FILE include/rte_mempool.h
==> default:   AR librte_mempool.a
==> default:   INSTALL-LIB librte_mempool.a
==> default: == Build lib/librte_mbuf
==> default:   CC rte_mbuf.o
==> default:   CC rte_mbuf_ptype.o
==> default:   SYMLINK-FILE include/rte_mbuf.h
==> default:   SYMLINK-FILE include/rte_mbuf_ptype.h
==> default:   AR librte_mbuf.a
==> default:   INSTALL-LIB librte_mbuf.a
==> default: == Build lib/librte_net
==> default:   SYMLINK-FILE include/rte_ip.h
==> default:   CC rte_net.o
==> default:   SYMLINK-FILE include/rte_tcp.h
==> default:   SYMLINK-FILE include/rte_udp.h
==> default:   SYMLINK-FILE include/rte_sctp.h
==> default:   SYMLINK-FILE include/rte_icmp.h
==> default:   SYMLINK-FILE include/rte_arp.h
==> default:   SYMLINK-FILE include/rte_ether.h
==> default:   SYMLINK-FILE include/rte_gre.h
==> default:   SYMLINK-FILE include/rte_net.h
==> default:   AR librte_net.a
==> default:   INSTALL-LIB librte_net.a
==> default: == Build lib/librte_ether
==> default:   CC rte_ethdev.o
==> default:   CC rte_flow.o
==> default:   SYMLINK-FILE include/rte_ethdev.h
==> default:   SYMLINK-FILE include/rte_eth_ctrl.h
==> default:   SYMLINK-FILE include/rte_dev_info.h
==> default:   SYMLINK-FILE include/rte_flow.h
==> default:   SYMLINK-FILE include/rte_flow_driver.h
==> default:   AR librte_ethdev.a
==> default:   INSTALL-LIB librte_ethdev.a
==> default: == Build lib/librte_vhost
==> default:   CC fd_man.o
==> default:   CC socket.o
==> default:   CC vhost.o
==> default:   CC vhost_user.o
==> default:   CC virtio_net.o
==> default:   SYMLINK-FILE include/rte_virtio_net.h
==> default:   AR librte_vhost.a
==> default:   INSTALL-LIB librte_vhost.a
==> default: == Build buildtools
==> default: == Build app
==> default: == Build app/proc_info
==> default: == Build buildtools/pmdinfogen
==> default:   HOSTCC pmdinfogen.o
==> default:   CC main.o
==> default:   HOSTLD dpdk-pmdinfogen
==> default:   INSTALL-HOSTAPP dpdk-pmdinfogen
==> default: == Build drivers
==> default: == Build drivers/net
==> default:   LD dpdk-procinfo
==> default:   INSTALL-APP dpdk-procinfo
==> default:   INSTALL-MAP dpdk-procinfo.map
==> default: Build complete [x86_64-native-linuxapp-gcc]
==> default: make[3]: Leaving directory '/spdk/dpdk'
==> default:   CC lib/bdev/bdev.o
==> default:   CC lib/blob/blobstore.o
==> default:   CC lib/bdev/scsi_nvme.o
==> default:   CC lib/bdev/error/vbdev_error.o
==> default:   CC lib/blob/request.o
==> default:   CC lib/bdev/error/vbdev_error_rpc.o
==> default:   CC lib/blob/bdev/blob_bdev.o
==> default:   LIB libspdk_vbdev_error.a
==> default:   CC lib/bdev/malloc/blockdev_malloc.o
==> default:   LIB libspdk_blob_bdev.a
==> default:   LIB libspdk_blob.a
==> default:   CC lib/blobfs/blobfs.o
==> default:   CC lib/bdev/malloc/blockdev_malloc_rpc.o
==> default:   LIB libspdk_bdev_malloc.a
==> default:   CC lib/bdev/null/blockdev_null.o
==> default:   CC lib/blobfs/tree.o
==> default:   CC lib/bdev/null/blockdev_null_rpc.o
==> default:   LIB libspdk_blobfs.a
==> default:   LIB libspdk_bdev_null.a
==> default:   CC lib/conf/conf.o
==> default:   CC lib/bdev/nvme/blockdev_nvme.o
==> default:   LIB libspdk_conf.a
==> default:   CC lib/copy/copy_engine.o
==> default:   CC lib/bdev/nvme/blockdev_nvme_rpc.o
==> default:   CC lib/copy/ioat/copy_engine_ioat.o
==> default:   LIB libspdk_bdev_nvme.a
==> default:   CC lib/bdev/rpc/bdev_rpc.o
==> default:   LIB libspdk_copy_ioat.a
==> default:   LIB libspdk_copy.a
==> default:   CC lib/cunit/spdk_cunit.o
==> default:   LIB libspdk_bdev_rpc.a
==> default:   CC lib/bdev/split/vbdev_split.o
==> default:   LIB libspdk_cunit.a
==> default:   CC lib/event/app.o
==> default:   LIB libspdk_vbdev_split.a
==> default:   CC lib/bdev/aio/blockdev_aio.o
==> default:   CC lib/event/reactor.o
==> default:   CC lib/bdev/aio/blockdev_aio_rpc.o
==> default:   CC lib/event/subsystem.o
==> default:   LIB libspdk_bdev_aio.a
==> default:   LIB libspdk_bdev.a
==> default:   CC lib/json/json_parse.o
==> default:   CC lib/event/rpc/app_rpc.o
==> default:   CC lib/json/json_util.o
==> default:   LIB libspdk_app_rpc.a
==> default:   LIB libspdk_event.a
==> default:   CC lib/jsonrpc/jsonrpc_server.o
==> default:   CC lib/json/json_write.o
==> default:   CC lib/jsonrpc/jsonrpc_server_tcp.o
==> default:   LIB libspdk_jsonrpc.a
==> default:   CC lib/log/log.o
==> default:   LIB libspdk_json.a
==> default:   CC lib/env_dpdk/env.o
==> default:   CC lib/log/rpc/log_rpc.o
==> default:   LIB libspdk_log_rpc.a
==> default:   LIB libspdk_log.a
==> default:   CC lib/net/interface.o
==> default:   CC lib/net/sock.o
==> default:   CC lib/net/net_framework_default.o
==> default:   CC lib/net/net_rpc.o
==> default:   CC lib/env_dpdk/pci.o
==> default:   LIB libspdk_net.a
==> default:   CC lib/rpc/rpc.o
==> default:   CC lib/env_dpdk/vtophys.o
==> default:   LIB libspdk_rpc.a
==> default:   CC lib/trace/trace.o
==> default:   LIB libspdk_trace.a
==> default:   CC lib/env_dpdk/init.o
==> default:   CC lib/util/bit_array.o
==> default:   CC lib/util/fd.o
==> default:   CC lib/env_dpdk/threads.o
==> default:   CC lib/util/io_channel.o
==> default:   CC lib/env_dpdk/pci_nvme.o
==> default:   CC lib/util/string.o
==> default:   CC lib/env_dpdk/pci_ioat.o
==> default:   LIB libspdk_util.a
==> default:   CC lib/nvme/nvme_ctrlr_cmd.o
==> default:   LIB libspdk_env_dpdk.a
==> default:   CC lib/nvmf/discovery.o
==> default:   CC lib/nvmf/subsystem.o
==> default:   CC lib/nvmf/nvmf.o
==> default:   CC lib/nvme/nvme_ctrlr.o
==> default:   CC lib/nvmf/request.o
==> default:   CC lib/nvmf/session.o
==> default:   CC lib/nvmf/transport.o
==> default:   CC lib/nvmf/direct.o
==> default:   CC lib/nvme/nvme_ns_cmd.o
==> default:   CC lib/nvmf/virtual.o
==> default:   LIB libspdk_nvmf.a
==> default:   CC lib/scsi/dev.o
==> default:   CC lib/scsi/lun.o
==> default:   CC lib/scsi/lun_db.o
==> default:   CC lib/nvme/nvme_ns.o
==> default:   CC lib/scsi/port.o
==> default:   CC lib/scsi/scsi.o
==> default:   CC lib/scsi/scsi_bdev.o
==> default:   CC lib/nvme/nvme_pcie.o
==> default:   CC lib/scsi/scsi_rpc.o
==> default:   CC lib/scsi/task.o
==> default:   LIB libspdk_scsi.a
==> default:   CC lib/ioat/ioat.o
==> default:   LIB libspdk_ioat.a
==> default:   CC lib/iscsi/acceptor.o
==> default:   CC lib/nvme/nvme_qpair.o
==> default:   CC lib/iscsi/conn.o
==> default:   CC lib/nvme/nvme.o
==> default:   CC lib/iscsi/crc32c.o
==> default:   CC lib/iscsi/init_grp.o
==> default:   CC lib/iscsi/iscsi.o
==> default:   CC lib/nvme/nvme_quirks.o
==> default:   CC lib/nvme/nvme_transport.o
==> default:   CC lib/nvme/nvme_uevent.o
==> default:   LIB libspdk_nvme.a
==> default:   CC lib/vhost/task.o
==> default:   CC lib/iscsi/md5.o
==> default:   CC lib/iscsi/param.o
==> default:   CC lib/iscsi/portal_grp.o
==> default:   CC lib/iscsi/tgt_node.o
==> default:   CC lib/vhost/vhost.o
==> default:   CC lib/iscsi/iscsi_subsystem.o
==> default:   CC lib/vhost/vhost_rpc.o
==> default:   CC lib/vhost/vhost_iommu.o
==> default:   CC lib/iscsi/iscsi_rpc.o
==> default:   CC lib/vhost/rte_vhost/fd_man.o
==> default:   CC lib/vhost/rte_vhost/socket.o
==> default:   CC lib/iscsi/task.o
==> default:   CC lib/vhost/rte_vhost/vhost_user.o
==> default:   LIB libspdk_iscsi.a
==> default:   CC lib/vhost/rte_vhost/vhost.o
==> default:   LIB libspdk_rte_vhost.a
==> default:   LIB libspdk_vhost.a
==> default:   TEST_HEADER include/spdk/event.h
==> default:   TEST_HEADER include/spdk/net.h
==> default:   TEST_HEADER include/spdk/copy_engine.h
==> default:   TEST_HEADER include/spdk/iscsi_spec.h
==> default:   TEST_HEADER include/spdk/io_channel.h
==> default:   CC examples/ioat/perf/perf.o
==> default:   TEST_HEADER include/spdk/log.h
==> default:   TEST_HEADER include/spdk/trace.h
==> default:   TEST_HEADER include/spdk/ioat_spec.h
==> default:   TEST_HEADER include/spdk/queue.h
==> default:   TEST_HEADER include/spdk/bit_array.h
==> default:   TEST_HEADER include/spdk/string.h
==> default:   TEST_HEADER include/spdk/jsonrpc.h
==> default:   TEST_HEADER include/spdk/json.h
==> default:   TEST_HEADER include/spdk/rpc.h
==> default:   TEST_HEADER include/spdk/ioat.h
==> default:   TEST_HEADER include/spdk/blobfs.h
==> default:   TEST_HEADER include/spdk/bdev.h
==> default:   TEST_HEADER include/spdk/likely.h
==> default:   TEST_HEADER include/spdk/env.h
==> default:   TEST_HEADER include/spdk/nvmf_spec.h
==> default:   TEST_HEADER include/spdk/mmio.h
==> default:   TEST_HEADER include/spdk/util.h
==> default:   TEST_HEADER include/spdk/fd.h
==> default:   TEST_HEADER include/spdk/gpt_spec.h
==> default:   TEST_HEADER include/spdk/barrier.h
==> default:   TEST_HEADER include/spdk/scsi_spec.h
==> default:   TEST_HEADER include/spdk/nvmf.h
==> default:   TEST_HEADER include/spdk/blob.h
==> default:   TEST_HEADER include/spdk/pci_ids.h
==> default:   TEST_HEADER include/spdk/assert.h
==> default:   TEST_HEADER include/spdk/nvme_spec.h
==> default:   TEST_HEADER include/spdk/endian.h
==> default:   TEST_HEADER include/spdk/scsi.h
==> default:   TEST_HEADER include/spdk/conf.h
==> default:   TEST_HEADER include/spdk/vhost.h
==> default:   TEST_HEADER include/spdk/nvme_intel.h
==> default:   TEST_HEADER include/spdk/nvme.h
==> default:   TEST_HEADER include/spdk/stdinc.h
==> default:   TEST_HEADER include/spdk/queue_extras.h
==> default:   TEST_HEADER include/spdk/blob_bdev.h
==> default:   CXX test/cpp_headers/event.o
==> default:   CXX test/cpp_headers/net.o
==> default:   LINK examples/ioat/perf/perf
==> default:   CC examples/ioat/verify/verify.o
==> default:   CXX test/cpp_headers/copy_engine.o
==> default:   CXX test/cpp_headers/iscsi_spec.o
==> default:   LINK examples/ioat/verify/verify
==> default:   CC examples/ioat/kperf/ioat_kperf.o
==> default:   CXX test/cpp_headers/io_channel.o
==> default:   LINK examples/ioat/kperf/ioat_kperf
==> default:   CXX test/cpp_headers/log.o
==> default:   CC examples/nvme/hello_world/hello_world.o
==> default:   CXX test/cpp_headers/trace.o
==> default:   LINK examples/nvme/hello_world/hello_world
==> default:   CXX test/cpp_headers/ioat_spec.o
==> default:   CC examples/nvme/identify/identify.o
==> default:   CXX test/cpp_headers/queue.o
==> default:   CXX test/cpp_headers/bit_array.o
==> default:   CXX test/cpp_headers/string.o
==> default:   LINK examples/nvme/identify/identify
==> default:   CXX test/cpp_headers/jsonrpc.o
==> default:   CC examples/nvme/perf/perf.o
==> default:   CXX test/cpp_headers/json.o
==> default:   CXX test/cpp_headers/rpc.o
==> default:   CXX test/cpp_headers/ioat.o
==> default:   CXX test/cpp_headers/blobfs.o
==> default:   CXX test/cpp_headers/bdev.o
==> default:   CXX test/cpp_headers/likely.o
==> default:   CXX test/cpp_headers/env.o
==> default:   LINK examples/nvme/perf/perf
==> default:   CXX test/cpp_headers/nvmf_spec.o
==> default:   CC examples/nvme/reserve/reserve.o
==> default:   CXX test/cpp_headers/mmio.o
==> default:   CXX test/cpp_headers/util.o
==> default:   LINK examples/nvme/reserve/reserve
==> default:   CC examples/nvme/nvme_manage/nvme_manage.o
==> default:   CXX test/cpp_headers/fd.o
==> default:   CXX test/cpp_headers/gpt_spec.o
==> default:   LINK examples/nvme/nvme_manage/nvme_manage
==> default:   CXX test/cpp_headers/barrier.o
==> default:   CC examples/nvme/arbitration/arbitration.o
==> default:   CXX test/cpp_headers/scsi_spec.o
==> default:   CXX test/cpp_headers/nvmf.o
==> default:   CXX test/cpp_headers/blob.o
==> default:   CXX test/cpp_headers/pci_ids.o
==> default:   CXX test/cpp_headers/assert.o
==> default:   CXX test/cpp_headers/nvme_spec.o
==> default:   CXX test/cpp_headers/endian.o
==> default:   LINK examples/nvme/arbitration/arbitration
==> default:   CXX test/cpp_headers/scsi.o
==> default:   CC examples/nvme/hotplug/hotplug.o
==> default:   CXX test/cpp_headers/conf.o
==> default:   CXX test/cpp_headers/vhost.o
==> default:   CXX test/cpp_headers/nvme_intel.o
==> default:   CXX test/cpp_headers/nvme.o
==> default:   CXX test/cpp_headers/stdinc.o
==> default:   CXX test/cpp_headers/queue_extras.o
==> default:   CXX test/cpp_headers/blob_bdev.o
==> default:   LINK examples/nvme/hotplug/hotplug
==> default:   CC test/lib/bdev/bdevio/bdevio.o
==> default:   CXX app/trace/trace.o
==> default:   LINK test/lib/bdev/bdevio/bdevio
==> default:   CC test/lib/bdev/bdevperf/bdevperf.o
==> default:   LINK app/trace/spdk_trace
==> default:   LINK test/lib/bdev/bdevperf/bdevperf
==> default:   CC app/nvmf_tgt/conf.o
==> default:   CC test/lib/blob/blob_ut/blob_ut.o
==> default:   CC app/nvmf_tgt/nvmf_main.o
==> default:   CC app/nvmf_tgt/nvmf_tgt.o
==> default:   CC app/nvmf_tgt/nvmf_rpc.o
==> default:   LINK app/nvmf_tgt/nvmf_tgt
==> default:   CXX app/iscsi_top/iscsi_top.o
==> default:   LINK test/lib/blob/blob_ut/blob_ut
==> default:   CC test/lib/blobfs/blobfs_async_ut/blobfs_async_ut.o
==> default:   LINK test/lib/blobfs/blobfs_async_ut/blobfs_async_ut
==> default:   CC test/lib/blobfs/blobfs_sync_ut/blobfs_sync_ut.o
==> default:   LINK test/lib/blobfs/blobfs_sync_ut/blobfs_sync_ut
==> default:   CC test/lib/blobfs/mkfs/mkfs.o
==> default:   LINK test/lib/blobfs/mkfs/mkfs
==> default:   CC test/lib/env/pci/pci_ut.o
==> default:   LINK test/lib/env/pci/pci_ut
==> default:   CC test/lib/env/vtophys/vtophys.o
==> default:   LINK app/iscsi_top/iscsi_top
==> default:   LINK test/lib/env/vtophys/vtophys
==> default:   CC test/lib/event/event_perf/event_perf.o
==> default:   CC app/iscsi_tgt/iscsi_tgt.o
==> default:   LINK test/lib/event/event_perf/event_perf
==> default:   LINK app/iscsi_tgt/iscsi_tgt
==> default:   CC test/lib/event/reactor/reactor.o
==> default:   LINK test/lib/event/reactor/reactor
==> default:   CC app/vhost/vhost.o
==> default:   CC test/lib/event/reactor_perf/reactor_perf.o
==> default:   LINK app/vhost/vhost
==> default:   LINK test/lib/event/reactor_perf/reactor_perf
==> default:   CC test/lib/event/subsystem/subsystem_ut.o
==> default:   CC test/lib/ioat/unit/ioat_ut.o
==> default:   LINK test/lib/event/subsystem/subsystem_ut
==> default:   CC test/lib/iscsi/param/param_ut.o
==> default:   LINK test/lib/ioat/unit/ioat_ut
==> default:   CC test/lib/json/jsoncat/jsoncat.o
==> default:   LINK test/lib/json/jsoncat/jsoncat
==> default:   CC test/lib/json/parse/json_parse_ut.o
==> default:   LINK test/lib/iscsi/param/param_ut
==> default:   CC test/lib/iscsi/pdu/pdu.o
==> default:   LINK test/lib/iscsi/pdu/pdu
==> default:   CC test/lib/iscsi/target_node/target_node_ut.o
==> default:   LINK test/lib/iscsi/target_node/target_node_ut
==> default:   CC test/lib/jsonrpc/server/jsonrpc_server_ut.o
==> default:   LINK test/lib/json/parse/json_parse_ut
==> default:   LINK test/lib/jsonrpc/server/jsonrpc_server_ut
==> default:   CC test/lib/json/util/json_util_ut.o
==> default:   CC test/lib/log/log_ut.o
==> default:   LINK test/lib/json/util/json_util_ut
==> default:   LINK test/lib/log/log_ut
==> default:   CC test/lib/json/write/json_write_ut.o
==> default:   CC test/lib/nvme/unit/nvme_c/nvme_ut.o
==> default:   CC test/lib/json/write/json_parse.o
==> default:   LINK test/lib/nvme/unit/nvme_c/nvme_ut
==> default:   CC test/lib/nvme/unit/nvme_ns_cmd_c/nvme_ns_cmd_ut.o
==> default:   LINK test/lib/json/write/json_write_ut
==> default:   CC test/lib/nvmf/direct/direct_ut.o
==> default:   LINK test/lib/nvmf/direct/direct_ut
==> default:   CC test/lib/nvmf/discovery/discovery_ut.o
==> default:   CC test/lib/nvmf/discovery/subsystem.o
==> default:   LINK test/lib/nvmf/discovery/discovery_ut
==> default:   CC test/lib/nvme/unit/nvme_ns_cmd_c/nvme.o
==> default:   CC test/lib/nvmf/request/request_ut.o
==> default:   LINK test/lib/nvmf/request/request_ut
==> default:   CC test/lib/nvmf/session/session_ut.o
==> default:   LINK test/lib/nvmf/session/session_ut
==> default:   LINK test/lib/nvme/unit/nvme_ns_cmd_c/nvme_ns_cmd_ut
==> default:   CC test/lib/nvme/unit/nvme_qpair_c/nvme_qpair_ut.o
==> default:   CC test/lib/nvmf/subsystem/subsystem_ut.o
==> default:   LINK test/lib/nvmf/subsystem/subsystem_ut
==> default:   CC test/lib/nvmf/virtual/virtual_ut.o
==> default:   LINK test/lib/nvmf/virtual/virtual_ut
==> default:   CC test/lib/scsi/dev/dev_ut.o
==> default:   LINK test/lib/nvme/unit/nvme_qpair_c/nvme_qpair_ut
==> default:   CC test/lib/nvme/unit/nvme_ctrlr_c/nvme_ctrlr_ut.o
==> default:   LINK test/lib/scsi/dev/dev_ut
==> default:   CC test/lib/scsi/init/init_ut.o
==> default:   LINK test/lib/scsi/init/init_ut
==> default:   CC test/lib/scsi/lun/lun_ut.o
==> default:   LINK test/lib/scsi/lun/lun_ut
==> default:   CC test/lib/scsi/scsi_bdev/scsi_bdev_ut.o
==> default:   LINK test/lib/scsi/scsi_bdev/scsi_bdev_ut
==> default:   CC test/lib/nvme/unit/nvme_ctrlr_c/nvme_quirks.o
==> default:   CC test/lib/scsi/scsi_nvme/scsi_nvme_ut.o
==> default:   LINK test/lib/scsi/scsi_nvme/scsi_nvme_ut
==> default:   CC test/lib/util/bit_array/bit_array_ut.o
==> default:   LINK test/lib/util/bit_array/bit_array_ut
==> default:   LINK test/lib/nvme/unit/nvme_ctrlr_c/nvme_ctrlr_ut
==> default:   CC test/lib/util/io_channel/io_channel_ut.o
==> default:   CC test/lib/nvme/unit/nvme_ctrlr_cmd_c/nvme_ctrlr_cmd_ut.o
==> default:   LINK test/lib/util/io_channel/io_channel_ut
==> default:   CC test/lib/util/string/string_ut.o
==> default:   LINK test/lib/util/string/string_ut
==> default:   CC test/lib/nvme/unit/nvme_pcie_c/nvme_pcie_ut.o
==> default:   LINK test/lib/nvme/unit/nvme_ctrlr_cmd_c/nvme_ctrlr_cmd_ut
==> default:   CC test/lib/nvme/unit/nvme_quirks_c/nvme_quirks_ut.o
==> default:   LINK test/lib/nvme/unit/nvme_pcie_c/nvme_pcie_ut
==> default:   LINK test/lib/nvme/unit/nvme_quirks_c/nvme_quirks_ut
==> default:   CC test/lib/nvme/unit/nvme_ns_c/nvme_ns_ut.o
==> default:   CC test/lib/nvme/aer/aer.o
==> default:   LINK test/lib/nvme/aer/aer
==> default:   CC test/lib/nvme/reset/reset.o
==> default:   LINK test/lib/nvme/unit/nvme_ns_c/nvme_ns_ut
==> default:   CC test/lib/nvme/sgl/sgl.o
==> default:   LINK test/lib/nvme/sgl/sgl
==> default:   CC test/lib/nvme/e2edp/nvme_dp.o
==> default:   LINK test/lib/nvme/reset/reset
==> default:   LINK test/lib/nvme/e2edp/nvme_dp
==> default:   CC test/lib/nvme/overhead/overhead.o
==> default:   LINK test/lib/nvme/overhead/overhead
==> default: Running provisioner: shell...
    default: Running: inline script
==> default: Running provisioner: file...
user@dev-system:~/spdk/scripts/vagrant$
user@dev-system:~/spdk/scripts/vagrant$ vagrant ssh
Welcome to Ubuntu 16.04 LTS (GNU/Linux 4.4.0-21-generic x86_64)
* Documentation:  https://help.ubuntu.com/
Last login: Tue May 23 11:43:10 2017 from 10.0.2.2
vagrant@localhost:~$ sudo /spdk/scripts/setup.sh
vagrant@localhost:~$ sudo /spdk/examples/nvme/hello_world/hello_world
Starting DPDK 17.02.0 initialization...
[ DPDK EAL parameters: hello_world -c 0x1 --file-prefix=spdk_pid1217 ]
EAL: Detected 2 lcore(s)
EAL: Probing VFIO support...
Initializing NVMe Controllers
Initialization complete.
vagrant@localhost:~$
