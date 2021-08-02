# Introduction

This directory contains a plug-in module for fio to enable use
with SPDK. Fio is free software published under version 2 of
the GPL license.

## Compiling fio

Clone the fio source repository from https://github.com/axboe/fio

    git clone https://github.com/axboe/fio
    cd fio

Compile the fio code and install:

    make
    make install

## Compiling SPDK

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

## Usage

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

fio by default forks a separate process for every job. It also supports just spawning a separate
thread in the same process for every job. The SPDK fio plugin is limited to this latter thread
usage model, so fio jobs must also specify thread=1 when using the SPDK fio plugin. The SPDK fio
plugin supports multiple threads - in this case, the "1" just means "use thread mode".

fio also currently has a race condition on shutdown if dynamically loading the ioengine by specifying the
engine's full path via the ioengine parameter - LD_PRELOAD is recommended to avoid this race condition.

When testing random workloads, it is recommended to set norandommap=1.  fio's random map
processing consumes extra CPU cycles which will degrade performance over time with
the fio_plugin since all I/O are submitted and completed on a single CPU core.

## Zoned Block Devices

SPDK has a zoned block device API (bdev_zone.h) which currently supports Open-channel SSDs,
NVMe Zoned Namespaces (ZNS), and the virtual zoned block device SPDK module.

If you wish to run fio against a SPDK zoned block device, you can use the fio option:

    zonemode=zbd

It is recommended to use a fio version newer than version 3.26, if using --numjobs > 1.
If using --numjobs=1, fio version >= 3.23 should suffice.

See zbd_example.fio in this directory for a zoned block device example config.

### Maximum Open Zones

Most zoned block devices have a resource constraint on the amount of zones which can be in an opened
state at any point in time. It is very important to not exceed this limit.

You can control how many zones fio will keep in an open state by using the
``--max_open_zones`` option.

If you use a fio version newer than 3.26, fio will automatically detect and set the proper value.
If you use an old version of fio, make sure to provide the proper --max_open_zones value yourself.

### Maximum Active Zones

Zoned block devices may also have a resource constraint on the number of zones that can be active at
any point in time. Unlike ``max_open_zones``, fio currently does not manage this constraint, and
there is thus no option to limit it either.

Since the max active zones limit (by definition) has to be greater than or equal to the max open
zones limit, the easiest way to work around that fio does not manage this constraint, is to start
with a clean state each run (except for read-only workloads), by resetting all zones before fio
starts running its jobs by using the engine option:

    --initial_zone_reset=1

### Zone Append

When running fio against a zoned block device you need to specify --iodepth=1 to avoid
"Zone Invalid Write: The write to a zone was not at the write pointer." I/O errors.
However, if your zoned block device supports Zone Append, you can use the engine option:

    --zone_append=1

To send zone append commands instead of write commands to the zoned block device.
When using zone append, you will be able to specify a --iodepth greater than 1.
