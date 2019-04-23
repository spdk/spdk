#Reduce compression bdev module

##Introduction

The reduce comprssion bdev module is a library for compression bdev.currently SPDK has already merged all codes. but this function is still experimental stage.

## testing configuration
We need to create comprssion bdev based on following basic bdevs:
malloc;
nvme.
the reduce compression bdev module also need to support basic IO and file system operation.

The codes need to deal with following scenario:
* PM is missing
* PM file is corrupt
* PM that exists goes with a different system

##Test Case 1: run basic IO to test reduce comprssion bdev module

##Test Case 2: run basic file system to test reduce compression bdev module

##Test Case 3: PM file is missing while using reduce comprssion bdev module

##Test Case 4: PM file is corrupt while using reduce comprssion bdev module

##Test Case 5: PM  that exists goes with a different system while using reduce comprssion bdev module