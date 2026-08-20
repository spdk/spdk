Added `blkdiscard` option to the `bdev_aio_create` RPC, exposed as `--blkdiscard` in the CLI. It
enables UNMAP support using the `BLKDISCARD` ioctl instead of `fallocate`, so the discard is issued
to the underlying block device. It can be combined with `fallocate`, which then only services
WRITE ZEROES. The option requires `filename` to be a block device and is mutually exclusive with
`readonly`.
