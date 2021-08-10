# Using bdevperf application {#bdevperf}

## Introduction

bdevperf is an SPDK application that is used for performance testing
of block devices (bdevs) exposed by the SPDK bdev layer.  It is an
alternative to the SPDK bdev fio plugin for benchmarking SPDK bdevs.
In some cases, bdevperf can provide much lower overhead than the fio
plugin, resulting in much better performance for tests using a limited
number of CPU cores.

bdevperf exposes command line interface that allows to specify
SPDK framework options as well as testing options.
Since SPDK 20.07, bdevperf supports configuration file that is similar
to FIO. It allows user to create jobs parameterized by
filename, cpumask, blocksize, queuesize, etc.

## Config file

Bdevperf's config file is similar to FIO's config file format.

Below is an example config file that uses all available parameters:

~~~{.ini}
[global]
filename=Malloc0:Malloc1
bs=1024
iosize=256
rw=randrw
rwmixread=90

[A]
cpumask=0xff

[B]
cpumask=[0-128]
filename=Malloc1

[global]
filename=Malloc0
rw=write

[C]
bs=4096
iosize=128
offset=1000000
length=1000000
~~~

Jobs `[A]` `[B]` or `[C]`, inherit default values from `[global]`
section residing above them. So in the example, job `[A]` inherits
`filename` value and uses both `Malloc0` and `Malloc1` bdevs as targets,
job `[B]` overrides its `filename` value and uses `Malloc1` and
job `[C]` inherits value `Malloc0` for its `filename`.

Interaction with CLI arguments is not the same as in FIO however.
If bdevperf receives CLI argument, it overrides values
of corresponding parameter for all `[global]` sections of config file.
So if example config is used, specifying `-q` argument
will make jobs `[A]` and `[B]` use its value.

Below is a full list of supported parameters with descriptions.

Param     | Default           | Description
--------- | ----------------- | -----------
filename  |                   | Bdevs to use, separated by ":"
cpumask   | Maximum available | CPU mask. Format is defined at @ref cpu_mask
bs        |                   | Block size (io size)
iodepth   |                   | Queue depth
rwmixread | `50`              | Percentage of a mixed workload that should be reads
offset    | `0`               | Start I/O at the provided offset on the bdev
length    | 100% of bdev size | End I/O at `offset`+`length` on the bdev
rw        |                   | Type of I/O pattern

Available rw types:

- read
- randread
- write
- randwrite
- verify
- reset
- unmap
- write_zeroes
- flush
- rw
- randrw
