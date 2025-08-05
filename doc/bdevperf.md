# bdevperf {#bdevperf}

## Introduction

bdevperf is an SPDK application used for performance testing
of block devices (bdevs) exposed by the SPDK bdev layer.  It is an
alternative to the SPDK bdev fio plugin for benchmarking SPDK bdevs.
In some cases, bdevperf can provide lower overhead than the fio
plugin, resulting in better performance and efficiency for tests
using a limited number of CPU cores.

bdevperf exposes command line interface that allows to specify
SPDK framework options as well as testing options.
bdevperf also supports a configuration file format similar
to FIO. It allows user to create jobs parameterized by
filename, cpumask, blocksize, queuesize, etc.

## Config file

bdevperf's config file format is similar to FIO.

Below is an example config file that uses all available parameters:

~~~{.ini}
[global]
filename=Malloc0:Malloc1
bs=1024
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

## JSON Output

`bdevperf` supports delivering test results in JSON format via the `bdevperf.py perform_tests`
RPC command. This feature allows users to programmatically parse and analyze test results.

### Enabling JSON Output

To enable JSON output, use the `bdevperf.py perform_tests` RPC command. The JSON response
includes detailed test results for each job and a summary of the test execution.
Below is an example of how to trigger a test and retrieve results in JSON format:

~~~{.sh}
sudo ./build/examples/bdevperf -c ./test/bdev/bdevperf/conf.json -m 0xFF -z
sudo PYTHONPATH=python ./examples/bdev/bdevperf/bdevperf.py perform_tests -q 16 -o 4096 -t 5 -w write
~~~

### Example JSON Output

The JSON response consists of two main sections:

1. **Results**: An array of objects, each representing a test job. Each object contains the following fields:

- `job`: The name of the job (e.g., `Malloc0`).
- `core_mask`: The CPU core mask used for the job.
- `workload`: The type of workload (e.g., `write`).
- `status`: The status of the test.
  * `finished` - the test completed successfully.
  * `failed`  - the test encountered an error.
  * `terminated` - the test was interrupted (e.g., via `SIGINT` or `Ctrl-C`).
- `queue_depth`: The queue depth used for the test.
- `io_size`: The I/O size in bytes.
- `runtime`: The test runtime in seconds.
- `iops`: The I/O operations per second achieved.
- `mibps`: The throughput in MiB/s.
- `io_failed`: The number of failed I/O operations.
- `io_timeout`: The number of timed-out I/O operations.
- `avg_latency_us`: The average latency in microseconds.
- `min_latency_us`: The minimum latency in microseconds.
- `max_latency_us`: The maximum latency in microseconds.

2. **Core Count**: The actual number of CPUs used during the test (note that `bdevperf` was run with `0xFF`
    mask, but only 2 cores were used).

#### Example

~~~{.json}
{
  "results": [
    {
      "job": "Malloc0",
      "core_mask": "0x1",
      "workload": "write",
      "status": "finished",
      "queue_depth": 16,
      "io_size": 4096,
      "runtime": 5.000048,
      "iops": 1200455.675625514,
      "mibps": 4689.279982912164,
      "io_failed": 0,
      "io_timeout": 0,
      "avg_latency_us": 13.252784244696382,
      "min_latency_us": 11.130434782608695,
      "max_latency_us": 222.6086956521739
    },
    {
      "job": "Malloc1",
      "core_mask": "0x2",
      "workload": "write",
      "status": "finished",
      "queue_depth": 16,
      "io_size": 4096,
      "runtime": 5.000024,
      "iops": 1216432.5611237066,
      "mibps": 4751.689691889479,
      "io_failed": 0,
      "io_timeout": 0,
      "avg_latency_us": 13.082916284986958,
      "min_latency_us": 5.7043478260869565,
      "max_latency_us": 90.82434782608695
    }
  ],
  "core_count": 2
}
~~~
