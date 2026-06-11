# AMD NTB Driver {#ntb_amd}

## Overview {#ntb_amd_overview}

The `ntb_amd` library is a user-space driver for the Non-Transparent Bridge
(NTB) found on AMD EPYC 4th-Generation ("SP4"/"SP4r2") processors. An NTB
connects two independent hosts (or two root complexes) over PCI Express and
lets each side expose a window of its own memory to the peer. Writes that
land in a memory window on one host are forwarded across the bridge and
appear in the backing memory of the other host, providing a low-latency
shared-memory transport without a transparent PCIe fabric.

The driver runs entirely in user space on top of SPDK's `env_dpdk` PCI
layer (vfio-pci / uio). It manages device probe/attach, BAR mapping,
memory-window address translation, scratchpad registers, doorbells, link
state, and the peer-to-peer bring-up handshake.

For the hardware register layout and electrical/topology details, refer to
the AMD reference document.The register offsets used by this driver are collected in
`include/spdk/ntb_amd_spec.h`.

## Public Interface {#ntb_amd_interface}

- spdk/ntb_amd.h — driver API
- spdk/ntb_amd_spec.h — register layout and on-the-wire protocol constants

## Topology model {#ntb_amd_topology}

The AMD NTB is **asymmetric**: when the link comes up, the two endpoints
are assigned distinct roles, advertised through bit 0 of the side-info
register and cached by the driver:

- **Primary** (`NTB_TOPO_PRI`)
- **Secondary** (`NTB_TOPO_SEC`)

The role is reported by `spdk_ntb_amd_attr_get()` under the `topo`
attribute (`SPDK_NTB_TOPO_NAME`) and resolves to one of the values in
`enum spdk_ntb_topo` (`NTB_TOPO_NONE` / `NTB_TOPO_PRI` / `NTB_TOPO_SEC`). A
device that reports `NTB_TOPO_NONE` is not correctly cabled or has not been
configured for NTB mode in the BIOS, and cannot be used.

The roles are not interchangeable at runtime — they are fixed by hardware
and BIOS configuration. Applications must therefore decide, per role, which
side initiates data movement (see the example below).

### Hardware resources {#ntb_amd_resources}

Per AMD 4th-gen NTB (see `include/spdk/ntb_amd_spec.h`):

Resource          | Count | Notes
----------------- | ----- | -----------------------------------------------
Memory windows    | 2     | Exposed via BAR2/3 (`NTB_BAR23`) and BAR4/5 (`NTB_BAR45`)
Scratchpads       | 16    | 32-bit registers, mirrored to the peer
Doorbells         | 16    | Peer-settable interrupt/notification bits

**Memory windows** are programmed with a translation (`XLAT`) and a limit
(`LIMIT`) register so that peer writes into a BAR land in a chosen local
DMA-capable buffer.

**Scratchpads** carry the bring-up handshake. The agreed slot layout is in
`enum spdk_ntb_spad_idx`: slot 0 is reserved for the hardware peer-discovery
interlock and slot 1 (`SPAD_NUM_MWS`) publishes the memory-window count.
The actual address translation is programmed directly into the peer's
`XLAT` registers by the driver, so per-window base addresses and sizes do
not need to be exchanged through scratchpads.

**Doorbells** let one side notify the other of an event (peer ready, peer
going down, copy complete). The application rings a peer doorbell with
`spdk_ntb_amd_peer_db_set()` and observes/clears its own with
`spdk_ntb_amd_db_read()` / `spdk_ntb_amd_db_clear()`.

## Key Functions {#ntb_amd_key_functions}

Function                                 | Description
---------------------------------------- | -----------
spdk_ntb_amd_probe()                     | @copybrief spdk_ntb_amd_probe()
spdk_ntb_amd_detach()                    | @copybrief spdk_ntb_amd_detach()
spdk_ntb_amd_set_active_mw()             | @copybrief spdk_ntb_amd_set_active_mw()
spdk_ntb_amd_handshake_work()            | @copybrief spdk_ntb_amd_handshake_work()
spdk_ntb_amd_dev_start()                 | @copybrief spdk_ntb_amd_dev_start()
spdk_ntb_amd_peer_ready_get()            | @copybrief spdk_ntb_amd_peer_ready_get()
spdk_ntb_amd_peer_ready_set()            | @copybrief spdk_ntb_amd_peer_ready_set()

## Two-host bring-up {#ntb_amd_bringup}

NTB is inherently a two-node setup: the same application is started on both
hosts, and neither side can complete bring-up until the peer is also
running. The handshake is symmetric up to the point where the link is
declared up; from there the application chooses an initiator based on the
topology role.

The sequence on **each** host is:

1. **Attach.** Call `spdk_ntb_amd_probe()` to enumerate and attach the NTB
   device. The attach callback yields an opaque `struct spdk_ntb_amd_dev`.
2. **Select a memory window.** Call `spdk_ntb_amd_set_active_mw()` to choose
   which window (BAR2/3 or BAR4/5) this run uses.
3. **Publish the handshake.** Allocate a physically-contiguous, DMA-capable
   staging buffer at least `sum(mw_size[i])` bytes long (e.g. with
   `spdk_dma_zmalloc()`) and call `spdk_ntb_amd_handshake_work()`. This
   publishes the memory-window count through a scratchpad, programs each
   window's translation to a non-overlapping slice of the buffer, and rings
   doorbell 0 to notify the peer.
4. **Wait for the peer.** Poll the doorbell. When the peer-ready doorbell
   arrives, verify the peer's memory-window count matches the local count
   (`spdk_ntb_amd_attr_get(SPDK_NTB_MW_CNT_NAME)` vs. the peer's
   `SPAD_NUM_MWS` scratchpad), then mark the peer ready with
   `spdk_ntb_amd_peer_ready_set()`. Because either host may come up first,
   re-running the handshake and re-ringing the doorbell on receipt makes the
   bring-up order-independent.
5. **Finalize.** Call `spdk_ntb_amd_dev_start()` once the link is up and the
   peer is ready, before any data movement.
6. **Move data.** The initiator writes into its memory window (the bytes
   appear in the peer's staging buffer) and rings the copy-done doorbell;
   the peer waits for that doorbell and reads its buffer.

Tear down with `spdk_ntb_amd_detach()`, which unmaps every BAR taken during
bring-up and releases the device.

## NTB perf example {#ntb_amd_perf}

`examples/ntb/perf` is a reference application that exercises the remote
memory window using either a CPU `memcpy` or AE4DMA-offloaded copies. It
uses only the public driver API and serves as a worked example of the
bring-up sequence above. (The DMA path drives the companion AE4DMA engine;
see [accel_fw](accel_fw.md) for the AE4DMA accel module.)

Run the same binary on both hosts. Options:

Option | Meaning
------ | -------------------------------------------------------------
`-O`   | Total order (total transfer size, as a power of two)
`-o`   | Chunk order (per-chunk size, as a power of two)
`-w`   | Initiate the copy from this side. Honored on the Primary; to drive the Secondary-to-Primary direction, change the topology gate in `perf.c` and rebuild.
`-a`   | Use AE4DMA DMA copies instead of CPU `memcpy`
`-k`   | Number of AE4DMA engines to use
`-l`   | Hardware queues (channels) per engine
`-M`   | Memory-window index to use (`0..N-1`)

Typical invocation — Primary host initiates a DMA copy, Secondary reads:

~~~bash
# On the Primary host (writer):
./build/examples/ntb_perf -m 0x1 -a -w

# On the Secondary host (reader):
./build/examples/ntb_perf -m 0x1 -a
~~~

The writer copies its staging buffer into the active memory window and rings
the copy-done doorbell; the reader blocks until that doorbell fires, then
reports how many bytes it received. Pass `-w` on exactly one side per run.
