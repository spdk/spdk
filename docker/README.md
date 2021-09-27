# SPDK Docker suite

This suite is meant to serve as an example of how SPDK can be encapsulated
into docker container images. The example containers consist of SPDK NVMe-oF
target sharing devices to another SPDK NVMe-oF application. Which serves
as both initiator and target. Finally a traffic generator based on FIO
issues I/O to the connected devices.

## Prerequisites

docker: We recommend version 20.10 and above because it supports cgroups v2 for
customization of host resources like CPUs, memory, and block I/O.

docker-compose: We recommend using 1.29.2 version or newer.

kernel: Hugepages must be allocated prior running the containers and hugetlbfs
mount must be available under /dev/hugepages. Also, tmpfs should be mounted
under /dev/shm. Depending on the use-case, some kernel modules should be also
loaded into the kernel prior running the containers.

proxy: If you are working behind firewall make sure dockerd is aware of the
proxy. Please refer to:
[docker-proxy](https://docs.docker.com/config/daemon/systemd/#httphttps-proxy)

To pass `$http_proxy` to docker-compose build use:
~~~{.sh}
docker-compose build --build-arg PROXY=$http_proxy
~~~

## How-To

`docker-compose.yaml` shows an example deployment of the storage containers based on SPDK.
Running `docker-compose build` creates 5 docker images:

- build_base
- storage-target
- proxy-container
- traffic-generator-nvme
- traffic-generator-virtio

The `build_base` image provides the core components required to containerize SPDK
applications. The fedora:33 image from the Fedora Container Registry is used and then SPDK is installed. SPDK is installed out of `build_base/spdk.tar.gz` provided.
See `build_base` folder for details on what's included in the final image.

Running `docker-compose up` creates 3 docker containers:

-- storage-target: Contains SPDK NVMe-oF target exposing single subsystem to
`proxy-container` based on malloc bdev.
-- proxy-container: Contains SPDK NVMe-oF target connecting to `storage-target`
and then exposing the same devices to `traffic-generator-nvme` using NVMe-oF and
to `traffic-generator-virtio` using Virtio.
-- traffic-generator-nvme: Contains FIO using SPDK plugin to connect to `proxy-container`
and runs a sample workload.
-- traffic-generator-virtio: Contains FIO using SPDK plugin to connect to `proxy-container`
and runs a sample workload.

Each container is connected to a separate "spdk" network which is created before
deploying the containers. See `docker-compose.yaml` for the network's detailed setup and ip assignment.

All the above boils down to:

~~~{.sh}
cd docker
tar -czf build_base/spdk.tar.gz --exclude='docker/*' -C .. .
docker-compose build
docker-compose up
~~~

The `storage-target` and `proxy-container` can be started as services.
Allowing for multiple traffic generator containers to connect.

~~~{.sh}
docker-compose up -d proxy-container
docker-compose run traffic-generator-nvme
docker-compose run traffic-generator-virtio
~~~

Enviroment variables to containers can be passed as shown in
[docs](https://docs.docker.com/compose/environment-variables/).
For example extra arguments to fio can be passed as so:

~~~{.sh}
docker-compose run -e FIO_ARGS="--minimal" traffic-generator-nvme
~~~

As each container includes SPDK installation it is possible to use rpc.py to
examine the final setup. E.g.:

~~~{.sh}
docker-compose exec storage-target rpc.py bdev_get_bdevs
docker-compose exec proxy-container rpc.py nvmf_get_subsystems
~~~

## Caveats

- If you run docker < 20.10 under distro which switched fully to cgroups2
  (e.g. f33) make sure that /sys/fs/cgroup/systemd exists otherwise docker/build
  will simply fail.
- Each SPDK app inside the containers is limited to single, separate CPU.
