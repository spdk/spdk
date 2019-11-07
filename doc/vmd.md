# VMD driver {#vmd}

# In this document {#vmd_toc}

* @ref vmd_intro
* @ref vmd_interface
* @ref vmd_key_functions
* @ref vmd_config
* @ref vmd_app_frame
* @ref vmd_app

# Introduction {#vmd_intro}

Intel Volume Management Device is hardware logic inside processor Root Complex
to help manage PCIe NVMe SSDs. It provides robust Hot Plug support and Status LED
management.

The driver is responsible for enumeration and hooking NVMe devices behind VMD
into SPDK PCIe subsystem. It provides API for LED management and hot plug.

# Public Interface {#vmd_interface}

- spdk/vmd.h

# Key Functions {#vmd_key_functions}

Function                                | Description
--------------------------------------- | -----------
spdk_vmd_init()                         | @copybrief spdk_vmd_init()
spdk_vmd_pci_device_list()              | @copybrief spdk_vmd_pci_device_list()
spdk_vmd_set_led_state()                | @copybrief spdk_vmd_set_led_state()
spdk_vmd_get_led_state()                | @copybrief spdk_vmd_get_led_state()
spdk_vmd_hotplug_monitor()              | @copybrief spdk_vmd_hotplug_monitor()

# Configuration {#vmd_config}

To enable VMD driver enumeration following steps are required:

Check for available VMD devices (VMD need to be properly setup in BIOS first).

Example:
```
$ lspci |grep 201d

$ 5d:05.5 RAID bus controller: Intel Corporation Device 201d (rev 04)
$ 5d:05.5 RAID bus controller: Intel Corporation Device 201d (rev 04)
```

Add VMD devices to PCI_WHITELIST.

Example:
```
$ export PCI_WHITELIST="0000:5d:05.5 0000:d7:05.5"
```

Run setup.sh script
```
$ sudo -E scripts/setup.sh
```

Check for available devices behind VMD with spdk_lspci.

Example:
```
$ sudo ./app/spdk_lspci/spdk_lspci

$ 5d0505:01:00.0 (8086 a54) (NVMe disk behind VMD)
$ 5d0505:03:00.0 (8086 a54) (NVMe disk behind VMD)
$ d70505:01:00.0 (8086 a54) (NVMe disk behind VMD)
$ d70505:03:00.0 (8086 a54) (NVMe disk behind VMD)
$ 0000:5d:05.5 (8086 201d) (VMD)
$ 0000:d7:05.5 (8086 201d) (VMD)
```

# Application framework {#vmd_app_frame}

When application framework is used VMD section need to be added to configuration file:

```
[VMD]
  Enable True
```

or use RPC call before framework starts e.g.

```
$ ./app/spdk_tgt/spdk_tgt --wait_for_rpc
$ ./scripts/rpc enable_vmd
$ ./scripts/rpc framework_start_init
```
# Applications w/o application framework {#vmd_app}

To enable VMD enumeration in SPDK application that are not using application framework
e.g nvme/perf, nvme/identify -V flag is required - please refer to app help if it supports VMD
