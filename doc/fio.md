# Fio {#Fio}

# Fio with SPDK Getting Started Guide

Now SPDK can support fio with nvme driver and bdev layer engine.
Fio(flexible I/O Tester) is a powerful tool which can be used to generate specified workload and get
desired output.

## Fio parameters introduction
Fio has a lot of parameters. Some parameters which are used frequently are listed below.
The fio version of the parameters are 2.21 which is same version used in SPDK.
ioengine:
defines how the job issues I/O to the file. The following type is normally used:
libaio
Linux native asynchronous I/O. Note that Linux may only support queued behavior with non-buffered I/O (set 'direct=1' or 'buffered=0').

rw:
Types of I/O pattern, Accepted values are:
read
Sequential reads.
write
Sequential write
trim
Sequential trims (Linux block devices only).
randread
Random reads.
randwrite
Random writes.
randtrim
rw, readwrite
Sequential mixed reads and writes.
randrw
Random mixed reads and writes.
trimwrite
Sequential trim+write sequences. Blocks will be trimmed first, then the same blocks will be
written to.

blocksize, bs:
The  block size in bytes for I/O units.  Default: 4096. For example, if you want to run 1MB io size for
each unit. You need to input blocksize=1048576 or bs=1048576.

runtime:
Terminate processing after the specified number of seconds.
The Fio will finish earlier if the file was completely read or written. If you want to force the fio runnng, you need to set the time_based parameter.

time_base:
If given, run for the specified runtime duration even if the files are completely read or written. The same workload will be repeated  as  many  times  as runtime allows.

iodepth:
Number of I/O units to keep in flight against the file.

thread:
Fio defaults to creating jobs by using fork, however if this option is given, fio will create jobs by using POSIX Threads' function "pthread_create" to create threads instead.

filename:
fio  normally  makes  up  a file name based on the job name, thread number, and file number.

invalidate:
Invalidate buffer-cache for the file prior to starting I/O.  Default: true.

direct:
If value is true, use non-buffered I/O. This is usually O_DIRECT.

norandommap:
Normally fio will cover every block of the file when doing random I/O. If this parameter is given, a new offset will be chosen without looking at past  I/O history. This means that some blocks may not be read or written, and that some blocks may be read/written more than once. This parameter is mutually exclusive with verify.

do_verify:
Run the verify phase after a write phase.  Only valid if verify is set.  Default: true.

verify:
Method of verifying file contents after each iteration of the job. Each verification method also implies verification of special header, which  is  written to  the beginning of each block. This header also includes meta information, like offset of the block, block number, timestamp when block was written, etc. verify=str can be combined with verify_pattern=str option.  The allowed values are:
md5 crc16 crc32 crc32c crc32c-intel crc64 crc7 sha256 sha512 sha1 sha3-224 sha3-256 sha3-384 sha3-512 xxhash

verify_dump:
If  set,  dump  the  contents of both the original data block and the data block we read off disk to files. This allows later analysis to inspect just what kind of data corruption occurred. Off by default.

A sample fio job file is as below:
[global]
thread=1
invalidate=1
rw=randread
time_based=1
runtime=12
ioengine=libaio
direct=1
bs=4096
iodepth=1
norandommap=1
verify_dump=1
do_verify=1
verify=crc32c-intel

[job0]
filename=/dev/nvme0n1

## run fio with SPDK nvme driver engine

Please refer to spdk/examples/nvme/fio_plugin/README.md about how to run fio with SPDK nvme driver engine.

## run fio with SPDK bdev layer engine

Please refer to spdk/examples/bdev/fio_plugin/README.md about how to run fio with SPDK nvme driver engine.

##Note

You can run spdk/scripts/fio.py to test iSCSI target.
