# Getting Started Guide {#nvmf_getting_started}

The NVMe over Fabrics target is a user space application that presents block devices over the
network using RDMA. It requires an RDMA-capable NIC with its corresponding OFED software package
installed to run. The target should work on all flavors of RDMA, but it is currently tested against
Mellanox NICs (RoCEv2) and Chelsio NICs (iWARP).

The NVMe over Fabrics specification defines subsystems that can be exported over the network. SPDK
has chosen to call the software that exports these subsystems a "target", which is the term used
for iSCSI. The specification refers to the "client" that connects to the target as a "host". Many
people will also refer to the host as an "initiator", which is the equivalent thing in iSCSI
parlance. SPDK will try to stick to the terms "target" and "host" to match the specification.

There will be both a target and a host implemented in the Linux kernel, and these are available
today as a set of patches against the kernel 4.8 release candidate. All of the testing against th
SPDK target has been against the proposed Linux kernel host. This means that for at least the host
machine, the kernel will need to be a release candidate until the code is actually merged. For the
system running the SPDK target, however, you can run any modern flavor of Linux as required by your
NIC vendor's OFED distribution.

# Prerequisites {#nvmf_prereqs}

This guide starts by assuming that you can already build the standard SPDK distribution on your
platform. By default, the NVMe over Fabrics target is not built. To build nvmf_tgt there are some
additional dependencies.

Fedora:
~~~{.sh}
dnf install libibverbs-devel librdmacm-devel
~~~

Ubuntu:
~~~{.sh}
apt-get install libibverbs-dev librdmacm-dev
~~~

Then build SPDK with RDMA enabled, either by editing CONFIG to enable CONFIG_RDMA or
enabling it on the `make` command line:

~~~{.sh}
make CONFIG_RDMA=y <other config parameters>
~~~

Once built, the binary will be in `app/nvmf_tgt`.

# Configuring NVMe over Fabrics Target {#nvmf_config}

A `nvmf_tgt`-specific configuration file is used to configure the NVMe over Fabrics target. This
file's primary purpose is to define subsystems. A fully documented example configuration file is
located at `etc/spdk/nvmf.conf.in`.

You should make a copy of the example configuration file, modify it to suit your environment, and
then run the nvmf_tgt application and pass it the configuration file using the -c option. Right now,
the target requires elevated privileges (root) to run.

~~~{.sh}
app/nvmf_tgt/nvmf_tgt -c /path/to/nvmf.conf
~~~
