# Lvol feature test plan

## Objective
The purpose of these tests is to verify the possibility of using lvol configuration in SPDK.

## Methodology
Configuration in test is to be done using example stub application.
All management is done using RPC calls, including logical volumes management.
All tests are performed using malloc backends.
One exception to malloc backends is are the tests for logical volume
tasting - these require persistent merory like NVMe backend.

Tests will be executed as scenarios - sets of smaller test step
in which return codes from RPC calls is validated.
Some configuration calls may also be validated by use of
"get_*" RPC calls, which provide additional information for verifying
results.

## Tests

### construct_lvol_store  - positive tests

#### TEST CASE 1 - Name: construct_lvs_positive
Positive test for constructing a new lvol store.
Call construct_lvol_store with correct base bdev name.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev
- check correct uuid values in response get_lvol_stores command
- destroy lvol store
- delete malloc bdev

Expected result:
- call successful, return code = 0, uuid printed to stdout
- get_lvol_stores: backend used for construct_lvol_store has uuid
  field set with the same uuid as returned from RPC call
- no other operation fails

### construct_lvol_bdev - positive tests

#### TEST CASE 2 - Name: construct_logical_volume_positive
Positive test for constructing a new logical volume.
Call construct_lvol_bdev with correct lvol store UUID and size in MiB for this bdev.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev
- check correct uuid values in response get_lvol_stores command
- construct_lvol_bdev on correct lvs_uuid and size
- delete lvol bdev
- destroy lvol store
- delete malloc bdev

Expected result:
- call successful, return code = 0
- get_bdevs: backend used for construct_lvol_bdev has name
  field set with the same name as returned value from call RPC method: construct_lvol_bdev
- no other operation fails

#### TEST CASE 3 - Name: construct_multi_logical_volumes_positive
Positive test for constructing a multi logical volumes.
Call construct_lvol_bdev with correct lvol store UUID and
size is equal one quarter of the this bdev size.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev
- check correct uuid values in response get_lvol_stores command
- construct_lvol_bdev on correct lvs_uuid and size
  (size is approximately equal to one quarter of the bdev size,
  because of lvol metadata)
- repeat the previous step three more times
- delete lvol bdevs
- destroy lvol store
- delete malloc bdev

Expected result:
- call successful, return code = 0
- get_lvol_store: backend used for construct_lvol_bdev has name
  field set with the same name as returned from RPC call for all repeat
- no other operation fails

#### TEST CASE 4 - Name: construct_lvol_bdev_using_name_positive
Positive test for constructing a logical volume using friendly names.
Verify that logical volumes can be created by using a friendly name
instead of uuid when referencing to lvol store.
Steps:
- create malloc bdev
- create logical volume store on created malloc bdev
- verify lvol store was created correctly
- create logical volume on lvol store by using a friendly name
as a reference
- verify logical volume was correctly created
- delete logical volue bdev
- destroy logical volume store
- delete malloc bdev

Expected result:
- calls successful, return code = 0
- no other operation fails

#### TEST CASE 5 - Name: construct_lvol_bdev_duplicate_names_positive
Positive test for constructing a logical volumes using friendly names.
Verify that logical volumes can use the same argument for friendly names
if they are created on separate logical volume stores.
Steps:
- create two malloc bdevs
- create logical volume stores on created malloc bdevs
- verify stores were created correctly
- create logical volume on first lvol store
- verify it was correctly created
- using the same friendly name argument create logical volume on second
lvol store
- verify logical volume was correctly created
- delete logical volue bdevs
- destroy logical volume stores
- delete malloc bdevs

Expected result:
- calls successful, return code = 0
- no other operation fails

### resize_lvol_store - positive tests

#### TEST CASE 6 - Name: resize_logical_volume_positive
Positive test for resizing a logical_volume.
Call resize_lvol_bdev with correct logical_volumes name and new size.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response get_lvol_stores command
- construct_lvol_bdev on correct lvs_uuid and size is
  equal to one quarter of size malloc bdev
- check size of the lvol bdev by command RPC : get_bdevs
- resize_lvol_bdev on correct lvs_uuid and size is
  equal half to size malloc bdev
- check size of the lvol bdev by command RPC : get_bdevs
- resize_lvol_bdev on the correct lvs_uuid and size is smaller by 1 MB
  from the full size malloc bdev
- check size of the lvol bdev by command RPC : get_bdevs
- resize_lvol_bdev on the correct lvs_uuid and size is equal 0 MiB
- check size of the lvol bdev by command RPC : get_bdevs
- delete lvol bdev
- destroy lvol store
- delete malloc bdev

Expected result:
- lvol bdev should change size after resize operations
- calls successful, return code = 0
- no other operation fails

### destroy_lvol_store - positive tests

#### TEST CASE 7 - Name: destroy_lvol_store_positive
Positive test for destroying a logical volume store.
Call destroy_lvol_store with correct logical_volumes name
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response get_lvol_stores command
- destroy_lvol_store
- check correct response get_lvol_stores command
- delete malloc bdev

Expected result:
- calls successful, return code = 0
- get_lvol_stores: response should be of no value after destroyed lvol store
- no other operation fails

#### TEST CASE 8 - Name: destroy_lvol_store_use_name_positive
Positive test for destroying a logical volume store using
lvol store name instead of uuid for reference.
Call destroy_lvol_store with correct logical_volumes name
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response get_lvol_stores command
- destroy_lvol_store
- check correct response get_lvol_stores command
- delete malloc bdev

Expected result:
- calls successful, return code = 0
- get_lvol_stores: response should be of no value after destroyed lvol store
- no other operation fails

#### TEST CASE 9 - Name: destroy_lvol_store_with_lvol_bdev_positive
Positive test for destroying a logical volume store with lvol bdev
created on top.
Call destroy_lvol_store with correct logical_volumes name
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response get_lvol_stores command
- construct_lvol_bdev on correct lvs_uuid and size is equal to size malloc bdev
- destroy_lvol_store
- check correct response get_lvol_stores command
- delete malloc bdev

Expected result:
- calls successful, return code = 0
- get_lvol_stores: response should be of no value after destroyed lvol store
- no other operation fails

#### TEST CASE 10 - Name: destroy_multi_logical_volumes_positive
Positive test for destroying a logical volume store with multiple lvol
bdevs created on top.
Call construct_lvol_bdev with correct lvol store UUID and
size is equal to one quarter of the this bdev size.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev
- check correct uuid values in response get_lvol_stores command
- construct_lvol_bdev on correct lvs_uuid and size
  (size is equal to one quarter of the bdev size)
- repeat the previous step four times
- destroy_lvol_store
- check correct response get_lvol_stores command
- delete malloc bdev

Expected result:
- call successful, return code = 0
- get_lvol_store: backend used for construct_lvol_bdev has name
  field set with the same name as returned from RPC call for all repeat
- no other operation fails

### nested_construct_logical_volume - positive tests

#### TEST CASE 11 - Name: nested_construct_logical_volume_positive
Positive test for constructing a nested new lvol store.
Call construct_lvol_store with correct base bdev name.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response get_lvol_stores command
- construct_lvol_bdev on correct lvs_uuid and size is
  equal to size malloc bdev
- construct first nested lvol store on created lvol_bdev
- check correct uuid values in response get_lvol_stores command
- construct first nested lvol bdev on correct lvs_uuid and size
- construct second nested lvol store on created first nested lvol bdev
- check correct uuid values in response get_lvol_stores command
- construct second nested lvol bdev on correct first nested lvs uuid and size
- delete nested lvol bdev and lvol store
- delete base lvol bdev and lvol store
- delete malloc bdev

Expected result:
- calls successful, return code = 0
- get_lvol_stores: backend used for construct_lvol_store has UUID
  field set with the same UUID as returned from RPC call
  backend used for construct_lvol_bdev has UUID
  field set with the same UUID as returned from RPC call
- no other operation fails

### destroy_lvol_store - positive tests

#### TEST CASE 12 - Name: destroy_resize_logical_volume_positive
Positive test for destroying a logical_volume after resizing.
Call destroy_lvol_store with correct logical_volumes name.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response get_lvol_stores command
- construct_lvol_bdev on correct lvs_uuid and size is
  equal to one quarter of size malloc bdev
- check size of the lvol bdev
- resize_lvol_bdev on correct lvs_uuid and size is
  equal half of size malloc bdev
- check size of the lvol bdev by command RPC : get_bdevs
- Resize_lvol_bdev on the correct lvs_uuid and the size is smaller by 1 MB
  from the full size malloc bdev
- check size of the lvol bdev by command RPC : get_bdevs
- resize_lvol_bdev on the correct lvs_uuid and size is equal 0 MiB
- check size of the lvol bdev by command RPC : get_bdevs
- destroy_lvol_store
- delete malloc bdev

Expected result:
- lvol bdev should change size after resize operations
- calls successful, return code = 0
- no other operation fails
- get_lvol_stores: response should be of no value after destroyed lvol store

### construct_lvol_store - negative tests

#### TEST CASE 13 - Name: construct_lvs_nonexistent_bdev
Negative test for constructing a new lvol store.
Call construct_lvol_store with base bdev name which does not
exist in configuration.
Steps:
- try construct_lvol_store on bdev which does not exist

Expected result:
- return code != 0
- Error code: ENODEV ("No such device") response printed to stdout

#### TEST CASE 14 - Name: construct_lvs_on_bdev_twice
Negative test for constructing a new lvol store.
Call construct_lvol_store with base bdev name twice.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response get_lvol_stores command
- try construct_lvol_store on the same bdev as in last step;
  this call should fail as base bdev is already claimed by lvol store
- destroy lvs
- delete malloc bdev

Expected result:
- first call successful
- second construct_lvol_store call return code != 0
- EEXIST response printed to stdout
- no other operation fails

#### TEST CASE 15 - Name: construct_lvs_name_twice
Negative test for constructing a new lvol store using the same
friendly name twice.
Steps:
- create two malloc bdevs
- create logical volume store on first malloc bdev
- using get_lvol_stores verify that logical volume store was correctly created
and has arguments as provided in step earlier (cluster size, friendly name, base bdev)
- try to create another logical volume store on second malloc bdev using the
same friendly name as before; this step is expected to fail as stores cannot have
the same name
- delete existing lvol store
- delete malloc bdevs

Expected results:
- creating two logical volume stores with the same friendly name should
not be possible
- no other operation fails

### construct_lvol_bdev - negative tests

#### TEST CASE 16 - Name: construct_logical_volume_nonexistent_lvs_uuid
Negative test for constructing a new logical_volume.
Call construct_lvol_bdev with lvs_uuid which does not
exist in configuration.
Steps:
- try to call construct_lvol_bdev with lvs_uuid which does not exist

Expected result:
- return code != 0
- ENODEV response printed to stdout

#### TEST CASE 17 - Name: construct_lvol_bdev_on_full_lvol_store
Negative test for constructing a new lvol bdev.
Call construct_lvol_bdev on a full lvol store.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response from get_lvol_stores command
- construct_lvol_bdev on correct lvs_uuid and size is smaller by 1 MB
  from the full size malloc bdev
- try construct_lvol_bdev on the same lvs_uuid as in last step;
  this call should fail as lvol store space is taken by previously created bdev
- destroy_lvol_store
- delete malloc bdev

Expected result:
- first call successful
- second construct_lvol_bdev call return code != 0
- EEXIST response printed to stdout
- no other operation fails

#### TEST CASE 18 - Name: construct_lvol_bdev_name_twice
Negative test for constructing lvol bdev using the same
friendly name twice on the same logical volumes store.
Steps:
- create malloc bdev
- create logical volume store on malloc bdev
- using get_lvol_stores verify that logical volume store was correctly created
and has arguments as provided in step earlier (cluster size, friendly name, base bdev)
- construct logical volume on lvol store and verify it was correctly created
- try to create another logical volume on the same lvol store using
the same friendly name as in previous step; this step should fail
- delete existing lvol bdev
- delete existing lvol store
- delete malloc bdevs

Expected results:
- creating two logical volume with the same friendly name within the same
lvol store should not be possible
- no other operation fails

### resize_lvol_store - negative tests

#### TEST CASE 19 - Name: resize_logical_volume_nonexistent_logical_volume
Negative test for resizing a logical_volume.
Call resize_lvol_bdev with logical volume which does not
exist in configuration.
Steps:
- try resize_lvol_store on logical volume which does not exist

Expected result:
- return code != 0
- Error code: ENODEV ("No such device") response printed to stdout

#### TEST CASE 20 - Name: resize_logical_volume_with_size_out_of_range
Negative test for resizing a logical volume.
Call resize_lvol_store with size argument bigger than size of base bdev.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response get_lvol_stores command
- construct_lvol_bdev on correct lvs_uuid and
  size is equal one quarter of size malloc bdev
- try resize_lvol_bdev on correct lvs_uuid and size is
  equal to size malloc bdev + 1MiB; this call should fail
- delete lvol bdev
- destroy lvol store
- delete malloc bdev

Expected result:
- resize_lvol_bdev call return code != 0
- Error code: ENODEV ("Not enough free clusters left on lvol store")
  response printed to stdout
- no other operation fails

### destroy_lvol_store - negative tests

#### TEST CASE 21 - Name: destroy_lvol_store_nonexistent_lvs_uuid
Call destroy_lvol_store with nonexistent logical_volumes name
exist in configuration.
Steps:
- try to call destroy_lvol_store with lvs_uuid which does not exist

Expected result:
- return code != 0
- Error code response printed to stdout

#### TEST CASE 22 - Name: delete_lvol_store_underlying_bdev
Call destroy_lvol_store after deleting it's base bdev.
Lvol store should be automatically removed on deleting underlying bdev.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response get_lvol_stores command
- delete malloc bdev
- try to destroy lvol store; this call should fail as lvol store
  is no longer present

Expected result:
- destroy_lvol_store retudn code != 0
- Error code: ENODEV ("No such device") response printed to stdout
- no other operation fails

### nested construct_lvol_bdev - test negative

#### TEST CASE 23 - Name: nested_construct_lvol_bdev_on_full_lvol_store
Negative test for constructing a new nested lvol bdev.
Call construct_lvol_bdev on a full lvol store.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response get_lvol_stores command
- construct_lvol_bdev on correct lvs_uuid and size is
  equal to size malloc bdev
- construct nested lvol store on previously created lvol_bdev
- check correct uuid values in response get_lvol_stores command
- construct nested lvol bdev on previously created nested lvol store
  and size is equal to size lvol store
- try construct another lvol bdev as in previous step; this call should fail
  as nested lvol store space is already claimed by lvol bdev
- delete nested lvol bdev
- destroy nested lvol_store
- delete base lvol bdev
- delete base lvol store
- delete malloc bdev

Expected result:
- second construct_lvol_bdev call on nested lvol store return code != 0
- EEXIST response printed to stdout
- no other operation fails

### destroy_lvol_store - negative tests

#### TEST CASE 24 - Name: nested_destroy_logical_volume_negative
Negative test for destroying a nested first lvol store.
Call destroy_lvol_store with correct base bdev name.
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response get_lvol_stores command
- construct_lvol_bdev on correct lvs_uuid and size is
  equal to size malloc bdev
- construct first nested lvol store on created lvol_bdev
- check correct uuid values in response get_lvol_stores command
- construct first nested lvol bdev on correct lvs_uuid and size
- check size of the lvol bdev by command RPC : get_bdevs
- destroy first lvol_store
- delete malloc bdev

Expected result:
- Error code: ENODEV ("the device is busy") response printed to stdout
- no other operation fails

### delete_bdev - positive tests

#### TEST CASE 25 - Name: delete_bdev_positive
Positive test for deleting malloc bdev.
Call construct_lvol_store with correct base bdev name.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev
- check correct uuid values in response get_lvol_stores command
- delete malloc bdev
- check response get_lvol_stores command

Expected result:
- get_lvol_stores: response should be of no value after destroyed lvol store
- no other operation fails

### construct_lvol_store_with_cluster_size  - negative tests

#### TEST CASE 26 - Name: construct_lvol_store_with_cluster_size_max
Negative test for constructing a new lvol store.
Call construct_lvol_store with cluster size is equal malloc bdev size + 1B.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev and cluster size equal
  malloc bdev size in bytes + 1B

Expected result:
- return code != 0
- Error code response printed to stdout

#### TEST CASE 27 - Name: construct_lvol_store_with_cluster_size_min
Negative test for constructing a new lvol store.
Call construct_lvol_store with cluster size = 0.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev and cluster size 0

Expected result:
- return code != 0
- Error code response printed to stdout

### logical volume tasting tests

#### TEST CASE 28 - Name: tasting_positive
Positive test for tasting a multi lvol bdev configuration.
Create a lvol store with some lvol bdevs on NVMe drive and restart vhost app.
After restarting configuration should be automatically loaded and should be exactly
the same as before restarting.
Check that running configuration can be modified after restarting and tasting.
Steps:
- run vhost app with NVMe bdev
- construct lvol store on NVMe bdev
- using get_lvol_stores command verify lvol store was correctly created
- construct five lvol bdevs on previously created lvol store;
  each lvol bdev size is approximately equal to 10% of total lvol store size
  (approximately because of the lvol metadata which consumes some of the space)
- using get_bdevs command verify lvol bdevs were correctly created
- shutdown vhost application by sending SIGTERM signal
- start vhost application with the same NVMe bdev as in the first step
- using get_lvol_stores command verify that previously created lvol strore
  was correctly discovered and loaded by tasting feature (including UUID's)
- using get_bdevs command verify that previously created lvol bdevs were
  correctly discovered and loaded by tasting feature (including UUID's)
- verify if configuration can be modified after tasting:
  construct five more lvol bdevs to fill up loaded lvol store,
  delete all existing lvol bdevs,
  destroy existing lvol store,
  verify removal results using get_lvol_stores and get_bdevs commands
- re-create initial configuration by repeating steps 2-5:
  create lvol store on NVMe bdev, create four lvol bdevs on lvol store and
  verify all configuration call results
- clean running configuration:
  delete all lvol bdevs,
  destroy lvol store
  verify removal results using get_lvol_stores and get_bdevs commands

Expected results:
- configuration is successfully tasted and loaded after restarting vhost
- lvol store attributes (UUID, total size, cluster size, etc.) remain the same after
  loading existing configuration
- lvol bdev attributes (UUID, size, etc.) remain the same after
  loading existing configuration
- all RPC configuration calls successful, return code = 0
- no other operation fails
### SIGTERM

#### TEST CASE 29 - Name: SIGTERM
Call CTRL+C (SIGTERM) occurs after creating lvol store
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response get_lvol_stores command
- Send SIGTERM signal to the application

Expected result:
- calls successful, return code = 0
- get_bdevs: no change
- no other operation fails
