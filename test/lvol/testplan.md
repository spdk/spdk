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