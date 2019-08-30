# PMEM bdev feature test plan

## Objective
The purpose of these tests is to verify possibility of using pmem bdev
configuration in SPDK by running functional tests FIO traffic verification
tests.

## Configuration
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

FIO traffic verification tests will serve as integration tests and will
be executed to config correct behavior of PMEM bdev when working with vhost,
nvmf_tgt and iscsi_tgt applications.

## Functional tests

### bdev_pmem_get_pool_info

#### bdev_pmem_get_pool_info_tc1
Negative test for checking pmem pool file.
Call with missing path argument.
Steps & expected results:
- Call bdev_pmem_get_pool_info with missing path argument
- Check that return code != 0 and error code =

#### bdev_pmem_get_pool_info_tc2
Negative test for checking pmem pool file.
Call with non-existant path argument.
Steps & expected results:
- Call bdev_pmem_get_pool_info with path argument that points to not existing file.
- Check that return code != 0 and error code = ENODEV

#### bdev_pmem_get_pool_info_tc3
Negative test for checking pmem pool file.
Call with other type of pmem pool file.
Steps & expected results:
- Using pmem utility tools create pool of OBJ type instead of BLK
(if needed utility tools are not available - create random file in filesystem)
- Call bdev_pmem_get_pool_info and point to file created in previous step.
- Check that return code != 0 and error code = ENODEV

#### bdev_pmem_get_pool_info_tc4
Positive test for checking pmem pool file.
Call with existing pmem pool file.
Steps & expected results:
- Call bdev_pmem_get_pool_info with path argument that points to existing file.
- Check that return code == 0

### bdev_pmem_create_pool
From libpmemblk documentation:
- PMEM block size has to be bigger than 512 internal blocks; if lower value
is used then PMEM library will silently round it up to 512 which is defined
in pmem/libpmemblk.h file as PMEMBLK_MIN_BLK.
- Total pool size cannot be less than 16MB which is defined i
pmem/libpmemblk.h file as PMEMBLK_MIN_POOL
- Total number of segments in PMEP pool file cannot be less than 256

#### bdev_pmem_create_pool_tc1
Negative test case for creating a new pmem.
Call bdev_pmem_create_pool with missing arguments.
Steps & expected results:
- call bdev_pmem_create_pool without path argument
- call return code != 0
- call bdev_pmem_get_pool_info and check that pmem pool file was not created
- call return code != 0
- call bdev_pmem_create_pool with path but without size and block size arguments
- call return code != 0
- call bdev_pmem_get_pool_info and check that pmem pool file was not created
- call return code != 0
- call bdev_pmem_create_pool with path and size but without block size arguments
- call return code != 0
- call bdev_pmem_get_pool_info and check that pmem pool file was not created
- call return code != 0

#### bdev_pmem_create_pool_tc2
Negative test case for creating a new pmem.
Call bdev_pmem_create_pool with non existing path argument.
Steps & expected results:
- call bdev_pmem_create_pool with path that does not exist
- call return code != 0
- call bdev_pmem_get_pool_info and check that pmem pool file was not created
- call return code != 0

#### bdev_pmem_create_pool_tc3
Positive test case for creating a new pmem pool on disk space.
Steps & expected results:
- call bdev_pmem_create_pool with correct path argument,
blocksize=512 and total size=256MB
- call return code = 0
- call bdev_pmem_get_pool_info and check that pmem file was created
- call return code = 0
- call delete_pmem_pool on previously created pmem
- return code = 0 and no error code

#### bdev_pmem_create_pool_tc4
Positive test case for creating a new pmem pool in RAM space.
# TODO: Research test steps for creating a pool in RAM!!!
Steps & expected results:
- call bdev_pmem_create_pool with correct path argument,
blocksize=512 and total size=256MB
- call return code = 0
- call bdev_pmem_get_pool_info and check that pmem file was created
- call return code = 0
- call delete_pmem_pool on previously created pmem
- return code = 0 and no error code

#### bdev_pmem_create_pool_tc5
Negative test case for creating two pmems with same path.
Steps & expected results:
- call bdev_pmem_create_pool with correct path argument,
blocksize=512 and total size=256MB
- call return code = 0
- call bdev_pmem_get_pool_info and check that pmem file was created
- call return code = 0
- call bdev_pmem_create_pool with the same path argument as before,
blocksize=4096 and total size=512MB
- call return code != 0, error code = EEXIST
- call bdev_pmem_create_pool and check that first pmem pool file is still
available and not modified (block size and total size stay the same)
- call return code = 0
- call delete_pmem_pool on first created pmem pool
- return code =0 and no error code

#### bdev_pmem_create_pool_tc6
Positive test case for creating pmem pool file with various block sizes.
Steps & expected results:
- call bdev_pmem_create_pool with correct path argument, total size=256MB
with different block size arguments - 1, 511, 512, 513, 1024, 4096, 128k and 256k
- call bdev_pmem_get_pool_info on each of created pmem pool and check if it was created;
For pool files created with block size <512 their block size should be rounded up
to 512; other pool files should have the same block size as specified in create
command
- call return code = 0; block sizes as expected
- call delete_pmem_pool on all created pool files

#### bdev_pmem_create_pool_tc7
Negative test case for creating pmem pool file with total size of less than 16MB.
Steps & expected results:
- call bdev_pmem_create_pool with correct path argument, block size=512 and
total size less than 16MB
- return code !=0 and error code !=0
- call bdev_pmem_get_pool_info to verify pmem pool file was not created
- return code = 0

#### bdev_pmem_create_pool_tc8
Negative test case for creating pmem pool file with less than 256 blocks.
Steps & expected results:
- call bdev_pmem_create_pool with correct path argument, block size=128k and
total size=30MB
- return code !=0 and error code !=0
- call bdev_pmem_get_pool_info to verify pmem pool file was not created
- return code = 0

### delete_pmem_pool

#### delete_pmem_pool_tc1
Negative test case for deleting a pmem.
Call delete_pmem_pool on non-exisiting pmem.
Steps & expected results:
- call delete_pmem_pool on non-existing pmem.
- return code !=0 and error code = ENOENT

#### delete_pmem_pool_tc2
Negative test case for deleting a pmem.
Call delete_pmem_pool on a file of wrong type
Steps & expected results:
- Using pmem utility tools create pool of OBJ type instead of BLK
(if needed utility tools are not available - create random file in filesystem)
- Call delete_pmem_pool and point to file created in previous step.
- return code !=0 and error code = ENOTBLK

#### delete_pmem_pool_tc3
Positive test case for creating and deleting a pemem.
Steps & expected results:
- call bdev_pmem_create_pool with correct arguments
- return code = 0 and no error code
- using bdev_pmem_get_pool_info check that pmem was created
- return code = 0 and no error code
- call delete_pmem_pool on previously created pmem
- return code = 0 and no error code
- using bdev_pmem_get_pool_info check that pmem no longer exists
- return code !=0 and error code = ENODEV

#### delete_pmem_pool_tc4
Negative test case for creating and deleting a pemem.
Steps & expected results:
- run scenario from test case 3
- call delete_pmem_pool on already deleted pmem pool
- return code !=0 and error code = ENODEV

### bdev_pmem_create

#### bdev_pmem_create_tc1
Negative test for constructing new pmem bdev.
Call bdev_pmem_create with missing argument.
Steps & expected results:
- Call bdev_pmem_create with missing path argument.
- Check that return code != 0

#### bdev_pmem_create_tc2
Negative test for constructing new pmem bdev.
Call bdev_pmem_create with not existing path argument.
Steps & expected results:
- call bdev_pmem_create with incorrect (not existing) path
- call return code != 0 and error code = ENODEV
- using get_bdevs check that no pmem bdev was created

#### bdev_pmem_create_tc3
Negative test for constructing pmem bdevs with random file instead of pmemblk pool.
Steps & expected results:
- using a system tool (like dd) create a random file
- call bdev_pmem_create with path pointing to that file
- return code != 0, error code = ENOTBLK

#### bdev_pmem_create_tc4
Negative test for constructing pmem bdevs with pmemobj instead of pmemblk pool.
Steps & expected results:
- Using pmem utility tools create pool of OBJ type instead of BLK
(if needed utility tools are not available - create random file in filesystem)
- call bdev_pmem_create with path pointing to that pool
- return code != 0, error code = ENOTBLK

#### bdev_pmem_create_tc5
Positive test for constructing pmem bdev.
Steps & expected results:
- call bdev_pmem_create_pool with correct arguments
- return code = 0, no errors
- call bdev_pmem_get_pool_info and check if pmem files exists
- return code = 0, no errors
- call bdev_pmem_create with with correct arguments to create a pmem bdev
- return code = 0, no errors
- using get_bdevs check that pmem bdev was created
- delete pmem bdev using bdev_pmem_delete
- return code = 0, no error code
- delete previously created pmem pool
- return code = 0, no error code

#### bdev_pmem_create_tc6
Negative test for constructing pmem bdevs twice on the same pmem.
Steps & expected results:
- call bdev_pmem_create_pool with correct arguments
- return code = 0, no errors
- call bdev_pmem_get_pool_info and check if pmem files exists
- return code = 0, no errors
- call bdev_pmem_create with with correct arguments to create a pmem bdev
- return code = 0, no errors
- using get_bdevs check that pmem bdev was created
- call bdev_pmem_create again on the same pmem file
- return code != 0, error code = EEXIST
- delete pmem bdev using bdev_pmem_delete
- return code = 0, no error code
- delete previously created pmem pool
- return code = 0, no error code

### bdev_pmem_delete

#### delete_bdev_tc1
Positive test for deleting pmem bdevs using bdev_pmem_delete call.
Steps & expected results:
- construct malloc and aio bdevs (also NVMe if possible)
- all calls - return code = 0, no errors; bdevs created
- call bdev_pmem_create_pool with correct path argument,
block size=512, total size=256M
- return code = 0, no errors
- call bdev_pmem_get_pool_info and check if pmem file exists
- return code = 0, no errors
- call bdev_pmem_create and create a pmem bdev
- return code = 0, no errors
- using get_bdevs check that pmem bdev was created
- delete pmem bdev using bdev_pmem_delete
- return code = 0, no errors
- using get_bdevs confirm that pmem bdev was deleted and other bdevs
were unaffected.

#### bdev_pmem_delete_tc2
Negative test for deleting pmem bdev twice.
Steps & expected results:
- call bdev_pmem_create_pool with correct path argument,
block size=512, total size=256M
- return code = 0, no errors
- call bdev_pmem_get_pool_info and check if pmem file exists
- return code = 0, no errors
- call bdev_pmem_create and create a pmem bdev
- return code = 0, no errors
- using get_bdevs check that pmem bdev was created
- delete pmem bdev using bdev_pmem_delete
- return code = 0, no errors
- using get_bdevs confirm that pmem bdev was deleted
- delete pmem bdev using bdev_pmem_delete second time
- return code != 0, error code = ENODEV


## Integration tests
Description of integration tests which run FIO verification traffic against
pmem_bdevs used in vhost, iscsi_tgt and nvmf_tgt applications can be found in
test directories for these components:
- spdk/test/vhost
- spdk/test/nvmf
- spdk/test/iscsi_tgt
