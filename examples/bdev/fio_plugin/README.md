# Introduction

This directory contains a plug-in module for fio to enable use
with SPDK. Fio is free software published under version 2 of
the GPL license.

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

To use the SPDK fio plugin with fio, specify the plugin binary using LD_PRELOAD when running
fio and set ioengine=spdk_bdev in the fio configuration file (see example_config.fio in the same
directory as this README).

    LD_PRELOAD=<path to spdk repo>/build/fio/spdk_bdev fio

The fio configuration file must contain one new parameter:

    spdk_json_conf=./examples/bdev/fio_plugin/bdev.json

You can specify which block device to run against by setting the filename parameter
to the block device name:

    filename=Malloc0

Or for NVMe devices:

    filename=Nvme0n1

Currently the SPDK fio plugin is limited to the thread usage model, so fio jobs must also specify thread=1
when using the SPDK fio plugin.

fio also currently has a race condition on shutdown if dynamically loading the ioengine by specifying the
engine's full path via the ioengine parameter - LD_PRELOAD is recommended to avoid this race condition.

When testing random workloads, it is recommended to set norandommap=1.  fio's random map
processing consumes extra CPU cycles which will degrade performance over time with
the fio_plugin since all I/O are submitted and completed on a single CPU core.
