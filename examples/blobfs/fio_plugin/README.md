# Compiling fio

Clone the fio source repository from https://github.com/axboe/fio

    git clone https://github.com/axboe/fio
    cd fio

Compile the fio code and install:

    make
    make install

# Compiling SPDK

Clone the SPDK source repository from https://github.com/spdk/spdk

    git clone https://github.com/spdk/spdk
    cd spdk
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

Make BlobFS by mkfs provided in the repo test/blobfs/mkfs before test

    <path to spdk repo>/test/blobfs/mkfs/mkfs <path to spdk repo>/examples/blobfs/fio_plugin/bdev.conf Nvme0n1

To use the SPDK fio plugin with fio, specify the plugin binary using LD_PRELOAD when running
fio and set ioengine=spdk_blobfs in the fio configuration file (see example_config.fio in the same
directory as this README).

    LD_PRELOAD=<path to spdk repo>/examples/blobfs/fio_plugin/fio_plugin fio <path to>/example_config.fio

The fio configuration file must contain one new parameter:

    spdk_conf=./examples/blobfs/fio_plugin/bdev.conf.in

This must point at an SPDK configuration file. There are a number of example configuration
files in the SPDK repository under etc/spdk.

And, you should specify the block device to run against by setting the bdev_name parameter
in fio configuration file too:

    bdev_name=Nvme0n1

Currently the SPDK fio plugin is limited to the thread usage model, so fio jobs must also specify thread=1
when using the SPDK fio plugin.

fio also currently has a race condition on shutdown if dynamically loading the ioengine by specifying the
engine's full path via the ioengine parameter - LD_PRELOAD is recommended to avoid this race condition.

When testing random workloads, it is recommended to set norandommap=1.  fio's random map
processing consumes extra CPU cycles which will degrade performance over time with
the fio_plugin since all I/O are submitted and completed on a single CPU core.
