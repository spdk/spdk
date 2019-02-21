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
  delete malloc bdev; use lvol store and lvol bdev friendly names for delete
  and destroy commands to check if new names can be correctly used for performing
  other RPC operations;

Expected results:
- lvol store and lvol bdevs correctly created
- lvol store and lvol bdevs names updated after renaming operation
- lvol store and lvol bdevs possible to delete using new names
- no other operation fails

#### TEST CASE 801 - Name: rename_lvs_nonexistent
Negative test case for lvol store rename.
Check that error is returned when trying to rename not existing lvol store.

Steps:
- call rename_lvol_store with name pointing to not existing lvol store

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
Check that error is returned when trying to rename not existing lvol bdev.

Steps:
- call rename_lvol_bdev with name pointing to not existing lvol bdev

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
