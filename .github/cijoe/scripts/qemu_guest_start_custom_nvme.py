#!/usr/bin/env python3
"""
Start a qemu-guest with NVMe devices
====================================

This creates a configuration, which on a recent Linux installation would be
something like:

* /dev/ng0n1 -- nvm
* /dev/ng0n2 -- zns

* /dev/ng1n1 -- nvm

* /dev/ng2n1 -- nvm (mdts=0 / unlimited)
* /dev/ng3n1 -- fdp-enabled subsystem and nvm namespace

Using the the 'ng' device-handle, as it is always available, whereas 'nvme' are
only for block-devices. Regardless, the above is just to illustrate one
possible "appearance" of the devices in Linux.

Retargetable: false
-------------------
"""
import errno
import logging as log
from pathlib import Path

from cijoe.qemu.wrapper import Guest


def qemu_nvme_args(nvme_img_root):
    """
    Returns list of drive-args and a string of qemu-arguments

    @returns drives, args
    """

    lbads = 12

    def subsystem(id, nqn=None, aux={}):
        """
        Generate a subsystem configuration
        @param id Identifier, could be something like 'subsys0'
        @param nqn Non-qualified-name, assigned verbatim when provided
        @param aux Auxilary arguments, e.g. add {fdp: on} here, to enable fdp
        """

        args = {"id": id}
        if nqn:
            args["nqn"] = nqn
        args.update(aux)

        return [
            "-device",
            ",".join(["nvme-subsys"] + [f"{k}={v}" for k, v in args.items()]),
        ]

    def controller(
        id, serial, mdts, downstream_bus, upstream_bus, controller_slot, subsystem=None
    ):
        args = {
            "id": id,
            "serial": serial,
            "bus": downstream_bus,
            "mdts": mdts,
            "ioeventfd": "on",
        }
        if subsystem:
            args["subsys"] = subsystem

        return [
            "-device",
            f"xio3130-downstream,id={downstream_bus},"
            f"bus={upstream_bus},chassis=2,slot={controller_slot}",
            "-device",
            ",".join(["nvme"] + [f"{k}={v}" for k, v in args.items()]),
        ]

    def namespace(controller_id, nsid, aux={}):
        """Returns qemu-arguments for a namespace configuration"""

        drive_id = f"{controller_id}n{nsid}"
        drive = {
            "id": drive_id,
            "file": str(nvme_img_root / f"{drive_id}.img"),
            "format": "raw",
            "if": "none",
            "discard": "on",
            "detect-zeroes": "unmap",
        }
        # drives.append(drive1)
        controller_namespace = {
            "id": drive_id,
            "drive": drive_id,
            "bus": controller_id,
            "nsid": nsid,
            "logical_block_size": 1 << lbads,
            "physical_block_size": 1 << lbads,
            **aux,
        }

        return drive, [
            "-drive",
            ",".join(f"{k}={v}" for k, v in drive.items()),
            "-device",
            ",".join(
                ["nvme-ns"] + [f"{k}={v}" for k, v in controller_namespace.items()]
            ),
        ]

    drives = []

    # NVMe configuration arguments
    nvme = []
    nvme += ["-device", "pcie-root-port,id=pcie_root_port1,chassis=1,slot=1"]

    upstream_bus = "pcie_upstream_port1"
    nvme += ["-device", f"x3130-upstream,id={upstream_bus},bus=pcie_root_port1"]

    #
    # Nvme0 - Controller for functional verification of namespaces with NVM and ZNS
    # command-sets
    #
    controller_id1 = "nvme0"
    controller_bus1 = "pcie_downstream_port1"
    controller_slot1 = 1
    nvme += controller(
        controller_id1, "deadbeef", 7, controller_bus1, upstream_bus, controller_slot1
    )

    # Nvme0n1 - NVM namespace
    drive1, qemu_nvme_dev1 = namespace(controller_id1, 1)
    nvme += qemu_nvme_dev1
    drives.append(drive1)

    # Nvme0n2 - ZNS namespace
    zoned_attributes = {
        "zoned": "on",
        "zoned.zone_size": "32M",
        "zoned.zone_capacity": "28M",
        "zoned.max_active": 256,
        "zoned.max_open": 256,
        "zoned.zrwas": 32 << lbads,
        "zoned.zrwafg": 16 << lbads,
        "zoned.numzrwa": 256,
    }

    drive2, qemu_nvme_dev2 = namespace(controller_id1, 2, zoned_attributes)
    nvme += qemu_nvme_dev2
    drives.append(drive2)

    # Nvme1 - Controller dedicated to Fabrics testing
    controller_id2 = "nvme1"
    controller_bus2 = "pcie_downstream_port2"
    controller_slot2 = 2
    nvme += controller(
        controller_id2, "adcdbeef", 7, controller_bus2, upstream_bus, controller_slot2
    )

    # Nvme1n1 - Namespace with NVM command-set
    drive3, qemu_nvme_dev3 = namespace(controller_id2, 1)
    nvme += qemu_nvme_dev3
    drives.append(drive3)

    # Nvme2 - Controller dedicated to testing HUGEPAGES / Large MDTS
    controller_id3 = "nvme2"
    controller_bus3 = "pcie_downstream_port3"
    controller_slot3 = 3
    nvme += controller(
        controller_id3, "beefcace", 0, controller_bus3, upstream_bus, controller_slot3
    )

    # Nvme2n1 - NVM namespace
    drive4, qemu_nvme_dev4 = namespace(controller_id3, 1)
    nvme += qemu_nvme_dev4
    drives.append(drive4)

    #
    # Nvme3 - Controller with FDP enabled subsystem
    #
    subsys_name = "subsys0"
    subsys_attributes = {
        "fdp": "on",
        "fdp.nruh": "8",
        "fdp.nrg": "32",
        "fdp.runs": "40960",
    }
    nvme += subsystem(subsys_name, aux=subsys_attributes)

    controller_id4 = "nvme3"
    controller_bus4 = "pcie_downstream_port4"
    controller_slot4 = 4
    nvme += controller(
        controller_id4,
        "beefcace",
        0,
        controller_bus4,
        upstream_bus,
        controller_slot4,
        subsys_name,
    )

    drv_fdp, qemu_nvme_dev_fdp = namespace(controller_id4, 1, {"fdp.ruhs": "'0;5;6;7'"})
    nvme += qemu_nvme_dev_fdp
    drives.append(drv_fdp)

    #
    # Nvme4 - Controller with PI enabled
    #

    controller_id5 = "nvme4"
    controller_bus5 = "pcie_downstream_port5"
    controller_slot5 = 5
    nvme += controller(
        controller_id5, "feebdaed", 7, controller_bus5, upstream_bus, controller_slot5
    )

    # Nvme4n1 - NVM namespace with PI type 1
    drv_pi1, qemu_nvme_dev_pi1 = namespace(controller_id5, 1, {"ms": 8, "pi": 1})
    nvme += qemu_nvme_dev_pi1
    drives.append(drv_pi1)

    # Nvme4n2 - NVM namespace with PI type 2
    drv_pi2, qemu_nvme_dev_pi2 = namespace(controller_id5, 2, {"ms": 8, "pi": 2})
    nvme += qemu_nvme_dev_pi2
    drives.append(drv_pi2)

    # Nvme4n3 - NVM namespace with PI type 3
    drv_pi3, qemu_nvme_dev_pi3 = namespace(controller_id5, 3, {"ms": 8, "pi": 3})
    nvme += qemu_nvme_dev_pi3
    drives.append(drv_pi3)

    return drives, nvme


def main(args, cijoe, step):
    """Start a qemu guest"""

    drive_size = "8G"
    guest = Guest(cijoe, cijoe.config)

    nvme_img_root = Path(step.get("with", {}).get("nvme_img_root", guest.guest_path))

    drives, nvme_args = qemu_nvme_args(nvme_img_root)

    # Check that the backing-storage exists, create them if they do not
    for drive in drives:
        err, _ = cijoe.run_local(f"[ -f {drive['file']} ]")
        if err:
            guest.image_create(drive["file"], drive["format"], drive_size)
        err, _ = cijoe.run_local(f"[ -f {drive['file']} ]")

    err = guest.start(extra_args=nvme_args)
    if err:
        log.error(f"guest.start() : err({err})")
        return err

    started = guest.is_up()
    if not started:
        log.error("guest.is_up() : False")
        return errno.EAGAIN

    return 0
