#VTune integration Test Plan

## System/Integration testing
 - Test SPDK can be compiled with VTune enabled.
 - Test VTune can be run with SPDK.

## Test Cases Description

### Test Cases prerequistes
1. Get latest SPDK codes and VTune.
2. run "git submodule update --init" in SPDK directory.

## Test Case 1: Compile SPDK with VTune
1. Run command "./configure --with-vtune=/vtune/path/" in SPDK directory.
2. Run command "make" in SPDK directory.
3. Check make command finished without issue.

## Test Case 2: Run VTune with SPDK iscsi target
1. Run SPDK iscsi target.
2. Discover and login to SPDK iscsi target in initiator machine.
3. Run fio.py in initiator machines. The argument is as below:
./fio.py 4096 16 randread 1200
4. Run Vtune to monitor SPDK iscsi target status.

## Test Case 3: Run VTune with SPDK nvmf target
1. Run SPDK nvmf target.
2. Discover and connect to SPDK nvmf target in host machine.
3. Run nvmf_fio.py in host machine. The argument is as below:
./nvmf_fio.py 4096 16 randread 1200
4. Run Vtune to monitor SPDK nvmf target status.
