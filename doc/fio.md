# Fio {#fio}

# Fio with SPDK Getting Started Guide

Now SPDK can support Fio with nvme driver and bdev layer engine.

Fio with nvme driver is to directly integrate with SPDK NVMe driver through those public APIs. Details refer to spdk/examples/nvme/fio_plugin/README.md.

Fio with bdev layer is to integrate with SPDK common block layer and can support different backend block devices. Details refer to spdk/examples/bdev/fio_plugin/README.md.

Usage of these two Fio supports has different aspects:

1. Fio with nvme driver is more focusing and suitable on the I/O performance testing with NVMe SSD backend.
2. Fio with bdev layer is to have more generic testing and functionalities with common block device handling.

## Fio parameters introduction
Fio(flexible I/O Tester) is a powerful tool which can be used to generate specified workload and get desired output.
Fio has a lot of parameters. Some parameters which are used frequently are listed below.
The Fio version of the parameters are 3.3 which is same version used in SPDK.
ioengine:
defines how the job issues I/O to the file. The following type is normally used:
libaio
Linux native asynchronous I/O. Note that Linux may only support queued behavior
with non-buffered I/O (set 'direct=1' or 'buffered=0').

rw:
Types of I/O pattern.

blocksize, bs:
The block size in bytes used for I/O units. Default: 4096. For example, if you want to run 1MB io size for
each unit. You need to input blocksize=1048576 or bs=1048576.

runtime:
Tell Fio to terminate processing after the specified period of time.
The Fio will finish earlier if the file was completely read or written. If you want to force the fio runnng, you need to set the time_based parameter.

time_base:
If given, run for the specified runtime duration even if the file(s) are completely read or written. The same workload will be repeated as many times as runtime allows.

iodepth:
Number of I/O units to keep in flight against the file.

thread:
Fio defaults to creating jobs by using fork, however if this option is given, fio will create jobs by using POSIX Threads' function "pthread_create" to create threads instead.

filename:
Fio normally makes up a file name based on the job name, thread number, and file number. If you want to share files between threads in a job or several jobs with fixed
file paths, specify a filename for each of them to override the default.

invalidate:
Invalidate the buffer/page cache parts of the files to be used prior to starting I/O if the platform and file type support it. Default: true.

direct:
If value is true, use non-buffered I/O. This is usually O_DIRECT.

norandommap:
Normally fio will cover every block of the file when doing random I/O. If this parameter is given, fio will get a new offset without looking at past I/O history. This means that some blocks may not be read or written, and that some blocks may be read/written more than once. If this option is used with verify and multiple blocksize ( via "bsrange"), only intact blocks are verified, i.e., partially overwritten blocks are ignored.

do_verify:
Run the verify phase after a write phase.  Only valid if verify is set.  Default: true.

verify:
If writing to a file, fio can verify the file contents after each iteration of the job. Each verification method also implies verification of special header, which  is  written to  the beginning of each block. This header also includes meta information, like offset of the block, block number, timestamp when block was written, etc. verify=str can be combined with verify_pattern=str option. The allowed values are:
md5 crc16 crc32 crc32c crc32c-intel crc64 crc7 sha256 sha512 sha1 sha3-224 sha3-256 sha3-384 sha3-512 xxhash

verify_dump:
If set, dump the contents of both the original data block and the data block we read off disk to files. This allows later analysis to inspect just what kind of data corruption occurred. Off by default.

A sample fio job file is as below:
[global]
thread=1
invalidate=1
rw=randwrite
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

[job1]
filename=/dev/nvme1n1:/dev/nvme2n1

## Note

You can run spdk/scripts/fio.py to test iSCSI target.
