Compiling
----------

First, clone the fio source repository from http://github.com/axboe/fio

    git clone http://github.com/axboe/fio

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

To use the SPDK fio plugin with fio, simply set the following in the fio configuration file
(see example_config.fio in the same directory as this README).

    ioengine=<path to fio_plugin binary>

To select NVMe devices, you simply pass an identifier as the filename in the format

    domain.bus.slot.func/namespace

Remember that NVMe namespaces start at 1, not 0! Also, the notation uses '.' throughout,
not ':'. For example - 0000.04.00.0/1.

Currently the SPDK fio plugin is limited to a single thread, so only one job is supported.
fio jobs must also specify thread=1 when using the SPDK fio plugin.

When testing random workloads, it is recommended to set norandommap=1.  fio's random map
processing consumes extra CPU cycles which will degrade performance over time with
the fio_plugin since all I/O are submitted and completed on a single CPU core.
