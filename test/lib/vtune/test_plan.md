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

## Test Case 2: Run VTune
1. Run VTune with SPDK.
3. Check VTune can be started without issue.
