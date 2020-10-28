# Compiling fio

First, clone the fio source repository from https://github.com/axboe/fio

    git clone https://github.com/axboe/fio

Then check out the latest fio version and compile the code:

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

    LD_PRELOAD=<path to spdk repo>/build/fio/spdk_nvme fio

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

When testing FIO on multiple NVMe SSDs with SPDK plugin, it is recommended to use multiple jobs in FIO configurion.
It has been observed that there are some performance gap between FIO(with SPDK plugin enabled) and SPDK perf
(examples/nvme/perf/perf) on testing multiple NVMe SSDs. If you use one job(i.e., use one CPU core) configured for
FIO test, the performance is worse than SPDK perf (also using one CPU core) against many NVMe SSDs. But if you use
multiple jobs for FIO test, the performance of FIO is similiar with SPDK perf. After analyzing this phenomenon, we
think that is caused by the FIO architecture. Mainly FIO can scale with multiple threads (i.e., using CPU cores),
but it is not good to use one thread against many I/O devices.

# End-to-end Data Protection (Optional)

Running with PI setting, following settings steps are required.
First, format device namespace with proper PI setting. For example:

    nvme format /dev/nvme0n1 -l 1 -i 1 -p 0 -m 1

In fio configure file, add PRACT and set PRCHK by flags(GUARD|REFTAG|APPTAG) properly. For example:

    pi_act=0
    pi_chk=GUARD

Blocksize should be set as the sum of data and metadata. For example, if data blocksize is 512 Byte, host generated
PI metadata is 8 Byte, then blocksize in fio configure file should be 520 Byte:

    bs=520

The storage device may use a block format that requires separate metadata (DIX). In this scenario, the fio_plugin
will automatically allocate an extra 4KiB buffer per I/O to hold this metadata. For some cases, such as 512 byte
blocks with 32 metadata bytes per block and a 128KiB I/O size, 4KiB isn't large enough. In this case, the
`md_per_io_size` option may be specified to increase the size of the metadata buffer.

Expose two options 'apptag' and 'apptag_mask', users can change them in the configuration file when using
application tag and application tag mask in end-to-end data protection.  Application tag and application
tag mask are set to 0x1234 and 0xFFFF by default.

# VMD (Optional)

To enable VMD enumeration add enable_vmd flag in fio configuration file:

    enable_vmd=1

# ZNS

To use Zoned Namespaces then build the io-engine against, and run using, a fio version >= 3.23 and add:

    zonemode=zbd

To your fio-script, also have a look at script-examples provided with fio:

    fio/examples/zbd-seq-read.fio
    fio/examples/zbd-rand-write.fio

## Maximum Open Zones

Zoned Namespaces has a resource constraint on the amount of zones which can be in an opened state at
any point in time. You can control how many zones fio will keep in an open state by using the
``--max_open_zones`` option.

The SPDK/NVMe fio io-engine will set a default value if you do not provide one.

## Maximum Active Zones

Zoned Namespaces has a resource constraint on the number of zones that can be active at any point in
time. Unlike ``max_open_zones``, then fio currently do not manage this constraint, and there is thus
no option to limit it either.

When running with the SPDK/NVMe fio io-engine you can be exposed to error messages, in the form of
completion errors, with the NVMe status code of 0xbd ("Too Many Active Zones"). To work around this,
then you can reset all zones before fio start running its jobs by using the engine option:

    --initial_zone_reset=1

## Shared Memory Increase

If your device has a lot of zones, fio can give you errors such as:

    smalloc: OOM. Consider using --alloc-size to increase the shared memory available.

This is because fio needs to allocate memory for the zone-report, that is, retrieve the state of
zones on the device including auxiliary accounting information. To solve this, then you can follow
fio's advice and increase ``--alloc-size``.
