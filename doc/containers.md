# SPDK and Containers {#containers}

This is a living document as there are many ways to use containers with
SPDK. As new usages are identified and tested, they will be documented
here.

## In this document {#containers_toc}

* @ref kata_containers_with_spdk_vhost
* @ref spdk_in_docker

## Using SPDK vhost target to provide volume service to Kata Containers and Docker {#kata_containers_with_spdk_vhost}

[Kata Containers](https://katacontainers.io) can build a secure container
runtime with lightweight virtual machines that feel and perform like
containers, but provide stronger workload isolation using hardware
virtualization technology as a second layer of defense.

From Kata Containers [1.11.0](https://github.com/kata-containers/runtime/releases/tag/1.11.0),
vhost-user-blk support is enabled in `kata-containers/runtime`. That is to say
SPDK vhost target can be used to provide volume service to Kata Containers directly.
In addition, a container manager like Docker, can be configured easily to launch
a Kata container with an SPDK vhost-user block device. For operating details, visit
Kata containers use-case [Setup to run SPDK vhost-user devices with Kata Containers and Docker](https://github.com/kata-containers/documentation/blob/master/use-cases/using-SPDK-vhostuser-and-kata.md#host-setup-for-vhost-user-devices)

## Containerizing an SPDK Application for Docker {#spdk_in_docker}

There are no SPDK specific changes needed to run an SPDK based application in
a docker container, however this quick start guide should help you as you
containerize your SPDK based application.

1. Make sure you have all of your app dependencies identified and included in your Dockerfile
2. Make sure you have compiled your application for the target arch
3. Make sure your host has hugepages enabled
4. Make sure your host has bound your nvme device to your userspace driver
5. Write your Dockerfile. The following is a simple Dockerfile to containerize the nvme `hello_world`
  example:

~~~{.sh}
# start with the latest Fedora
FROM fedora

# if you are behind a proxy, set that up now
ADD dnf.conf /etc/dnf/dnf.conf

# these are the min dependencies for the hello_world app
RUN dnf install libaio-devel -y
RUN dnf install numactl-devel -y

# set our working dir
WORKDIR /app

# add the hello_world binary
ADD hello_world hello_world

# run the app
CMD ./hello_world
~~~

6. Create your image

`sudo docker image build -t hello:1.0 .`

7. You docker command line will need to include at least the following:
- the `--privileged` flag to enable sharing of hugepages
- use of the `-v` switch to map hugepages

`sudo docker run --privileged -v /dev/hugepages:/dev/hugepages hello:1.0`

or depending on the needs of your app you may need one or more of the following parameters:

- If you are using the SPDK app framework: `-v /dev/shm:/dev/shm`
- If you need to use RPCs from outside of the container: `-v /var/tmp:/var/tmp`
- If you need to use the host network (i.e. NVMF target application): `--network host`

Your output should look something like this:

~~~{.sh}
$ sudo docker run --privileged -v //dev//hugepages://dev//hugepages hello:1.0
Starting SPDK v20.01-pre git sha1 80da95481 // DPDK 19.11.0 initialization...
[ DPDK EAL parameters: hello_world -c 0x1 --log-level=lib.eal:6 --log-level=lib.cryptodev:5 --log-level=user1:6 --iova-mode=pa
--base-virtaddr=0x200000000000 --match-allocations --file-prefix=spdk0 --proc-type=auto ]
EAL: No available hugepages reported in hugepages-1048576kB
Initializing NVMe Controllers
Attaching to 0000:06:00.0
Attached to 0000:06:00.0
Using controller INTEL SSDPEDMD400G4  (CVFT7203005M400LGN  ) with 1 namespaces.
  Namespace ID: 1 size: 400GB
Initialization complete.
INFO: using host memory buffer for IO
Hello world!
~~~
