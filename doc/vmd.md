# VMD driver {#vmd}

## In this document {#vmd_toc}

* @ref vmd_intro
* @ref vmd_interface
* @ref vmd_key_functions
* @ref vmd_config
* @ref vmd_app_frame
* @ref vmd_app
* @ref vmd_led

## Introduction {#vmd_intro}

Intel Volume Management Device is a hardware logic inside processor's Root Complex
responsible for management of PCIe NVMe SSDs. It provides robust Hot Plug support
and Status LED management.

The driver is responsible for enumeration and hooking NVMe devices behind VMD
into SPDK PCIe subsystem. It also provides API for LED management and hot plug.

## Public Interface {#vmd_interface}

- spdk/vmd.h

## Key Functions {#vmd_key_functions}

Function                                | Description
--------------------------------------- | -----------
spdk_vmd_init()                         | @copybrief spdk_vmd_init()
spdk_vmd_pci_device_list()              | @copybrief spdk_vmd_pci_device_list()
spdk_vmd_set_led_state()                | @copybrief spdk_vmd_set_led_state()
spdk_vmd_get_led_state()                | @copybrief spdk_vmd_get_led_state()
spdk_vmd_hotplug_monitor()              | @copybrief spdk_vmd_hotplug_monitor()

## Configuration {#vmd_config}

To enable VMD driver enumeration, the following steps are required:

Check for available VMD devices (VMD needs to be properly set up in BIOS first).

Example:
```
$ lspci | grep 201d

$ 5d:05.5 RAID bus controller: Intel Corporation Device 201d (rev 04)
$ d7:05.5 RAID bus controller: Intel Corporation Device 201d (rev 04)
```

Run setup.sh script with VMD devices set in PCI_ALLOWED.

Example:
```
$ PCI_ALLOWED="0000:5d:05.5 0000:d7:05.5" scripts/setup.sh
```

Check for available devices behind the VMD with spdk_lspci.

Example:
```
$ ./build/bin/spdk_lspci

 5d0505:01:00.0 (8086 a54) (NVMe disk behind VMD)
 5d0505:03:00.0 (8086 a54) (NVMe disk behind VMD)
 d70505:01:00.0 (8086 a54) (NVMe disk behind VMD)
 d70505:03:00.0 (8086 a54) (NVMe disk behind VMD)
 0000:5d:05.5 (8086 201d) (VMD)
 0000:d7:05.5 (8086 201d) (VMD)
```

VMD NVMe BDF could be used as regular NVMe BDF.

Example:
```
$ ./scripts/rpc.py bdev_nvme_attach_controller -b NVMe1 -t PCIe -a 5d0505:01:00.0
```

## Application framework {#vmd_app_frame}

When application framework is used, VMD section needs to be added to the configuration file:

JSON config:
```
{
    "subsystem": "vmd",
    "config": [
      {
        "method": "enable_vmd",
        "params": {}
      }
    ]
}
```

or use RPC call before framework starts e.g.
```
$ ./build/bin/spdk_tgt --wait_for_rpc
$ ./scripts/rpc.py enable_vmd
$ ./scripts/rpc.py framework_start_init
```
## Applications w/o application framework {#vmd_app}

To enable VMD enumeration in SPDK application that are not using application framework
e.g nvme/perf, nvme/identify -V flag is required - please refer to app help if it supports VMD.

Applications need to call spdk_vmd_init() to enumerate NVMe devices behind the VMD prior to calling
spdk_nvme_(probe|connect).
To support hot plugs spdk_vmd_hotplug_monitor() needs to be called periodically.

## LED management {#vmd_led}

VMD LED utility in the [examples/vmd/led](https://github.com/spdk/spdk/tree/master/examples/vmd/led)
could be used to set LED states.

In order to verify that a platform is correctly configured to support LED management, ledctl(8) can
be utilized.  For instructions on how to use it, consult the manual page of this utility.
