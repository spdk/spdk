#!/usr/bin/env python3
"""
Guest initialization
====================

This script initializes a guest environment by setting up the necessary resources 
for guest-instance management. It creates a directory structure to store files 
related to the guest, such as:

- PID files
- Monitor files
- Serial input/output logs
- Other instance-specific metadata

Additionally, the script prepares guest storage, including the boot drive, 
using an existing `.qcow2` image (e.g., created via Cloud-init, Packer, etc.).

Retargetable: False
-------------------
"""
import errno
import logging as log
from pathlib import Path

from cijoe.core.misc import download, download_and_verify
from cijoe.qemu.wrapper import Guest


def main(args, cijoe, step):
    """Provision using an existing boot image"""

    guest_name = step.get("with", {}).get("guest_name", None)
    if guest_name is None:
        log.error("missing step-argument: with.guest_name")
        return errno.EINVAL

    guest = Guest(cijoe, cijoe.config, guest_name)

    if (
        system_image_name := (
            cijoe.config.options.get("qemu", {})
            .get("guests", {})
            .get(guest_name, {})
            .get(
                "system_image_name", step.get("with", {}).get("system_image_name", None)
            )
        )
    ) is None:
        log.error("qemu.guests.THIS.system_args.system_image_name is not set")
        return errno.EINVAL

    if (
        disk := (
            cijoe.config.options.get("system-imaging", {})
            .get("images", {})
            .get(system_image_name, {})
            .get("disk", None)
        )
    ) is None:
        log.error(f"system-imaging.images.{system_image_name}.disk is not set")
        return errno.EINVAL

    if not (diskimage_path := Path(disk.get("path"))).exists():
        if (disk_url := disk.get("url", None)) is None:
            log.error(
                f"Cannot download; no 'url' in configuration-file for disk({disk})"
            )
            return errno.EINVAL

        diskimage_path.parent.mkdir(exist_ok=True, parents=True)

        disk_url_checksum = disk.get("url_checksum")
        err, path = (
            download_and_verify(disk_url, disk_url_checksum, diskimage_path)
            if disk_url_checksum
            else download(disk_url, diskimage_path)
        )
        if err:
            log.error(f"err({err}, path({path})")
            return err

    err = guest.initialize(diskimage_path)
    if err:
        log.error(f"guest.initialize({diskimage_path}); err({err})")
        return err

    return 0
