# GPT SPDK support Test Plan

# Integration test
#1 bdevperf
- Set nvme disk partition to gpt partition, such as Nvme0n1p1,Nvme0n1p2
- Run test/lib/bdev/blockdev.sh

#2 iSCSI target (use the GPT bdev to conduct fio test)
- Set nvme disk partition to gpt partition, such as Nvme0n1p1,Nvme0n1p2
- Set IP is 192.168.1.11
- Start the iscsi_tgt process with iscsi.conf.in
- iscsiadm login
- Run scripts/fio.py
    - FIO IO depth: 1, 16, 64
    - FIO Blocksize: 512, 4k, 256k
    - FIO RW modes: read, randread, write, randwrite, rw, randrw
    - each test job set 10 seconds

#3 NVMf target (use the GPT bdev to export related subsystem for test)
- Set nvme disk partition to gpt partition, such as Nvme0n1p1,Nvme0n1p2
- Load the drivers(mlx5_ib,nvme-rdma,nvme-fabrics)
- Set IP is 192.168.3.11
- Start the nvmf_tgt process with nvmf.conf
- nvme connect rdma
- Run test/nvmf/fio/fio.sh
    - FIO IO depth: 1, 16, 64
    - FIO Blocksize: 512, 4k, 256k
    - FIO RW modes: read, randread, write, randwrite, rw, randrw
    - each test job set 10 seconds

##Test Cases Execution Prerequisites
# Clone and compile GPT SPDK in a directory;

#Unit test: test the function of spdk_gpt_parse with the following case.

##Test Case 1: GPT structure is NULL
- Set gpt is NULL

##Test Case 2: GPT buffsize is NULL
- Set gpt->buf is NULL

##Test Case 3: check_mbr failed
- Set *gpt is "aaa..."

##Test Case 4: read_header failed
- Set check_mbr passed

##Test Case 5: read_partitions failed
- Set read_header passed

##Test Case 6: all passed
- Set read_partitions passed

##Test Case 7: Signature mismatch
- Set *gpt is "aaa..."

##Test Case 8: start lba mismatch
- Set mbr_signature is 0xAA55 matched

##Test Case 9: start lba matched, os_type mismatch
- Set partitions[0].start is 1, lba matched

##Test Case 10: os_type matched, size_lba mismatch
- Set partitions[0].os_type is 0xEE matched

##Test Case 11: size_lba matched
- Set partitions[0].size_lba is 0xFFFFFFFF, 

##Test Case 12: head_size of GPT > sector_size of GPT
- Set header_size is 600, sector_size is 512

##Test Case 13: head crc32 does not match original_crc
- Set header_crc32 is 0x22D18C80

##Test Case 14: signature did not match
- Set header_crc32 matched

##Test Case 15: lba range check error
- Set signature matched, usable_lba mismatch

##Test Case 16: usable_lba matched
- Set first_usable_lba, last_usable_lba

##Test Case 17: Number of partition entries exceeds max value
- Set num_partition_entries is 256

##Test Case 18: Entry size of partition is not expected
- Set num_partition_entries is 64 matched

##Test Case 19: GPT buffsize is not enough
- Set partition_entry_lba mismatch

##Test Case 20: GPT partition entry array crc32 did not match
- Set partition_entry_lba is 32 matched

##Test Case 21: GPT partition entry array crc32 matched
- Set partition_entry_array_crc32 is 0xebee44fb
