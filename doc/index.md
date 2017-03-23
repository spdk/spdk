# Storage Performance Development Kit {#index}

# Introduction {#intro}

- [SPDK on GitHub](https://github.com/spdk/spdk/)
- [SPDK.io](http://www.spdk.io/)

The Storage Performance Development Kit (SPDK) provides a set of tools and libraries
for writing high performance, scalable, user-mode storage applications.
It achieves high performance by moving all of the necessary drivers
into userspace and operating in a polled mode instead of relying on interrupts,
which avoids kernel context switches and eliminates interrupt handling overhead.

## General Information {#general}

 - @ref directory_structure
 - @ref porting
 - [Public API header files](files.html)

## Modules {#modules}

- @ref event
- @ref nvme
- @ref nvmf
- @ref ioat
- @ref iscsi
- @ref bdev
- @ref blob
- @ref blobfs
- @ref vhost
