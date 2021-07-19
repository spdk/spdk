# Introduction

This directory contains a plug-in module for fio to enable use with SPDK. Fio is free software
published under version 2 of the GPL license.

## Compiling fio

Clone the [fio source repository](https://github.com/axboe/fio)

```bash
    git clone https://github.com/axboe/fio
    cd fio
```

Compile the fio code and install:

```bash
    make
    make install
```

## Compiling SPDK

Clone the [SPDK source repository](https://github.com/spdk/spdk)

```bash
    git clone https://github.com/spdk/spdk
    cd spdk
    git submodule update --init
```

Then, run the SPDK configure script to enable fio (point it to the root of the fio repository):

```bash
    cd spdk
    ./configure --with-fio=/path/to/fio/repo <other configuration options>
```

Finally, build SPDK:

```bash
    make
```

**Note to advanced users**: These steps assume you're using the DPDK submodule. If you are using your
own version of DPDK, the fio plugin requires that DPDK be compiled with -fPIC. You can compile DPDK
with -fPIC by modifying your DPDK configuration file and adding the line:

```bash
EXTRA_CFLAGS=-fPIC
```

## Usage

To use the SPDK fio plugin with fio, specify the plugin binary using LD_PRELOAD when running
fio and set ioengine=spdk_bdev in the fio configuration file (see example_config.fio in the same
directory as this README). Following example command assumes `fio` is in your system `$PATH` environment variable.

```bash
LD_PRELOAD=<path to spdk repo>/build/fio/spdk_bdev fio
```

The fio configuration file must contain parameter pointing to a JSON configuration file containing SPDK bdev configuration:

```bash
spdk_json_conf=./examples/bdev/fio_plugin/bdev.json
```

You can specify which block device to run against by setting the filename parameter
to the block device name:

```bash
filename=Malloc0
```

Or for NVMe devices:

```bash
filename=Nvme0n1
```

fio by default forks a separate process for every job. It also supports just spawning a separate
thread in the same process for every job. The SPDK fio plugin is limited to this latter thread
usage model, so fio jobs must also specify thread=1 when using the SPDK fio plugin. The SPDK fio
plugin supports multiple threads - in this case, the "1" just means "use thread mode".

fio also currently has a race condition on shutdown if dynamically loading the ioengine by specifying the
engine's full path via the ioengine parameter - LD_PRELOAD is recommended to avoid this race condition.

When testing random workloads, it is recommended to set norandommap=1.  fio's random map
processing consumes extra CPU cycles which will degrade performance over time with
the fio_plugin since all I/O are submitted and completed on a single CPU core.

### Step-by-step usage examples

These examples assume you have built fio and SPDK with `--with-fio` option enabled.

#### Using fio bdev plugin with local NVMe storage

- Bind local NVMe drives to userspace driver

- Run gen_nvme.sh script to create a JSON file with bdev subsystem configuration

    ```bash
    scripts/gen_nvme.sh --json-with-subsystems > /tmp/bdev.json

    cat /tmp/bdev_local.json  | jq
    {
        "subsystems": [
        {
            "subsystem": "bdev",
            "config": [
            {
                "method": "bdev_nvme_attach_controller",
                "params": {
                "trtype": "PCIe",
                "name": "Nvme0",
                "traddr": "0000:0a:00.0"
                }
            },
            {
                "method": "bdev_nvme_attach_controller",
                "params": {
                "trtype": "PCIe",
                "name": "Nvme1",
                "traddr": "0000:85:00.0"
                }
            }
            ]
        }
        ]
    }
    ```

- Prepare fio configuration file

    ```bash
    cat /tmp/fio.conf

    [global]
    ioengine=/spdk/build/fio/spdk_bdev
    spdk_json_conf=/tmp/bdev.json

    thread=1
    direct=1
    group_reporting=1

    bs=4k
    rw=randread
    rwmixread=70
    time_based=1
    runtime=10
    norandommap=1

    [filename0]
    filename=Nvme0n1
    filename=Nvme1n1
    iodepth=8
    ```

- Run fio with spdk bdev plugin

    ```bash
    /usr/src/fio/fio /tmp/fio.conf
    ```

#### Using fio bdev plugin as NVMe-oF initiator with remote storage

- Start SPDK NVMe-oF Target process and configure it with block devices and NVMe-oF subsystems

    ```bash
    build/bin/nvmf_tgt &
    sleep 3
    scripts/rpc.py bdev_malloc_create 10 512 -b Malloc0
    scripts/rpc.py nvmf_create_transport -t TCP
    scripts/rpc.py nvmf_create_subsystem nqn.2018-09.io.spdk:cnode1 -a -s S000001
    scripts/rpc.py nvmf_subsystem_add_listener nqn.2018-09.io.spdk:cnode1 -t tcp -f ipv4 -s 4420 -a 10.0.0.1
    scripts/rpc.py nvmf_subsystem_add_ns nqn.2018-09.io.spdk:cnode1 Malloc0
    ```

- Run gen_nvme.sh script to prepare a JSON file containing bdev subsystem configuration
  for initiator which will allow it to connect to target

    ```bash
    scripts/gen_nvme.sh --json-with-subsystems --mode=remote \
    --trid=tcp:10.0.0.1:4420:nqn.2018-09.io.spdk:cnode1 > /tmp/bdev.json

    cat /tmp/bdev.json | jq
    {
        "subsystems": [
        {
            "subsystem": "bdev",
            "config": [
            {
                "method": "bdev_nvme_attach_controller",
                "params": {
                "trtype": "tcp",
                "adrfam": "IPv4",
                "name": "Nvme0",
                "subnqn": "nqn.2018-09.io.spdk:cnode1",
                "traddr": "10.0.0.1",
                "trsvcid": "4420"
                }
            }
            ]
        }
        ]
    }
    ```

- Prepare fio configuration file

    ```bash
    cat /tmp/fio.conf

    [global]
    ioengine=/spdk/build/fio/spdk_bdev
    spdk_json_conf=/tmp/bdev.json

    thread=1
    direct=1
    group_reporting=1

    bs=4k
    rw=randread
    rwmixread=70
    time_based=1
    runtime=10
    norandommap=1

    [filename0]
    filename=Nvme0n1
    iodepth=8
    ```

- Run fio bdev plugin as initiator

    ```bash
    /usr/src/fio/fio /tmp/fio.conf
    ```

## Zoned Block Devices

SPDK has a zoned block device API (bdev_zone.h) which currently supports Open-channel SSDs,
NVMe Zoned Namespaces (ZNS), and the virtual zoned block device SPDK module.

If you wish to run fio against a SPDK zoned block device, you can use the fio option:

```bash
zonemode=zbd
```

It is recommended to use a fio version newer than version 3.26, if using --numjobs > 1.
If using --numjobs=1, fio version >= 3.23 should suffice.

See zbd_example.fio in this directory for a zoned block device example config.

### Maximum Open Zones

Most zoned block devices have a resource constraint on the amount of zones which can be in an opened
state at any point in time. It is very important to not exceed this limit.

You can control how many zones fio will keep in an open state by using the
`--max_open_zones` option.

If you use a fio version newer than 3.26, fio will automatically detect and set the proper value.
If you use an old version of fio, make sure to provide the proper --max_open_zones value yourself.

### Maximum Active Zones

Zoned block devices may also have a resource constraint on the number of zones that can be active at
any point in time. Unlike `max_open_zones`, fio currently does not manage this constraint, and
there is thus no option to limit it either.

Since the max active zones limit (by definition) has to be greater than or equal to the max open
zones limit, the easiest way to work around that fio does not manage this constraint, is to start
with a clean state each run (except for read-only workloads), by resetting all zones before fio
starts running its jobs by using the engine option:

```bash
--initial_zone_reset=1
```

### Zone Append

When running fio against a zoned block device you need to specify --iodepth=1 to avoid
"Zone Invalid Write: The write to a zone was not at the write pointer." I/O errors.
However, if your zoned block device supports Zone Append, you can use the engine option:

```bash
--zone_append=1
```

To send zone append commands instead of write commands to the zoned block device.
When using zone append, you will be able to specify a --iodepth greater than 1.
