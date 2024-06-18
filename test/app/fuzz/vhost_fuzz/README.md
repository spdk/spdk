# Overview

This application is intended to fuzz test the SPDK vhost target by supplying
malformed or invalid requests across a unix domain socket. This fuzzer
currently supports fuzzing both vhost block and vhost scsi devices. When
fuzzing a vhost scsi device, users can select whether to fuzz the scsi I/O
queue or the scsi admin queue. Please see the NVMe fuzzer readme for information
on how output is generated, debugging procedures, and the JSON format expected
when supplying preconstructed values to the fuzzer.

## Request Types

Like the NVMe fuzzer, there is an example json file showing the types of requests
that the application accepts. Since the vhost application accepts both vhost block
and vhost scsi commands, there are three distinct object types that can be passed in
to the application.

1. vhost_blk_cmd
2. vhost_scsi_cmd
3. vhost_scsi_mgmt_cmd

Each one of these objects contains distinct data types and they should not be used interchangeably.

All three of the data types begin with three iovec structures describing the request, data, and response
memory locations. By default, these values are overwritten by the application even when supplied as part
of a json file. This is because the request and resp data pointers are intended to point to portions of
the data structure.

If you want to override these iovec values using a json file, you can specify the -k option.
In most cases, this will just result in the application failing all I/O immediately since
the request will no longer point to a valid memory location.

It is possible to supply all three types of requests in a single array to the application. They will be parsed and
submitted to the proper block devices.

## RPC

The vhost fuzzer differs from the NVMe fuzzer in that it expects devices to be configured via rpc. The fuzzer should
always be started with the --wait-for-rpc argument. Please see below for an example of starting the fuzzer.

~~~bash
./test/app/fuzz/vhost_fuzz/vhost_fuzz -t 30 --wait-for-rpc &
./scripts/rpc.py fuzz_vhost_create_dev -s ./Vhost.1 -b -V
./scripts/rpc.py fuzz_vhost_create_dev -s ./naa.VhostScsi0.1 -l -V
./scripts/rpc.py framework_start_init
~~~
