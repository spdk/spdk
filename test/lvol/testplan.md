# Lvol feature test plan

## Objective
The purpose of these tests is to verify the possibility of using lvol configuration in SPDK.  

## Methodology
Configuration in test is to be done using example stub application.
All management is done using RPC calls, including logical volumes management.
All tests are performed using malloc backends.
One exception to malloc backends is the last test, which is for logical volume
tasting - this one requires NVMe backend.

Tests will be executed as scenarios - sets of smaller test step 
in which return codes from RPC calls is validated. 
Some configuration calls may also be validated by use of
"get_*" RPC calls, which provide additional information for verifying
results.

## Tests

### construct_lvol_store

#### construct_lvs_positive
Positive test for constructing a new lvol store.
Call construct_lvol_store with correct base bdev name.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev
- delete malloc bdev

Expected result:
- call successfull, return code = 0, GUID printed to stdout
- get_bdevs: backend used for construct_lvol_store has GUID
field set with the same GUID as returned from RPC call
- no other operation fails

#### construct_lvs_nonexistent_bdev
Negative test for constructing a new lvol store.
Call construct_lvol_store with base bdev name which does not
exist in configuration.
Steps:
- try construct_lvol_store on bdev which does not exist

Expected result:
- return code != 0
- error response printed to stdout

#### construct_lvs_on_bdev_twice
Negative test for constructing a new lvol store.
Call construct_lvol_store with base bdev name twice.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- construct_lvol_store on the same bdev as in last step
- destroy lvs
- delete malloc bdev

Expected result:
- first call successful
- second call return code != 0
- error response printed to stdout
- no other operation fails

### construct_lvol_bdev

#### construct_logical_volume_positive
Positive test for constructing a new logical volume.
Call construct_lvol_bdev with correct lvol store UUID and size in MiB for this bdev.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev
- construct_lvol_bdev on correct lvs_guid and size
- delete malloc bdev

Expected result:
- call successfull, return code = 0
- get_bdevs: backend used for construct_lvol_bdev has name
field set with the same name as returned from RPC call
- no other operation fails

#### construct_multi_logical_volumes_positive
Positive test for constructing a multi logical volumes.
Call construct_lvol_bdev with correct lvol store UUID and size is equal one quarter of the this bdev size.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev
- construct_lvol_bdev on correct lvs_guid and size (size is equal one quarter of the bdev size)
- Repeat the last step three times 
- delete malloc bdev

Expected result:
- call successfull, return code = 0
- get_bdevs: backend used for construct_lvol_bdev has name
field set with the same name as returned from RPC call for all repeat
- no other operation fails

#### construct_logical_volume_nonexistent_lvs_guid
Negative test for constructing a new logical_volume.
Call construct_lvol_bdevs with lvs_guid which does not
exist in configuration.
Steps:
- try construct_lvol_bdev on lvs_guid which does not exist

Expected result:
- return code != 0
- error response printed to stdout

#### construct_logical_volumes_on_busy_bdev
Negative test for constructing a new logical volume.
Call construct_lvol_bdev with name logical volume and size in MiB.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- construct_lvol_bdev on correct lvs_guid and size is equal of size malloc bdev
- construct_lvol_bdev on the same lvs_guid as in last step
- destroy lvs
- delete malloc bdev

Expected result:
- first call successful
- second call return code != 0
- error response printed to stdout
- no other operation fails

### resize_lvol_store

#### resize_logical_volume_positive
Positive test for resize a logical_volume.
Call resize_lvol_bdev with correct logical_volumes name and new size.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- construct_lvol_bdev on correct lvs_guid and size is equal one quarter of size malloc bdev
- resize_lvol_bdev on correct lvs_guid and size is equal half of size malloc bdev
- resize_lvol_bdev on the correct lvs_guid and size is equal full of size malloc bdev
- resize_lvol_bdev on the correct lvs_guid and size is equal 0 MiB
- destroy lvs
- delete malloc bdev

Expected result:
- call successfull, return code = 0
- get_bdevs: no change
- no other operation fails

#### resize_logical_volume_nonexistent_logical_volume.
Negative test for resizing a logical_volume.
Call resize_lvol_bdev with logical volume which does not
exist in configuration.
Steps:
- try resize_lvol_store on logical volume which does not exist

Expected result:
- return code != 0
- error response printed to stdout

#### resize_logical_volume_with_size_out_of_range
Negative test for constructing a new lvol store.
Call construct_lvol_store with base bdev name twice.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- construct_lvol_bdev on correct lvs_guid and size is equal one quarter of size malloc bdev
- resize_lvol_bdev on correct lvs_guid and size is equal of size malloc bdev + 1MiB,10MiB,100MiB,1000MiB
- destroy lvs
- delete malloc bdev

Expected result:
- fourth call return code != 0 for all cases
- error response printed to stdout
- no other operation fails

