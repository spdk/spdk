# Compiling fio

First, clone the fio source repository from https://github.com/axboe/fio

    git clone https://github.com/axboe/fio

Then check out the fio 2.21:

    cd fio && git checkout fio-2.21

Finally, compile the code:

    make

# Compiling SPDK

First, clone the SPDK source repository from https://github.com/spdk/spdk

    git clone https://github.com/spdk/spdk
    git submodule update --init

Then, run the SPDK configure script to enable fio (point it to the root of the fio repository):

    cd spdk
    ./configure --with-fio=/path/to/fio/repo <other configuration options>

Finally, build SPDK:

    make

**Note to advanced users**: These steps assume you're using the DPDK submodule. If you are using your
own version of DPDK, the fio plugin requires that DPDK be compiled with -fPIC. You can compile DPDK
with -fPIC by modifying your DPDK configuration file and adding the line:

    EXTRA_CFLAGS=-fPIC

# Usage

To use the SPDK fio plugin with fio, specify the plugin binary using LD_PRELOAD when running
fio and set ioengine=spdk in the fio configuration file (see example_config.fio in the same
directory as this README).

    LD_PRELOAD=<path to spdk repo>/examples/nvme/fio_plugin/fio_plugin fio

To select NVMe devices, you pass an SPDK Transport Identifier string as the filename. These are in the
form:

    filename=key=value [key=value] ... ns=value

Specifically, for local PCIe NVMe devices it will look like this:

    filename=trtype=PCIe traddr=0000.04.00.0 ns=1

And remote devices accessed via NVMe over Fabrics will look like this:

    filename=trtype=RDMA adrfam=IPv4 traddr=192.168.100.8 trsvcid=4420 ns=1


**Note**: The specification of the PCIe address should not use the normal ':'
and instead only use '.'. This is a limitation in fio - it splits filenames on
':'. Also, the NVMe namespaces start at 1, not 0, and the namespace must be
specified at the end of the string.

Currently the SPDK fio plugin is limited to the thread usage model, so fio jobs must also specify thread=1
when using the SPDK fio plugin.

fio also currently has a race condition on shutdown if dynamically loading the ioengine by specifying the
engine's full path via the ioengine parameter - LD_PRELOAD is recommended to avoid this race condition.

When testing random workloads, it is recommended to set norandommap=1.  fio's random map
processing consumes extra CPU cycles which will degrade performance over time with
the fio_plugin since all I/O are submitted and completed on a single CPU core.
