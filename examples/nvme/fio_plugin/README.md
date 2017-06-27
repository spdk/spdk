Compiling
----------

First, clone the fio source repository from https://github.com/axboe/fio

    git clone https://github.com/axboe/fio

Then check out the fio 2.18 tag

    cd fio && git checkout fio-2.18

Finally, compile the code with

    ./configure && make

Next, edit the CONFIG file located in the root of the SPDK repository and set CONFIG_FIO_PLUGIN
to y and FIO_SOURCE_DIR to the location of the fio repository that was just created.

Further, you'll need to build DPDK with -fPIC set. You can do this by modifying your
DPDK config file (i.e. config/defconfig_x86_64-native-linuxapp-gcc) to include the line

    EXTRA_CFLAGS=-fPIC

At this point, build SPDK as per normal. The fio plugin will be placed in the same directory
as this README.

Usage
------

To use the SPDK fio plugin with fio, specify the plugin binary using LD_PRELOAD when running
fio and set ioengine=spdk in the fio configuration file (see example_config.fio in the same
directory as this README).

    LD_PRELOAD=<path to fio_plugin binary> fio

To select NVMe devices, you simply pass an identifier as the filename in the format

    filename=key=value [key=value] ... ns=value

Do not have any ':' in filename, otherwise it will be spilt into several file names. Also the
NVMe namespaces start at 1, not 0! The namespace must also be specified at the end.

Local PCIe NVMe devices will be specified using a PCI domain.bus.slot.func traddr format.  For example:

    filename=trtype=PCIe traddr=0000.04.00.0 ns=1

Remote devices accessed via NVMe over Fabrics will be specified based on the following example:

    filename=trtype=RDMA adrfam=IPv4 traddr=192.168.100.8 trsvcid=4420 ns=1

Currently the SPDK fio plugin is limited to thread usage model, so fio jobs must also specify thread=1
when using the SPDK fio plugin.

fio also currently has a race condition on shutdown if dynamically loading the ioengine by specifying the
engine's full path via the ioengine parameter - LD_PRELOAD is recommended to avoid this race condition.

When testing random workloads, it is recommended to set norandommap=1.  fio's random map
processing consumes extra CPU cycles which will degrade performance over time with
the fio_plugin since all I/O are submitted and completed on a single CPU core.
