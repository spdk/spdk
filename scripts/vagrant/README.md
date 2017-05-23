Introduction
============

This is a vagrant environment for SPDK with support for Ubuntu 16.04 and
Centos 7.2. Be sure to use vagrant 1.9.4 or higher and VirtualBox 5.1 or
higher and the extention pack or things likely won't work.

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

The following shows sample output from starting up a VM and running
the NVMe sample application "hello world". If you don't see the
NVMe device as seen below in both the lspci output as well as the
application output you likely have a VirtualBox and/or Vagrant
versioning issue.

user@dev-system:~$ cd spdk
user@dev-system:~/spdk$ cd scripts/
user@dev-system:~/spdk/scripts$ cd vagrant/
user@dev-system:~/spdk/scripts/vagrant$ vagrant up
Bringing machine 'default' up with 'virtualbox' provider...
==> default: Clearing any previously set forwarded ports...
==> default: Clearing any previously set network interfaces...
==> default: Preparing network interfaces based on configuration...
    default: Adapter 1: nat
    default: Adapter 2: hostonly
==> default: Forwarding ports...
    default: 22 (guest) => 2222 (host) (adapter 1)
==> default: Running 'pre-boot' VM customizations...
==> default: Booting VM...
==> default: Waiting for machine to boot. This may take a few minutes...
    default: SSH address: 127.0.0.1:2222
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
    default: Guest Additions Version: 5.1
    default: VirtualBox Version: 5.1
==> default: Configuring and enabling network interfaces...
==> default: Rsyncing folder: /home/peluse/spdk/ => /spdk
==> default: Mounting shared folders...
    default: /vagrant => /home/peluse/spdk/scripts/vagrant
==> default: Running provisioner: shell...
    default: Running: /tmp/vagrant-shell20170524-2405-3cam94.sh
==> default: vm.nr_hugepages = 1024
==> default: Hit:1 http://us.archive.ubuntu.com/ubuntu xenial InRelease
<< whole bunch of these previous lines trimmed >>
==> default: Fetched 2,329 kB in 3s (588 kB/s)
==> default: Reading package lists...
==> default: Building dependency tree...
==> default:
==> default: Reading state information...
==> default: Calculating upgrade...
==> default: The following packages have been kept back:
==> default:   linux-generic linux-headers-generic linux-image-generic
==> default: The following packages will be upgraded:
==> default:   accountsservice apparmor apt apt-transport-https apt-utils base-files bash
<< whole bunch of these previous lines trimmed >>
==> default: 167 upgraded, 0 newly installed, 0 to remove and 3 not upgraded.
==> default: Need to get 124 MB of archives.
==> default: After this operation, 39.5 MB of additional disk space will be used.
==> default: Get:1 http://us.archive.ubuntu.com/ubuntu xenial-updates/main amd64 base-files amd64 9.4ubuntu4.4 [60.                 2 kB]
<< whole bunch of these previous lines trimmed >>
Extracting templates from packages: 100%
==> default: Preconfiguring packages ...
==> default: Fetched 124 MB in 2min 50s (727 kB/s)
==> default: Reading package lists...
==> default: Building dependency tree...
==> default:
==> default: Reading state information...
==> default: The following additional packages will be installed:
==> default:   gdbserver git-man libbabeltrace-ctf1 libbabeltrace1 libc-dev-bin libc6
==> default:   libc6-dbg libc6-dev liberror-perl libpython3.5 libpython3.5-minimal
==> default:   libpython3.5-stdlib patch python3.5 python3.5-minimal
==> default: Suggested packages:
==> default:   gdb-doc git-daemon-run | git-daemon-sysvinit git-doc git-el git-email
==> default:   git-gui gitk gitweb git-arch git-cvs git-mediawiki git-svn glibc-doc
==> default:   diffutils-doc python3.5-venv python3.5-doc binfmt-support
==> default: The following NEW packages will be installed:
==> default:   gdb gdbserver git git-man libbabeltrace-ctf1 libbabeltrace1 libc6-dbg
==> default:   liberror-perl libpython3.5 patch
==> default: The following packages will be upgraded:
==> default:   libc-dev-bin libc6 libc6-dev libpython3.5-minimal libpython3.5-stdlib
==> default:   python3.5 python3.5-minimal
==> default: 7 upgraded, 10 newly installed, 0 to remove and 163 not upgraded.
==> default: Need to get 11.8 MB/20.9 MB of archives.
==> default: After this operation, 64.7 MB of additional disk space will be used.
==> default: Get:1 http://us.archive.ubuntu.com/ubuntu xenial/main amd64 libbabeltrace1 amd64 1.3.2-1 [34.7 kB]
<< whole bunch of these previous lines trimmed >>
==> default: Preconfiguring packages ...
==> default: Fetched 11.8 MB in 17s (669 kB/s)
==> default: Setting up libc6:amd64 (2.23-0ubuntu7) ...
==> default: Processing triggers for libc-bin (2.23-0ubuntu3) ...
<< whole bunch of these previous lines trimmed >>
==> default: Running provisioner: shell...
    default: Running: /tmp/vagrant-shell20170524-2405-1wt8p3c.sh
==> default: 0:/tmp/vagrant-shell
==> default: 1:
==> default: 2:
==> default: SUDOCMD: sudo -H -u vagrant
==> default: KERNEL_OS: GNU/Linux
==> default: KERNEL_MACHINE: x86_64
==> default: KERNEL_RELEASE: 4.4.0-21-generic
==> default: KERNEL_VERSION: #37-Ubuntu SMP Mon Apr 18 18:33:37 UTC 2016
==> default: DISTRIB_ID: Ubuntu
==> default: DISTRIB_RELEASE: 16.04
==> default: DISTRIB_CODENAME: xenial
==> default: DISTRIB_DESCRIPTION: Ubuntu 16.04 LTS
==> default: Reading package lists...
==> default: Building dependency tree...
==> default: Reading state information...
==> default: gcc is already the newest version (4:5.3.1-1ubuntu1).
==> default: make is already the newest version (4.1-6).
==> default: The following additional packages will be installed:
==> default:   binutils cpp-5 g++-5 gcc-5 gcc-5-base libaio1 libasan2 libatomic1 libcc1-0
==> default:   libcilkrts5 libcunit1 libgcc-5-dev libgomp1 libitm1 liblsan0 libmpx0
==> default:   libquadmath0 libssl-doc libssl1.0.0 libstdc++-5-dev libstdc++6 libtsan0
==> default:   libubsan0 zlib1g zlib1g-dev
==> default: Suggested packages:
==> default:   binutils-doc gcc-5-locales g++-multilib g++-5-multilib gcc-5-doc
==> default:   libstdc++6-5-dbg gcc-5-multilib libgcc1-dbg libgomp1-dbg libitm1-dbg
==> default:   libatomic1-dbg libasan2-dbg liblsan0-dbg libtsan0-dbg libubsan0-dbg
==> default:   libcilkrts5-dbg libmpx0-dbg libquadmath0-dbg libcunit1-doc libstdc++-5-doc
==> default: The following NEW packages will be installed:
==> default:   g++ g++-5 libaio-dev libaio1 libcunit1 libcunit1-dev libssl-dev libssl-doc
==> default:   libstdc++-5-dev zlib1g-dev
==> default: The following packages will be upgraded:
==> default:   binutils cpp-5 gcc-5 gcc-5-base libasan2 libatomic1 libcc1-0 libcilkrts5
==> default:   libgcc-5-dev libgomp1 libitm1 liblsan0 libmpx0 libquadmath0 libssl1.0.0
==> default:   libstdc++6 libtsan0 libubsan0 zlib1g
==> default: 19 upgraded, 10 newly installed, 0 to remove and 144 not upgraded.
==> default: Need to get 12.4 MB/35.8 MB of archives.
==> default: After this operation, 49.8 MB of additional disk space will be used.
==> default: Get:1 http://us.archive.ubuntu.com/ubuntu xenial-updates/main amd64 libstdc++-5-dev amd64 5.4.0-6ubuntu1~16.04.4 [1,426 kB]
<< whole bunch of these previous lines trimmed >>
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
==> default:   SYMLINK-FILE include/rte_atomic_64.h
==> default:   SYMLINK-FILE include/rte_rtm.h
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
==> default:   CC eal_common_options.o
==> default:   CC eal_common_dev.o
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
==> default:   CC rte_timer.o
==> default:   CC rte_ring.o
==> default:   SYMLINK-FILE include/rte_ring.h
==> default:   AR librte_ring.a
==> default:   SYMLINK-FILE include/rte_timer.h
==> default:   INSTALL-LIB librte_ring.a
==> default:   AR librte_timer.a
==> default: == Build lib/librte_mempool
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
==> default: == Build buildtools/pmdinfogen
==> default: == Build app/proc_info
==> default:   HOSTCC pmdinfogen.o
==> default:   HOSTLD dpdk-pmdinfogen
==> default:   INSTALL-HOSTAPP dpdk-pmdinfogen
==> default: == Build drivers
==> default:   CC main.o
==> default: == Build drivers/net
==> default:   LD dpdk-procinfo
==> default:   INSTALL-APP dpdk-procinfo
==> default:   INSTALL-MAP dpdk-procinfo.map
==> default: Build complete [x86_64-native-linuxapp-gcc]
==> default: make[3]: Leaving directory '/spdk/dpdk'
==> default:   CC lib/blob/blobstore.o
==> default:   CC lib/bdev/bdev.o
==> default:   CC lib/bdev/scsi_nvme.o
==> default:   CC lib/blob/request.o
==> default:   CC lib/bdev/error/vbdev_error.o
==> default:   CC lib/blob/bdev/blob_bdev.o
==> default:   LIB libspdk_blob_bdev.a
==> default:   LIB libspdk_blob.a
==> default:   CC lib/bdev/error/vbdev_error_rpc.o
==> default:   CC lib/blobfs/blobfs.o
==> default:   LIB libspdk_vbdev_error.a
==> default:   CC lib/bdev/malloc/blockdev_malloc.o
==> default:   CC lib/blobfs/tree.o
==> default:   CC lib/bdev/malloc/blockdev_malloc_rpc.o
==> default:   LIB libspdk_blobfs.a
==> default:   CC lib/conf/conf.o
==> default:   LIB libspdk_bdev_malloc.a
==> default:   CC lib/bdev/null/blockdev_null.o
==> default:   LIB libspdk_conf.a
==> default:   CC lib/copy/copy_engine.o
==> default:   CC lib/bdev/null/blockdev_null_rpc.o
==> default:   CC lib/copy/ioat/copy_engine_ioat.o
==> default:   LIB libspdk_bdev_null.a
==> default:   CC lib/bdev/nvme/blockdev_nvme.o
==> default:   LIB libspdk_copy_ioat.a
==> default:   LIB libspdk_copy.a
==> default:   CC lib/cunit/spdk_cunit.o
==> default:   CC lib/bdev/nvme/blockdev_nvme_rpc.o
==> default:   LIB libspdk_cunit.a
==> default:   LIB libspdk_bdev_nvme.a
==> default:   CC lib/event/app.o
==> default:   CC lib/bdev/rpc/bdev_rpc.o
==> default:   CC lib/event/reactor.o
==> default:   LIB libspdk_bdev_rpc.a
==> default:   CC lib/bdev/split/vbdev_split.o
==> default:   LIB libspdk_vbdev_split.a
==> default:   CC lib/bdev/aio/blockdev_aio.o
==> default:   CC lib/event/subsystem.o
==> default:   CC lib/bdev/aio/blockdev_aio_rpc.o
==> default:   LIB libspdk_bdev_aio.a
==> default:   CC lib/event/rpc/app_rpc.o
==> default:   LIB libspdk_bdev.a
==> default:   CC lib/json/json_parse.o
==> default:   LIB libspdk_app_rpc.a
==> default:   LIB libspdk_event.a
==> default:   CC lib/jsonrpc/jsonrpc_server.o
==> default:   CC lib/json/json_util.o
==> default:   CC lib/json/json_write.o
==> default:   CC lib/jsonrpc/jsonrpc_server_tcp.o
==> default:   LIB libspdk_json.a
==> default:   CC lib/log/log.o
==> default:   LIB libspdk_jsonrpc.a
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
==> default:   CC lib/env_dpdk/init.o
==> default:   CC lib/trace/trace.o
==> default:   CC lib/env_dpdk/threads.o
==> default:   CC lib/env_dpdk/pci_nvme.o
==> default:   LIB libspdk_trace.a
==> default:   CC lib/util/bit_array.o
==> default:   CC lib/env_dpdk/pci_ioat.o
==> default:   CC lib/util/fd.o
==> default:   LIB libspdk_env_dpdk.a
==> default:   CC lib/nvme/nvme_ctrlr_cmd.o
==> default:   CC lib/util/io_channel.o
==> default:   CC lib/util/string.o
==> default:   LIB libspdk_util.a
==> default:   CC lib/nvmf/discovery.o
==> default:   CC lib/nvmf/subsystem.o
==> default:   CC lib/nvme/nvme_ctrlr.o
==> default:   CC lib/nvmf/nvmf.o
==> default:   CC lib/nvmf/request.o
==> default:   CC lib/nvmf/session.o
==> default:   CC lib/nvmf/transport.o
==> default:   CC lib/nvme/nvme_ns_cmd.o
==> default:   CC lib/nvmf/direct.o
==> default:   CC lib/nvmf/virtual.o
==> default:   CC lib/nvme/nvme_ns.o
==> default:   LIB libspdk_nvmf.a
==> default:   CC lib/scsi/dev.o
==> default:   CC lib/nvme/nvme_pcie.o
==> default:   CC lib/scsi/lun.o
==> default:   CC lib/scsi/lun_db.o
==> default:   CC lib/scsi/port.o
==> default:   CC lib/scsi/scsi.o
==> default:   CC lib/nvme/nvme_qpair.o
==> default:   CC lib/scsi/scsi_bdev.o
==> default:   CC lib/scsi/scsi_rpc.o
==> default:   CC lib/nvme/nvme.o
==> default:   CC lib/scsi/task.o
==> default:   LIB libspdk_scsi.a
==> default:   CC lib/ioat/ioat.o
==> default:   LIB libspdk_ioat.a
==> default:   CC lib/iscsi/acceptor.o
==> default:   CC lib/nvme/nvme_quirks.o
==> default:   CC lib/iscsi/conn.o
==> default:   CC lib/nvme/nvme_transport.o
==> default:   CC lib/iscsi/crc32c.o
==> default:   CC lib/iscsi/init_grp.o
==> default:   CC lib/nvme/nvme_uevent.o
==> default:   CC lib/iscsi/iscsi.o
==> default:   LIB libspdk_nvme.a
==> default:   CC lib/iscsi/md5.o
==> default:   CC lib/iscsi/param.o
==> default:   CC lib/iscsi/portal_grp.o
==> default:   CC lib/iscsi/tgt_node.o
==> default:   CC lib/iscsi/iscsi_subsystem.o
==> default:   CC lib/iscsi/iscsi_rpc.o
==> default:   CC lib/iscsi/task.o
==> default:   CC lib/vhost/task.o
==> default:   LIB libspdk_iscsi.a
==> default:   CC lib/vhost/vhost.o
==> default:   CC lib/vhost/vhost_rpc.o
==> default:   CC lib/vhost/vhost_iommu.o
==> default:   CC lib/vhost/rte_vhost/fd_man.o
==> default:   CC lib/vhost/rte_vhost/socket.o
==> default:   CC lib/vhost/rte_vhost/vhost_user.o
==> default:   CC lib/vhost/rte_vhost/vhost.o
==> default:   LIB libspdk_vhost.a
==> default:   LIB libspdk_rte_vhost.a
==> default:   TEST_HEADER include/spdk/event.h
==> default:   TEST_HEADER include/spdk/net.h
==> default:   TEST_HEADER include/spdk/copy_engine.h
==> default:   TEST_HEADER include/spdk/iscsi_spec.h
==> default:   TEST_HEADER include/spdk/io_channel.h
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
==> default:   CC examples/ioat/perf/perf.o
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
==> default:   CXX test/cpp_headers/copy_engine.o
==> default:   LINK examples/ioat/perf/perf
==> default:   CXX test/cpp_headers/iscsi_spec.o
==> default:   CC examples/ioat/verify/verify.o
==> default:   CXX test/cpp_headers/io_channel.o
==> default:   CXX test/cpp_headers/log.o
==> default:   LINK examples/ioat/verify/verify
==> default:   CXX test/cpp_headers/trace.o
==> default:   CC examples/ioat/kperf/ioat_kperf.o
==> default:   CXX test/cpp_headers/ioat_spec.o
==> default:   CXX test/cpp_headers/queue.o
==> default:   LINK examples/ioat/kperf/ioat_kperf
==> default:   CXX test/cpp_headers/bit_array.o
==> default:   CC examples/nvme/hello_world/hello_world.o
==> default:   CXX test/cpp_headers/string.o
==> default:   CXX test/cpp_headers/jsonrpc.o
==> default:   LINK examples/nvme/hello_world/hello_world
==> default:   CXX test/cpp_headers/json.o
==> default:   CC examples/nvme/identify/identify.o
==> default:   CXX test/cpp_headers/rpc.o
==> default:   CXX test/cpp_headers/ioat.o
==> default:   CXX test/cpp_headers/blobfs.o
==> default:   CXX test/cpp_headers/bdev.o
==> default:   LINK examples/nvme/identify/identify
==> default:   CC examples/nvme/perf/perf.o
==> default:   CXX test/cpp_headers/likely.o
==> default:   CXX test/cpp_headers/env.o
==> default:   CXX test/cpp_headers/nvmf_spec.o
==> default:   CXX test/cpp_headers/mmio.o
==> default:   CXX test/cpp_headers/util.o
==> default:   CXX test/cpp_headers/fd.o
==> default:   CXX test/cpp_headers/gpt_spec.o
==> default:   CXX test/cpp_headers/barrier.o
==> default:   LINK examples/nvme/perf/perf
==> default:   CXX test/cpp_headers/scsi_spec.o
==> default:   CC examples/nvme/reserve/reserve.o
==> default:   CXX test/cpp_headers/nvmf.o
==> default:   LINK examples/nvme/reserve/reserve
==> default:   CXX test/cpp_headers/blob.o
==> default:   CC examples/nvme/nvme_manage/nvme_manage.o
==> default:   CXX test/cpp_headers/pci_ids.o
==> default:   CXX test/cpp_headers/assert.o
==> default:   LINK examples/nvme/nvme_manage/nvme_manage
==> default:   CXX test/cpp_headers/nvme_spec.o
==> default:   CC examples/nvme/arbitration/arbitration.o
==> default:   CXX test/cpp_headers/endian.o
==> default:   CXX test/cpp_headers/scsi.o
==> default:   CXX test/cpp_headers/conf.o
==> default:   CXX test/cpp_headers/vhost.o
==> default:   CXX test/cpp_headers/nvme_intel.o
==> default:   CXX test/cpp_headers/nvme.o
==> default:   CXX test/cpp_headers/stdinc.o
==> default:   CXX test/cpp_headers/queue_extras.o
==> default:   CXX test/cpp_headers/blob_bdev.o
==> default:   LINK examples/nvme/arbitration/arbitration
==> default:   CC test/lib/bdev/bdevio/bdevio.o
==> default:   CC examples/nvme/hotplug/hotplug.o
==> default:   LINK test/lib/bdev/bdevio/bdevio
==> default:   CC test/lib/bdev/bdevperf/bdevperf.o
==> default:   LINK examples/nvme/hotplug/hotplug
==> default:   CXX app/trace/trace.o
==> default:   LINK test/lib/bdev/bdevperf/bdevperf
==> default:   CC test/lib/blob/blob_ut/blob_ut.o
==> default:   LINK app/trace/spdk_trace
==> default:   CC app/nvmf_tgt/conf.o
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
==> default:   LINK test/lib/env/vtophys/vtophys
==> default:   CC test/lib/event/event_perf/event_perf.o
==> default:   LINK test/lib/event/event_perf/event_perf
==> default:   CC test/lib/event/reactor/reactor.o
==> default:   LINK test/lib/event/reactor/reactor
==> default:   CC test/lib/event/reactor_perf/reactor_perf.o
==> default:   LINK test/lib/event/reactor_perf/reactor_perf
==> default:   CC test/lib/event/subsystem/subsystem_ut.o
==> default:   LINK app/iscsi_top/iscsi_top
==> default:   LINK test/lib/event/subsystem/subsystem_ut
==> default:   CC test/lib/ioat/unit/ioat_ut.o
==> default:   CC app/iscsi_tgt/iscsi_tgt.o
==> default:   LINK test/lib/ioat/unit/ioat_ut
==> default:   CC test/lib/iscsi/param/param_ut.o
==> default:   LINK app/iscsi_tgt/iscsi_tgt
==> default:   LINK test/lib/iscsi/param/param_ut
==> default:   CC test/lib/iscsi/pdu/pdu.o
==> default:   CC app/vhost/vhost.o
==> default:   LINK test/lib/iscsi/pdu/pdu
==> default:   LINK app/vhost/vhost
==> default:   CC test/lib/iscsi/target_node/target_node_ut.o
==> default:   CC test/lib/json/jsoncat/jsoncat.o
==> default:   LINK test/lib/iscsi/target_node/target_node_ut
==> default:   CC test/lib/jsonrpc/server/jsonrpc_server_ut.o
==> default:   LINK test/lib/json/jsoncat/jsoncat
==> default:   CC test/lib/json/parse/json_parse_ut.o
==> default:   LINK test/lib/jsonrpc/server/jsonrpc_server_ut
==> default:   CC test/lib/log/log_ut.o
==> default:   LINK test/lib/log/log_ut
==> default:   CC test/lib/nvme/unit/nvme_c/nvme_ut.o
==> default:   LINK test/lib/nvme/unit/nvme_c/nvme_ut
==> default:   CC test/lib/nvme/unit/nvme_ns_cmd_c/nvme_ns_cmd_ut.o
==> default:   CC test/lib/nvme/unit/nvme_ns_cmd_c/nvme.o
==> default:   LINK test/lib/json/parse/json_parse_ut
==> default:   CC test/lib/json/util/json_util_ut.o
==> default:   LINK test/lib/nvme/unit/nvme_ns_cmd_c/nvme_ns_cmd_ut
==> default:   CC test/lib/nvme/unit/nvme_qpair_c/nvme_qpair_ut.o
==> default:   LINK test/lib/json/util/json_util_ut
==> default:   CC test/lib/json/write/json_write_ut.o
==> default:   LINK test/lib/nvme/unit/nvme_qpair_c/nvme_qpair_ut
==> default:   CC test/lib/nvme/unit/nvme_ctrlr_c/nvme_ctrlr_ut.o
==> default:   CC test/lib/json/write/json_parse.o
==> default:   CC test/lib/nvme/unit/nvme_ctrlr_c/nvme_quirks.o
==> default:   LINK test/lib/json/write/json_write_ut
==> default:   CC test/lib/nvmf/direct/direct_ut.o
==> default:   LINK test/lib/nvme/unit/nvme_ctrlr_c/nvme_ctrlr_ut
==> default:   CC test/lib/nvme/unit/nvme_ctrlr_cmd_c/nvme_ctrlr_cmd_ut.o
==> default:   LINK test/lib/nvmf/direct/direct_ut
==> default:   CC test/lib/nvmf/discovery/discovery_ut.o
==> default:   CC test/lib/nvmf/discovery/subsystem.o
==> default:   LINK test/lib/nvme/unit/nvme_ctrlr_cmd_c/nvme_ctrlr_cmd_ut
==> default:   CC test/lib/nvme/unit/nvme_pcie_c/nvme_pcie_ut.o
==> default:   LINK test/lib/nvmf/discovery/discovery_ut
==> default:   CC test/lib/nvmf/request/request_ut.o
==> default:   LINK test/lib/nvmf/request/request_ut
==> default:   CC test/lib/nvmf/session/session_ut.o
==> default:   LINK test/lib/nvmf/session/session_ut
==> default:   LINK test/lib/nvme/unit/nvme_pcie_c/nvme_pcie_ut
==> default:   CC test/lib/nvmf/subsystem/subsystem_ut.o
==> default:   CC test/lib/nvme/unit/nvme_quirks_c/nvme_quirks_ut.o
==> default:   LINK test/lib/nvmf/subsystem/subsystem_ut
==> default:   CC test/lib/nvmf/virtual/virtual_ut.o
==> default:   LINK test/lib/nvmf/virtual/virtual_ut
==> default:   CC test/lib/scsi/dev/dev_ut.o
==> default:   LINK test/lib/nvme/unit/nvme_quirks_c/nvme_quirks_ut
==> default:   CC test/lib/nvme/unit/nvme_ns_c/nvme_ns_ut.o
==> default:   LINK test/lib/scsi/dev/dev_ut
==> default:   CC test/lib/scsi/init/init_ut.o
==> default:   LINK test/lib/scsi/init/init_ut
==> default:   CC test/lib/scsi/lun/lun_ut.o
==> default:   LINK test/lib/nvme/unit/nvme_ns_c/nvme_ns_ut
==> default:   LINK test/lib/scsi/lun/lun_ut
==> default:   CC test/lib/nvme/aer/aer.o
==> default:   CC test/lib/scsi/scsi_bdev/scsi_bdev_ut.o
==> default:   LINK test/lib/nvme/aer/aer
==> default:   CC test/lib/nvme/reset/reset.o
==> default:   LINK test/lib/scsi/scsi_bdev/scsi_bdev_ut
==> default:   CC test/lib/scsi/scsi_nvme/scsi_nvme_ut.o
==> default:   LINK test/lib/scsi/scsi_nvme/scsi_nvme_ut
==> default:   CC test/lib/util/bit_array/bit_array_ut.o
==> default:   LINK test/lib/util/bit_array/bit_array_ut
==> default:   CC test/lib/util/io_channel/io_channel_ut.o
==> default:   LINK test/lib/util/io_channel/io_channel_ut
==> default:   LINK test/lib/nvme/reset/reset
==> default:   CC test/lib/util/string/string_ut.o
==> default:   CC test/lib/nvme/sgl/sgl.o
==> default:   LINK test/lib/util/string/string_ut
==> default:   LINK test/lib/nvme/sgl/sgl
==> default:   CC test/lib/nvme/e2edp/nvme_dp.o
==> default:   CC test/lib/nvme/overhead/overhead.o
==> default:   LINK test/lib/nvme/overhead/overhead
==> default:   LINK test/lib/nvme/e2edp/nvme_dp
==> default: Running provisioner: shell...
    default: Running: inline script
==> default: 0000:00:0e.0 (80ee 4e56): nvme -> uio_pci_generic
user@dev-system:~/spdk/scripts/vagrant$ vagrant ssh
Welcome to Ubuntu 16.04 LTS (GNU/Linux 4.4.0-21-generic x86_64)

 * Documentation:  https://help.ubuntu.com/
vagrant@localhost:~$ lspci
00:00.0 Host bridge: Intel Corporation 440FX - 82441FX PMC [Natoma] (rev 02)
00:01.0 ISA bridge: Intel Corporation 82371SB PIIX3 ISA [Natoma/Triton II]
00:01.1 IDE interface: Intel Corporation 82371AB/EB/MB PIIX4 IDE (rev 01)
00:02.0 VGA compatible controller: InnoTek Systemberatung GmbH VirtualBox Graphics Adapter
00:03.0 Ethernet controller: Intel Corporation 82540EM Gigabit Ethernet Controller (rev 02)
00:04.0 System peripheral: InnoTek Systemberatung GmbH VirtualBox Guest Service
00:07.0 Bridge: Intel Corporation 82371AB/EB/MB PIIX4 ACPI (rev 08)
00:08.0 Ethernet controller: Intel Corporation 82540EM Gigabit Ethernet Controller (rev 02)
00:0e.0 Non-Volatile memory controller: InnoTek Systemberatung GmbH Device 4e56
vagrant@localhost:~$ sudo /spdk/examples/nvme/hello_world/hello_world
Starting DPDK 17.02.0 initialization...
[ DPDK EAL parameters: hello_world -c 0x1 --file-prefix=spdk_pid17681 ]
EAL: Detected 2 lcore(s)
EAL: Probing VFIO support...
Initializing NVMe Controllers
EAL: PCI device 0000:00:0e.0 on NUMA socket 0
EAL:   probe driver: 80ee:4e56 spdk_nvme
Attaching to 0000:00:0e.0
Attached to 0000:00:0e.0
Using controller ORCL-VBOX-NVME-VER12 (VB1234-56789        ) with 1 namespaces.
  Namespace ID: 1 size: 1GB
Initialization complete.
Hello world!
vagrant@localhost:~$
