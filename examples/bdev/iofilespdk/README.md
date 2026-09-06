# SPDK + CUDA: GPU-to-Storage Buffer Write

Proof of concept: a CUDA kernel writes directly into the DMA buffer
SPDK uses for NVMe I/O, so the CPU doesn't copy the data on its way
to the storage device.

## What it does

1. Reads an input file (`-f <path>`).
2. Allocates an SPDK DMA buffer (`spdk_dma_zmalloc`).
3. A CUDA kernel copies the file's data into that buffer directly —
   made GPU-visible via `cudaHostRegister`/`cudaHostGetDevicePointer`,
   no `cudaMemcpy` on this side.
4. SPDK writes that buffer to the target bdev, reads it back, and
   dumps the result to `readback.bin`.

## Verified

Byte-for-byte round-trip (`diff`/`sha256sum`) on a `Malloc0` bdev, for
both text and binary files (e.g. ELF). Tested on a local VM and two
cluster nodes, with real NVMe hardware.

## Known limitation

The claim holds for the **destination** buffer — the one that actually
reaches the SSD is written only by the GPU. It doesn't yet hold for
the **source**: the input file is read into plain `malloc()` memory,
which failed to register with CUDA directly (silent no-op, cause
unconfirmed). Current workaround: stage it into a separate
`cudaHostAlloc`'d buffer with one CPU-side `memcpy` before the kernel
runs. Fix: allocate `file_data` itself with `cudaHostAlloc` and drop
the staging buffer — not yet done.

## Files

- `iofilespdk.c` — SPDK app: args, bdev open, buffer setup, I/O
  callbacks.
- `gpu_fill.h` / `gpu_fill.cu` — `gpu_copy_buffer()`: registers the
  SPDK buffer, stages the source, launches the copy kernel.
- `Makefile` — adds an `nvcc` build step to SPDK's example Makefile.

## Building

```bash
cd examples/bdev/iofilespdk
make gpu_fill.o && make
```

(`gpu_fill.o` must be built explicitly first — not yet wired into the
app's dependency chain.)

## Running

```bash
sudo ./build/examples/iofilespdk -f <file> -b Malloc0 -c malloc_bdev.json
diff <file> readback.bin && echo MATCH
```

For real NVMe: bind the device first (`scripts/setup.sh`), then use
`-b Nvme0n1 -c nvme_bdev.json`.

## Relationship to BaM

Proves the shared-buffer primitive, not BaM's full model — a CPU
thread still owns the queue pair and calls `spdk_bdev_write`. The GPU
doesn't submit I/O or poll completions itself yet.

## Notes

On the cluster, `uio_pci_generic` fails to load (blocked cluster-wide);
`vfio-pci` works with `DRIVER_OVERRIDE=vfio-pci`.