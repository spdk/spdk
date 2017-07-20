# SPDK BDEV Test Plan

## Functionality Test

### Test tool/bdev_io
- blockdev_write_read_4k
- blockdev_write_read_512Bytes
- blockdev_write_read_size_gt_128k
- blockdev_write_read_invalid_size
- blockdev_write_read_offset_plus_nbytes_equals_bdev_size
- blockdev_write_read_offset_plus_nbytes_gt_bdev_size
- blockdev_write_read_max_offset
- blockdev_overlapped_write_read_8k
- blockdev_writev_readv_4k
- blockdev_writev_readv_30x4k
- blockdev_writev_readv_512Bytes
- blockdev_writev_readv_size_gt_128k
- blockdev_writev_readv_size_gt_128k_two_iov
- blockdev_test_reset

## Gpt Test
- part device by gpt

## Performance Test

### Test tool/bdevperf
- queue depth  32 size 4096 write verify 5s
- queue depth 128 size 4096/65536 write verify 60s
- queue depth 128 size 4096/65536 read 60s
- queue depth 128 size 4096/65536 unmap 60s

## Error simulation

### Test tool/Rpc error injection
- run mkfs and inject read/write/reset errors
- run fio over iscsi and inject read/write/reset errors
