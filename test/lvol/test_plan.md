# Lvol feature test plan

## Objective
The purpose of these tests is to verify the possibility of using lvol configuration in SPDK.

## Methodology
Configuration in test is to be done using example stub application.
All management is done using RPC calls, including logical volumes management.
All tests are performed using malloc backends.
One exception to malloc backends are tests for logical volume
tasting - these require persistent merory like NVMe backend.

Tests will be executed as scenarios - sets of smaller test step
in which return codes from RPC calls is validated.
Some configuration calls may also be validated by use of
"get_*" RPC calls, which provide additional information for verifying
results.

Tests with thin provisioned lvol bdevs, snapshots and clones are using nbd devices.
Before writing/reading to lvol bdev, bdev is installed with rpc start_nbd_disk.
After finishing writing/reading, rpc stop_nbd_disk is used.

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

#### TEST CASE 50 - Name: construct_logical_volume_positive
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

#### TEST CASE 51 - Name: construct_multi_logical_volumes_positive
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
- create and delete four lvol bdevs again from steps above
- destroy lvol store
- delete malloc bdev

Expected result:
- call successful, return code = 0
- get_lvol_store: backend used for construct_lvol_bdev has name
  field set with the same name as returned from RPC call for all repeat
- no other operation fails

#### TEST CASE 52 - Name: construct_lvol_bdev_using_name_positive
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
- delete logical volume bdev
- destroy logical volume store
- delete malloc bdev

Expected result:
- calls successful, return code = 0
- no other operation fails

#### TEST CASE 53 - Name: construct_lvol_bdev_duplicate_names_positive
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
- delete logical volume bdevs
- destroy logical volume stores
- delete malloc bdevs

Expected result:
- calls successful, return code = 0
- no other operation fails

### construct_lvol_bdev - negative tests

#### TEST CASE 100 - Name: construct_logical_volume_nonexistent_lvs_uuid
Negative test for constructing a new logical_volume.
Call construct_lvol_bdev with lvs_uuid which does not
exist in configuration.
Steps:
- try to call construct_lvol_bdev with lvs_uuid which does not exist

Expected result:
- return code != 0
- ENODEV response printed to stdout

#### TEST CASE 101 - Name: construct_lvol_bdev_on_full_lvol_store
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

#### TEST CASE 102 - Name: construct_lvol_bdev_name_twice
Negative test for constructing lvol bdev using the same
friendly name twice on the same logical volume store.
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
- creating two logical volumes with the same friendly name within the same
  lvol store should not be possible
- no other operation fails

### resize_lvol_store - positive tests

#### TEST CASE 150 - Name: resize_logical_volume_positive
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

### resize lvol store - negative tests

#### TEST CASE 200 - Name: resize_logical_volume_nonexistent_logical_volume
Negative test for resizing a logical_volume.
Call resize_lvol_bdev with logical volume which does not
exist in configuration.
Steps:
- try resize_lvol_store on logical volume which does not exist

Expected result:
- return code != 0
- Error code: ENODEV ("No such device") response printed to stdout

#### TEST CASE 201 - Name: resize_logical_volume_with_size_out_of_range
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

### destroy_lvol_store - positive tests

#### TEST CASE 250 - Name: destroy_lvol_store_positive
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

#### TEST CASE 251 - Name: destroy_lvol_store_use_name_positive
Positive test for destroying a logical volume store using
lvol store name instead of uuid for reference.
Call destroy_lvol_store with correct logical volume name
Steps:
- create a malloc bdev
- construct_lvol_store on created malloc bdev
- check correct uuid values in response from get_lvol_stores command
- destroy_lvol_store
- check correct response from get_lvol_stores command
- delete malloc bdev

Expected result:
- calls successful, return code = 0
- get_lvol_stores: response should be of no value after destroyed lvol store
- no other operation fails

#### TEST CASE 252 - Name: destroy_lvol_store_with_lvol_bdev_positive
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

#### TEST CASE 253 - Name: destroy_multi_logical_volumes_positive
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

#### TEST CASE 254 - Name: destroy_resize_logical_volume_positive
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

#### TEST CASE 255 - Name: delete_lvol_store_persistent_positive
Positive test for removing lvol store persistently
Steps:
- construct_lvol_store on NVMe bdev
- destroy lvol store
- delete NVMe bdev
- add NVMe bdev
- check if destroyed lvol store does not exist on NVMe bdev

Expected result:
- get_lvol_stores should not report any existsing lvol stores in configuration
  after deleting and adding NVMe bdev
- no other operation fails

### destroy_lvol_store - negative tests

#### TEST CASE 300 - Name: destroy_lvol_store_nonexistent_lvs_uuid
Call destroy_lvol_store with nonexistent logical_volumes name
exist in configuration.
Steps:
- try to call destroy_lvol_store with lvs_uuid which does not exist

Expected result:
- return code != 0
- Error code response printed to stdout

#### TEST CASE 301 - Name: delete_lvol_store_underlying_bdev
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

### nested destroy_lvol_bdev - negative tests

#### TEST CASE 350 - Name: nested_destroy_logical_volume_negative
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

### nested construct_logical_volume - positive tests

#### TEST CASE 400 - Name: nested_construct_logical_volume_positive
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

### construct_lvol_store - negative tests

#### TEST CASE 450 - Name: construct_lvs_nonexistent_bdev
Negative test for constructing a new lvol store.
Call construct_lvol_store with base bdev name which does not
exist in configuration.
Steps:
- try construct_lvol_store on bdev which does not exist

Expected result:
- return code != 0
- Error code: ENODEV ("No such device") response printed to stdout

#### TEST CASE 451 - Name: construct_lvs_on_bdev_twice
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

#### TEST CASE 452 - Name: construct_lvs_name_twice
Negative test for constructing a new lvol store using the same
friendly name twice.
Steps:
- create two malloc bdevs
- create logical volume store on first malloc bdev
- using get_lvol_stores verify that logical volume store was correctly created
  and has arguments as provided in step earlier (cluster size, friendly name, base bdev)
- try to create another logical volume store on second malloc bdev using the
  same friendly name as before; this step is expected to fail as lvol stores
  cannot have the same name
- delete existing lvol store
- delete malloc bdevs

Expected results:
- creating two logical volume stores with the same friendly name should
not be possible
- no other operation fails

### nested construct_lvol_bdev - test negative

#### TEST CASE 500 - Name: nested_construct_lvol_bdev_on_full_lvol_store
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

### delete_bdev - positive tests

#### TEST CASE 550 - Name: delete_bdev_positive
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

#### TEST CASE 600 - Name: construct_lvol_store_with_cluster_size_max
Negative test for constructing a new lvol store.
Call construct_lvol_store with cluster size is equal malloc bdev size + 1B.
Steps:
- create a malloc bdev
- construct_lvol_store on correct, exisitng malloc bdev and cluster size equal
  malloc bdev size in bytes + 1B

Expected result:
- return code != 0
- Error code response printed to stdout

#### TEST CASE 601 - Name: construct_lvol_store_with_cluster_size_min
Negative test for constructing a new lvol store.
Call construct_lvol_store with cluster size smaller than minimal value of 8192.
Steps:
- create a malloc bdev
- try construct lvol store on malloc bdev with cluster size 8191
- verify that lvol store was not created

Expected result:
- construct lvol store return code != 0
- Error code response printed to stdout

### Provisioning

#### TEST CASE 650 - Name: thin_provisioning_check_space
- create malloc bdev
- construct lvol store on malloc bdev
- create thin provisioned lvol bdev with size equals to lvol store free space
- check and save number of free clusters for lvol store
- write data (less than lvs cluster size) to created lvol bdev starting from offset 0.
- check that free clusters on lvol store was decremented by 1
- write data (lvs cluster size) to lvol bdev with offset set to one and half of cluster size
- check that free clusters on lvol store was decremented by 2
- write data to lvol bdev to the end of its size
- check that lvol store free clusters number equals to 0
- destroy thin provisioned lvol bdev
- check that saved number of free clusters equals to current free clusters
- destroy lvol store
- destroy malloc bdev

Expected result:
- calls successful, return code = 0
- no other operation fails

#### TEST CASE 651 - Name: thin_provisioning_read_empty_bdev
- create malloc bdev
- construct lvol store on malloc bdev
- create thick provisioned lvol bvdev with size equal to lvol store
- create thin provisioned lvol bdev with the same size
- fill the whole thick provisioned lvol bdev
- perform read operations on thin provisioned lvol bdev
  and check if they return zeroes
- destroy thin provisioned lvol bdev
- destroy lvol store
- destroy malloc bdev

Expected result:
- calls successful, return code = 0
- no other operation fails

#### TEST CASE 652 - Name: thin_provisioning_data_integrity_test
- create malloc bdev
- construct lvol store on malloc bdev
- construct thin provisioned lvol bdev with size equal to lvol store
- on the whole lvol bdev perform write operation with verification
- destroy thin provisioned lvol bdev
- destroy lvol store
- destroy malloc bdev

Expected result:
- calls successful, return code = 0
- verification ends with success
- no other operation fails

#### TEST CASE 653 - Name: thin_provisioning_resize
- create malloc bdev
- construct lvol store on malloc bdev
- construct thin provisioned lvol bdevs on created lvol store
  with size equal to 50% of lvol store
- fill all free space of lvol bdev with data
- save number of free clusters for lvs
- resize bdev to full size of lvs
- check if bdev size changed (total_data_clusters*cluster_size
  equal to num_blocks*block_size)
- check if free_clusters on lvs remain unaffected
- perform write operation with verification
  to newly created free space of lvol bdev
- resize bdev to 30M and check if it ended with success
- check if free clusters on lvs equals to saved counter
- destroy thin provisioned lvol bdev
- destroy lvol store
- destroy malloc bdev

Expected result:
- calls successful, return code = 0
- no other operation fails

#### TEST CASE 654 - Name: thin_overprovisioning
- create malloc bdev
- construct lvol store on malloc bdev
- construct two thin provisioned lvol bdevs on created lvol store
  with size equals to free lvs size
- fill first bdev to 75% of its space with specific pattern
- fill second bdev up to 75% of its space
- check that error message occured while filling second bdev with data
- check if data on first disk stayed unchanged
- destroy thin provisioned lvol bdev
- destroy lvol store
- destroy malloc bdev

Expected result:
- calls successful, return code = 0
- no other operation fails

#### TEST CASE 655 - Name: thin_provisioning_filling_disks_less_than_lvs_size
- create malloc bdev
- construct lvol store on malloc bdev
- construct two thin provisioned lvol bdevs on created lvol store
  with size equal to 70% of lvs size
- check if bdevs are available and size of every disk is equal to 70% of lvs size
- fill first disk with 70% of its size and second one also with 70% of its size
- check if operation didn't fail
- destroy thin provisioned lvol bdevs
- destroy lvol store
- destroy malloc bdev

Expected result:
- calls successful, return code = 0
- no other operation fails

### logical volume tasting tests

#### TEST CASE 700 - Name: tasting_positive
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

#### TEST CASE 701 - Name: tasting_lvol_store_positive
Positive test for tasting lvol store.
Steps:
- run vhost app with NVMe bdev
- construct lvol store on NVMe bdev
- delete NVMe bdev
- add NVMe bdev
- check if lvol store still exists in vhost configuration
- destroy lvol store from NVMe bdev

Expected result:
- calls successful (lvol store should be tasted correctly), return code = 0
- no other operation fails

### snapshot and clone

#### TEST CASE 750 - Name: snapshot_readonly
- constrcut malloc bdev
- construct lvol store on malloc bdev
- construct lvol bdev
- fill lvol bdev with 100% of its space using write operation
- create snapshot of created lvol bdev
- check if created snapshot has readonly status
- try to perform write operation on created snapshot
- check if write failed
- destroy lvol bdev
- destroy lvol store
- destroy malloc bdev

Expected result:
- calls successful, return code = 0
- no other operation fails

#### TEST CASE 751 - Name: snapshot_compare_with_lvol_bdev
- construct malloc bdev
- construct lvol store on malloc bdev
- construct thin provisioned lvol bdev with size less than 25% of lvs
- construct thick provisioned lvol bdev with size less than 25% of lvs
- fill first lvol bdev with 50% of its space
- fill second lvol bdev with 100% of their space
- create snapshots of created lvol bdevs and check that they are readonly
- check using cmp program if data on corresponding lvol bdevs
  and snapshots are the same
- fill lvol bdev again with 50% of its space using write operation
- compare thin provisioned bdev clusters with snapshot clusters
  and check that 50% of data are the same and 50% are different
- destroy lvol bdevs
- destroy lvol store
- destroy malloc bdev

Expected result:
- calls successful, return code = 0
- removing snapshot should always end with success
- no other operation fails

#### TEST CASE 752 - Name: snapshot_during_io_traffic
- construct malloc bdev
- construct lvol store on malloc bdev
- construct thin provisioned lvol bdev
- perform write operation with verification to created lvol bdev
- during write operation create snapshot of created lvol bdev
- check that snapshot has been created successfully and check that it is readonly
- check that write operation ended with success
- destroy lvol bdev
- destroy lvol store
- destroy malloc bdev

Expected result:
- calls successful, return code = 0
- no other operation fails

#### TEST CASE 753 - Name: snapshot_of_snapshot
- construct malloc bdev
- construct lvol store on malloc bdev
- construct thick provisioned lvol bdev
- create snapshot of created lvol bdev and check that it is readonly
- create snapshot of previously created snapshot
- check if operation fails
- destroy lvol bdev
- destroy lvol store
- destroy malloc bdev

Expected result:
- calls successful, return code = 0
- creating snapshot of snapshot should fail
- no other operation fails

#### TEST CASE 754 - Name: clone_bdev_only
- construct malloc bdev
- construct lvol store on malloc
- construct thick provisioned lvol bdev
- create clone of created lvol bdev
- check if operation fails
- create snapshot of lvol bdev and check that it is readonly
- create clone of created lvol bdev
- check if operation failed
- create clone of snapshot on the same lvs
  where snaphot was created
- check if operation ends with success
- check if clone is not readonly
- check that clone is thin provisioned
- destroy lvol bdev
- destroy lvol store
- destroy malloc bdev

Expected result:
- calls successful, return code = 0
- cloning thick provisioned lvol bdev should fail
- no other operation fails

#### TEST CASE 755 - Name: clone_writing_to_clone
- construct with malloc bdev
- construct lvol store on malloc bdev
- construct thick provisioned lvol bdev
- fill lvol bdev with 100% of its space
- create snapshot of thick provisioned lvol bdev
- create two clones of created snapshot
- perform write operation to first clone
  and verify that data were written correctly
- check that operation ended with success
- compare second clone with snapshot and check
  that data on both bdevs are the same
- destroy lvol bdev
- destroy lvol store
- destroy malloc bdev

Expected result:
- calls successful, return code = 0
- no other operation fails

### logical volume rename tests

#### TEST CASE 800 - Name: rename_positive
Positive test for lvol store and lvol bdev rename.
Steps:
- create malloc bdev
- construct lvol store on malloc bdev
- create 4 lvol bdevs on top of previously created lvol store
- rename lvol store; verify that lvol store friendly name was
  updated in get_lvol_stores output; verify that prefix in lvol bdevs
  friendly names were also updated
- rename lvol bdevs; use lvols UUID's to point which lvol bdev name to change;
  verify that all bdev names were successfully updated
- rename lvol bdevs; use lvols alias name to point which lvol bdev
  name to change; verify that all bdev names were successfully updated
- clean running configuration: delete lvol bdevs, destroy lvol store,
  delete malloc bdev; use lvol store and lvol bdev friendly names to for delete
  and destroy commands to check if new names can be correctly used for performing
  other RPC operations;

Expected results:
- lvol store and lvol bdevs correctly created
- lvol store and lvol bdevs names updated after renaming operation
- lvol store and lvol bdevs possible to delete using new names
- no other operation fails

#### TEST CASE 801 - Name: rename_lvs_nonexistent
Negative test case for lvol store rename.
Check that error is returned when trying to rename not exisitng lvol store.

Steps:
- call rename_lvol_store with name pointing to not exisitng lvol store

Expected results:
- rename_lvol_store return code != 0
- no other operation fails

#### TEST CASE 802 - Name: rename_lvs_EEXIST
Negative test case for lvol store rename.
Check that error is returned when trying to rename to a name which is already
used by another lvol store.

Steps:
- create 2 malloc bdevs
- construct lvol store on each malloc bdev
- on each lvol store create 4 lvol bdevs
- call rename_lvol_store on first lvol store and try to change its name to
  the same name as used by second lvol store
- verify that both lvol stores still have the same names as before
- verify that lvol bdev have the same aliases as before

Expected results:
- rename_lvol_store return code != 0; not possible to rename to already
  used name
- no other operation fails

#### TEST CASE 803 - Name: rename_lvol_bdev_nonexistent
Negative test case for lvol bdev rename.
Check that error is returned when trying to rename not exisitng lvol bdev.

Steps:
- call rename_lvol_bdev with name pointing to not exisitng lvol bdev

Expected results:
- rename_lvol_bdev return code != 0
- no other operation fails

#### TEST CASE 804 - Name: rename_lvol_bdev_EEXIST
Negative test case for lvol bdev rename.
Check that error is returned when trying to rename to a name which is already
used by another lvol bdev.

Steps:
- create malloc bdev
- construct lvol store on malloc bdev
- construct 2 lvol bdevs on lvol store
- call rename_lvol_bdev on first lvol bdev and try to change its name to
  the same name as used by second lvol bdev
- verify that both lvol bdev still have the same names as before

Expected results:
- rename_lvol_bdev return code != 0; not possible to rename to already
  used name
- no other operation fails

### SIGTERM

#### TEST CASE 10000 - Name: SIGTERM
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
