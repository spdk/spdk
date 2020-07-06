## NVMe device performance testing workflow with Popper

[Popper](https://github.com/getpopper/popper) is a container-native workflow execution and task automation engine for defining and executing container-native workflows in Docker, as well as other container engines. 
More details about Popper can be found [here](https://popper.readthedocs.io/).

This folder contains a Popper workflow defined in the `bdev-vs-libaio.yml` file that automatically run benchmarks for a NVMe device with two different I/O engines: Linux-native asynchronous I/O access library (libaio) engine against the [SPDK block device layer](https://spdk.io/doc/bdev.html) through the [fio](https://fio.readthedocs.io/en/latest/fio_doc.html) tool, similarly to the tests presented in the [SPDK NVMe BDEV Performance Report Release 18.04](https://ci.spdk.io/download/performance-reports/SPDK_nvme_bdev_perf_report_18.04.pdf), and plots the results.

These benchmarks tests each engine performance for accessing a device with NMVe protocol and measures the IOPS (Input/Output operations per second) under three different operation workloads: 
- 100% Random read workload
- 100% Random write workload
- Random mixed workload (70% Read 30% Write)

Not without preconditioning which formats the device and ensures that it has reached steady state through the precondition workload for both read and write workloads. A steady state is the state of the SSD device where the IOPS variation in time will not be significantly affecting the measurments of the tests. 

> More information on preconditioning SSD devices for performance testing can be found [here](https://www.snia.org/sites/default/files/technical_work/PTS/SSS_PTS_2.0.1.pdf)

### Steps
The workflow contains the following steps:
<p align="center">
  <img src="https://user-images.githubusercontent.com/33427387/86626258-4e136d00-bf7b-11ea-95fe-7ed924fd12dd.png">
</p>

> Diagram above obtained with:
 > `popper dot -f scripts/perf/popper/bdev-vs-libaio.yml | dot -Tpng -o wf.png`
 
 1. **build-img**: Builds a docker image that builds both fio and spdk.
 2. **prepare**: Format the NVMe device and prepares it for the tests.
 3. **run-read-benchmark**: Preocondition the device and runs the random reads benchmarks.
 4. **run-write-benchmark**: Preocondition the device and runs the random write benchmarks.
 5. **run-mix-benchmark**: Runs mixed workloads benchmarks.
 6. **plot-results**: Generate plots with the results in a PDF file.

## Getting Started:

1. Install [Docker](https://docs.docker.com/get-docker/) if you don't have it already.


2. Install Popper
```
curl -sSfL https://raw.githubusercontent.com/getpopper/popper/master/install.sh | sh
```

3. Clone the repository.
```
git clone https://github.com/spdk/spdk.git
```

## Executing
**WARNING: Running this workflow will erase *everything* in the NVMe device that you will measure**

NOTE: This workflow points to a device mounted in `/dev/nvme0n1`, to specify other directory to run the tests please update the `DEVICE` variable defined in the `bdev-vs-libaio.yml` file in the `options` section:

```
options:
  env:
    DEVICE: /dev/nvme0n1
    ...
```
To run the workflow:

```
cd spdk/
popper run -f scripts/perf/popper/bdev-vs-libaio.yml -c scripts/perf/popper/config.yml
```
Any step defined in the `bdev-vs-libaio.yml` can be executed in interactive mode. For example, to open a shell in the `prepare` step:

```
popper sh -f scripts/perf/popper/bdev-vs-libaio.yml prepare
```