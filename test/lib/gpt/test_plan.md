# GPT SPDK support Test Plan

# Sytem/Integration testing
- Run GPT commands, checking the command output.

#Test Cases Description

##Test Cases Execution Prerequisites
1. Clone and compile SPDK in a directory;


##Test Case 1: Gpt is NULL

##Test Case 2: GPT buffsize is NULL

##Test Case 3: Signature mismatch

##Test Case 4: start lba mismatch

##Test Case 5: partitions of os_type is not GPT_PROTECTIVE_MBR

##Test Case 6: head_size of GPT < size of (*head) 

##Test Case 7: head_size of GPT > sector_size of GPT

##Test Case 8: head crc32 does not match original_crc

##Test Case 9: signature did not match

##Test Case 10: lba range check error

##Test Case 11: Number of partition entries exceeds max value

##Test Case 12: Entry size of partition is not expected

##Test Case 13: GPT buffsize is not enough

##Test Case 14: GPT partition entry array crc32 did not match
