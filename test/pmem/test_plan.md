# PMEM bdev feature test plan

## Objective
The purpose of these tests is to verify possibility of using pmem bdev
configuration in SPDK by running functional and integration tests.

## Methodology
Configuration in tests is to be done using example stub application
(spdk/example/bdev/io/bdev_io).
All possible management is done using RPC calls with the exception of
use of split bdevs which have to be configured in .conf file.

Functional tests are executed as scenarios - sets of smaller test steps
in which results and return codes of RPC calls are validated.
Some configuration calls may also additionally be validated
by use of "get" (e.g. get_bdevs) RPC calls, which provide additional
information for veryfing results.
In some steps additional write/read operations will be performed on
PMEM bdevs in order to check IO path correct behavior.

Integrity tests will be executed to config correct behavior of PMEM bdev
when working with vhost, nvmf_tgt and iscsi_tgt applications.

## Functional tests

### exists_pmem

#### exists_pmem_tc1
Negative test for checking pmem file.
Call with missing path argument.
Steps & expected results:
- Call exists_pmem with missing path argument
- Check that return code != 0 and error code = ? (TODO: check that later)

#### exists_pmem_tc2
Negative test for checking pmem file.
Call with non-existant path argument.
Steps & expected results:
- Call exists_pmem with path argument that points to not existing file.
- Check that return code != 0 and error code = ENODEV (TODO: check that later)


### create_pmem

#### create_pmem_tc1
Negative test case for creating a new pmem.
Call create_pmem with missing arguments.
Steps & expected results:
- call create_pmem without path argument
- call return code != 0
- call extist_pmem and check that pmem file was not created
- call return code != 0 

#### create_pmem_tc2
Negative test case for creating a new pmem.
Call create_pmem with non existing path argument.
Steps & expected results:
- call create_pmem with path that does not exist
- call return code != 0
- call extist_pmem and check that pmem file was not created
- call return code != 0 

#### create_pmem_tc3
Positive test case for creating a new pmem.
Steps & expected results:
- call create_pmem with correct path argument
- call return code = 0
- call exist_pmem and check that pmem file was created
- call return code = 0
- call delete_pmem on previously created pmem
- return code = 0 and no error code

#### create_pmem_tc4
Negative test case for creating two pmems with same path.
Steps & expected results:
- call create_pmem with correct path argument
- call return code = 0
- call exist_pmem and check that pmem file was created
- call return code = 0
- call create_pmem with the same path argument as before
- call return code != 0, error code = EEXIST (TODO: check)
- call exist_pmem and check that pmem file is still available
- call return code = 0
- call delete_pmem on previously created pmem
- return code =0 and no error code


### delete_pmem

#### delete_pmem_tc1
Negative test case for deleting a pmem.
Call delete_pmem on non-exisiting pmem.
Steps & expected results:
- call delete_pmem on non-existing pmem.
- return code !=0 and error code = ENODEV (TODO: check that)

#### delete_pmem_tc2
Positive test case for creating and deleting a pemem.
Steps & expected results:
- call create_pmem with correct arguments
- return code = 0 and no error code
- using exist_pmem check that pmem was created
- return code = 0 and no error code
- call delete_pmem on previously created pmem
- return code = 0 and no error code
- using exist_pmem check that pmemno longer exists
- return code !=0 and error code = ENODEV (TODO: check that)


### create_pmem_bdev
From libpmemblk documentation:
- PMEM block size has to be bigger than 512 internal blocks; if lower value
is used then PMEM library will silently round it up to 512 which is defined
in nvml/libpmemblk.h file as PMEMBLK_MIN_BLK.
- Total pool size cannot be less than 16MB which is defined i 
nvml/libpmemblk.h file as PMEMBLK_MIN_POOL
- Maximum allowed size of PMEM? Possibly the smallest of available
memory modules on machine (smallest RAM stick) - need to check.

#### create_pmem_bdev_tc1
Negative test for constructing new pmem bdev.
Call create_pmem with missing arguments.
Steps & expected results:
- call create_pmem_bdev with correct path argument
- call exist_pmem and check if pmem file exists
- call create_pmem_bdev with correct path to file but with no block size and total size
- call return code != 0; using get_bdevs verify that no pmem file was created
- call create_pmem_bdev with correct path to file and total size but no block size
- call return code != 0; using get_bdevs verify that no pmem file was created
- call delete_pmem on previously created pmem
- return code = 0 and no error code

#### create_pmem_bdev_tc2
Negative test for constructing new pmem bdev.
Call create_pmem_bdev with not existing path argument.
Steps & expected results:
- call create_pmem_bdev with incorrect (not existing) path and
correct total size and block size
- call return code != 0 and error code: ENODEV (TODO: Check that)
- using get_bdevs check that no pmem bdev was created

#### create_pmem_bdev_tc3
Negative test for constructing new pmem bdev.
Call create_pmem_bdev with total size less than 16MB.
Steps & expected results:
- call create_pmem_bdev with correct path argument
- call exist_pmem and check if pmem file exists
- call create_pmem_bdev with correct, existing path and correct block size (>512)
- use total size for pool less than 16MB
- call return code != 0 and error code: ? (TODO: Check that)
- using get_bdevs check that no pmem bdev was created
- call delete_pmem on previously created pmem
- return code = 0 and no error code

#### create_pmem_bdev_tc4
Positive test for constructing new pmem bdevs with block size smaller than 512.
Steps & expected results:
- call create_pmem with correct path argument
- return code = 0, no errors
- call exist_pmem and check if pmem file exists
- return code = 0, no errors
- call create_pmem_bdev with with correct path and total size,
use block sizes less than 512 (1, 256, 511)
- for each used block size:
  - return code = 0
- using get_bdevs check that all pmem bdevs were created
- from get_bdevs output confirm that created pmem bdevs have
block size rounded up to 512
- delete each pmem bdev using delete_bdev
- return code = 0, no error code
- delete previously created pmems

#### create_pmem_bdev_tc5
Positive test for constructing new pmem bdevs with block size bigger than 512.
Steps & expected results:
- call create_pmem with correct path argument
- return code = 0, no errors
- call exist_pmem and check if pmem file exists
- return code = 0, no errors
- call create_pmem_bdev with with correct path and total size,
use block sizes bigger than 512 (512, 513, 4096, 131072)
- for each used block size:
  - return code = 0
- using get_bdevs check that all pmem bdevs were created
- from get_bdevs output confirm that created pmem bdevs have
block size as requested
- delete each pmem bdev using delete_bdev
- return code = 0, no error code
- delete previously created pmems
- return code = 0, no error code

#### create_pmem_bdev_tc6
Negative test for constructing pmem bdevs twice on the same pmem.
Steps & expected results:
- call create_pmem with correct path argument
- return code = 0, no errors
- call exist_pmem and check if pmem files exists
- return code = 0, no errors
- call create_pmem_bdev with with correct arguments to create a pmem bdev
- return code = 0, no errors
- using get_bdevs check that pmem bdev was created
- call create_pmem_bdev again on the same pmem file; use different
block size and total size
- return code != 0, error code = EEXIST (TODO: check)
- using get_bdevs check that previous pmem bdev was not overwritten
(compare total size and block size info)
- delete each pmem bdev using delete_bdev
- delete previously created pmems
- return code = 0, no error code

### delete_bdev

#### delete_bdev_tc1
Positive test for deleting pmem bdevs using common delete_bdev call.
Steps & expected results:
- construct malloc and aio bdevs (also NVMe if possible)
- all calls - return code = 0, no errors; bdevs created
- call create_pmem with correct path argument
- return code = 0, no errors
- call exist_pmem and check if pmem file exists
- return code = 0, no errors
- call create_pmem_bdev and create a pmem bdev
- return code = 0, no errors
- using get_bdevs check that pmem bdev was created
- delete pmem bdev using delete_bdev
- return code = 0, no errors
- using get_bdevs confirm that pmem bdev was deleted and other bdevs
were unaffected.

#### delete_bdev_tc2
Negative test for deleting pmem bdev twice.
Steps & expected results:
- call create_pmem with correct path argument
- return code = 0, no errors
- call exist_pmem and check if pmem file exists
- return code = 0, no errors
- call create_pmem_bdev and create a pmem bdev
- return code = 0, no errors
- using get_bdevs check that pmem bdev was created
- delete pmem bdev using delete_bdev
- return code = 0, no errors
- using get_bdevs confirm that pmem bdev was deleted
- delete pmem bdev using delete_bdev second time
- return code != 0, error code = ENODEV (TODO: check)


## Integrity tests
To be done