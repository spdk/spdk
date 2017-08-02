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
- call successfull, return code = 0, uuid printed to stdout
- get_bdevs: backend used for construct_lvol_store has uuid
  field set with the same uuid as returned from RPC call
- no other operation fails

#### construct_lvs_nonexistent_bdev
Negative test for constructing a new lvol store.
Call construct_lvol_store with base bdev name which does not
exist in configuration.
Steps:
- try construct_lvol_store on bdev which does not exist

Expected result:
- return code != 0
- Error code: ENODEV ("No such device") response printed to stdout

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
- EEXIST response printed to stdout
- no other operation fails

### construct_lvol_bdev

#### construct_logical_volume_positive
Positive test for constructing a new logical volume.
Call construct_lvol_bdev with correct lvol store UUID and size in MiB for this bdev.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev
- construct_lvol_bdev on correct lvs_uuid and size
- delete malloc bdev

Expected result:
- call successfull, return code = 0
- get_bdevs: backend used for construct_lvol_bdev has name
  field set with the same name as returned value from call RPC method: construct_lvol_bdev
- no other operation fails

#### construct_multi_logical_volumes_positive
Positive test for constructing a multi logical volumes.
Call construct_lvol_bdev with correct lvol store UUID and
size is equal one quarter of the this bdev size.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev
- construct_lvol_bdev on correct lvs_uuid and size
  (size is equal one quarter of the bdev size)
- repeat the previous step three times
- delete malloc bdev

Expected result:
- call successfull, return code = 0
- get_bdevs: backend used for construct_lvol_bdev has name
  field set with the same name as returned from RPC call for all repeat
- no other operation fails

#### construct_logical_volume_nonexistent_lvs_uuid
Negative test for constructing a new logical_volume.
Call construct_lvol_bdevs with lvs_uuid which does not
exist in configuration.
Steps:
- try to call construct_lvol_bdev with lvs_uuid which does not exist

Expected result:
- return code != 0
- ENODEV response printed to stdout

#### construct_logical_volumes_on_busy_bdev
Negative test for constructing a new logical volume.
Call construct_lvol_bdev on a busy malloc bdev.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- construct_lvol_bdev on correct lvs_uuid and size is equal of size malloc bdev
- construct_lvol_bdev on the same lvs_uuid as in last step
- destroy_lvol_store
- delete malloc bdev

Expected result:
- first call successful
- second call return code != 0
- Error code response printed to stdout
- no other operation fails

### resize_lvol_store

#### resize_logical_volume_positive
Positive test for resizing a logical_volume.
Call resize_lvol_bdev with correct logical_volumes name and new size.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- construct_lvol_bdev on correct lvs_uuid and size is
  equal one quarter of size malloc bdev
- check size of the lvol bdev
- resize_lvol_bdev on correct lvs_uuid and size is
  equal half of size malloc bdev
- check size of the lvol bdev by command RPC : get_bdevs
- resize_lvol_bdev on the correct lvs_uuid and size is
  equal full of size malloc bdev
- check size of the lvol bdev by command RPC : get_bdevs
- resize_lvol_bdev on the correct lvs_uuid and size is equal 0 MiB
- check size of the lvol bdev by command RPC : get_bdevs
- destroy_lvol_store
- delete malloc bdev

Expected result:
- lvol bdev should change size after resize operations
- calls successfull, return code = 0
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
- Error code: ENODEV ("No such device") response printed to stdout

#### resize_logical_volume_with_size_out_of_range
Negative test for constructing a new lvol store.
Call construct_lvol_store with base bdev name twice.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- construct_lvol_bdev on correct lvs_uuid and
  size is equal one quarter of size malloc bdev
- resize_lvol_bdev on correct lvs_uuid and size is
  equal of size malloc bdev + 1MiB
- destroy_lvol_store
- delete malloc bdev

Expected result:
- fourth call return code != 0 for all cases
- Error code response printed to stdout
- no other operation fails

### destroy_lvol_store

#### destroy_lvol_store_positive
Positive test for destroying a logical volume store.
Call destroy_lvol_store with correct logical_volumes name
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- construct_lvol_bdev on correct lvs_uuid and size is equal of size malloc bdev
- destroy_lvol_store 
- delete malloc bdev

Expected result:
- calls successfull, return code = 0
- get_bdevs: no change
- no other operation fails

### destroy_lvol_store in multiple logical volumes

Positive test for destroying singe lvol store with a multiple logical volumes.
Call construct_lvol_bdev with correct lvol store UUID and size is equal
one quarter of the this bdev size.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev
- construct_lvol_bdev on correct lvs_uuid and size
  (size is equal one quarter of the bdev size)
- repeat the previous step three times
- destroy one lvol store
Expected result:
- calls successfull, return code = 0
- get_bdevs: no change
- no other operation fails

####  destroy_lvol_store_nonexistent_lvs_uuid
Call destroy_lvol_store with nonexistent logical_volumes name
exist in configuration.
Steps:
- try to call destroy_lvol_store with lvs_uuid which does not exist

Expected result:
- return code != 0
- Error code response printed to stdout

####  destroy_lvol_store_nonexistent_bdev
Call destroy_lvol_store with nonexistent bdevs
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- delete malloc bdev
- destroy_lvol_store
Expected result:
- Error code response printed to stdout
- no other operation fails

### nested construct_lvol_store

#### nasted construct_logical_volume_positive
Positive test for constructing a nasted new lvol store.
Call construct_lvol_store with correct base bdev name.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- construct_lvol_bdev on correct lvs_uuid and size is
  equal of size malloc bdev
- check size of the lvol bdev 
- construct_lvol_store on created lvol_bdev
- check size of the lvol bdev by command RPC : get_bdevs
- destroy all lvol_store
- delete malloc bdev

Expected result:
- calls successfull, return code = 0
- get_bdevs: backend used for construct_lvol_store has UUID
  field set with the same UUID as returned from RPC call
  backend used for construct_lvol_bdev has UUID
  field set with the same UUID as returned from RPC call
- no other operation fails

#### nasted construct_logical_volume_nonexistent
Negative test for constructing lvol store with a nonexistent lvol bdevs.
Call construct_lvol_store with base bdev name which does not
exist in configuration.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- construct_lvol_bdev on correct lvs_uuid and size is
  equal of size malloc bdev
- check size of the lvol bdev 
- construct_lvol_store on nonexistent lvol_bdev
- destroy all lvol_store 
- delete malloc bdev

Expected result:
- return code != 0
- Error code: ENODEV ("No such device") response printed to stdout

### nested destroy_busy_lvol_store

#### nasted construct_logical_volume_positive
Negative test for destroying a nasted first lvol store.
Call destroy_lvol_store with correct base bdev name.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- construct_lvol_bdev on correct lvs_uuid and size is
  equal of size malloc bdev
- construct_first_nested_lvol_store on created lvol_bdev
- construct_first_nested_lvol_bdev on correct lvs_uuid and size
- construct_second_nested_lvol_store on created first_nested_lvol_bdev
- construct_second_nested_lvol_bdev on correct first_nested_lvs_uuid and size
- destroy first nested lvol_store
- delete malloc bdev

Expected result:
-Expected result:
- Error code response printed to stdout
- no other operation fails

#### SIGTERM
Call CTRL+C (SIGTERM) occurs after creating lvol store

Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- Send SIGTERM signal to the application

Expected result:
- calls successfull, return code = 0
- get_bdevs: no change
- no other operation fails

