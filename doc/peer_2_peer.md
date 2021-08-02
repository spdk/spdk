# Peer-2-Peer DMAs {#peer_2_peer}

Please note that the functionality discussed in this document is
currently tagged as experimental.

## In this document {#p2p_toc}

* @ref p2p_overview
* @ref p2p_nvme_api
* @ref p2p_cmb_copy
* @ref p2p_issues

## Overview {#p2p_overview}

Peer-2-Peer (P2P) is the concept of DMAing data directly from one PCI
End Point (EP) to another without using a system memory buffer. The
most obvious example of this from an SPDK perspective is using a NVMe
Controller Memory Buffer (CMB) to enable direct copies of data between
two NVMe SSDs.

In this section of documentation we outline how to perform P2P
operations in SPDK and outline some of the issues that can occur when
performing P2P operations.

## The P2P API for NVMe {#p2p_nvme_api}

The functions that provide access to the NVMe CMBs for P2P
capabilities are given in the table below.

Key Functions                               | Description
------------------------------------------- | -----------
spdk_nvme_ctrlr_map_cmb()                   | @copybrief spdk_nvme_ctrlr_map_cmb()
spdk_nvme_ctrlr_unmap_cmb()                 | @copybrief spdk_nvme_ctrlr_unmap_cmb()
spdk_nvme_ctrlr_get_regs_cmbsz()            | @copybrief spdk_nvme_ctrlr_get_regs_cmbsz()

## Determining device support {#p2p_support}

SPDK's identify example application displays whether a device has a controller
memory buffer and which operations it supports. Run it as follows:

~~~{.sh}
./build/examples/identify -r traddr:<pci id of ssd>
~~~

## cmb_copy: An example P2P Application {#p2p_cmb_copy}

Run the cmb_copy example application.

~~~{.sh}
./build/examples/cmb_copy -r <pci id of write ssd>-1-0-1 -w <pci id of write ssd>-1-0-1 -c <pci id of the ssd with cmb>
~~~
This should copy a single LBA (LBA 0) from namespace 1 on the read
NVMe SSD to LBA 0 on namespace 1 on the write SSD using the CMB as the
DMA buffer.

## Issues with P2P {#p2p_issues}

* In some systems when performing peer-2-peer DMAs between PCIe EPs
  that are directly connected to the Root Complex (RC) the DMA may
  fail or the performance may not be great. Basically your milage may
  vary. It is recommended that you use a PCIe switch (such as those
  provided by Broadcom or Microsemi) as that is know to provide good
  performance.
* Even with a PCIe switch there may be occasions where peer-2-peer
  DMAs fail to work. This is probably due to PCIe Access Control
  Services (ACS) being enabled by the BIOS and/or OS. You can disable
  ACS using setpci or via out of tree kernel patches that can be found
  on the internet.
* In more complex topologies involving several switches it may be
  possible to construct multiple paths between EPs. This could lead to
  TLP ordering problems. If you are working in these environments be
  careful!
