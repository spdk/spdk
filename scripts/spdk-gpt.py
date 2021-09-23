#!/usr/bin/env python3

import argparse
import os
import stat
import struct


def block_exists(block):
    return os.path.exists(block) and stat.S_ISBLK(os.stat(block).st_mode)


def lbsize(block):
    if not os.path.exists("/sys/block/%s/queue/logical_block_size"):
        return 512

    with open("/sys/block/%s/queue/logical_block_size" % block) as lbs:
        return int(lbs.read())


def readb(block, offset, length, format="Q"):
    b = os.open(block, os.O_RDONLY)
    os.lseek(b, offset, os.SEEK_SET)
    data = os.read(b, length)
    os.close(b)
    return struct.unpack(format, data)[0]


def is_spdk_gpt(block, entry):
    block_path = "/dev/" + block

    if not block_exists(block_path):
        print("%s is not a block device" % block)
        return False

    disk_lbsize = lbsize(block)
    gpt_sig = 0x5452415020494645  # EFI PART
    spdk_guid = [0x7c5222bd, 0x8f5d, 0x4087, 0x9c00, 0xbf9843c7b58c]

    if readb(block_path, disk_lbsize, 8) != gpt_sig:
        print("No valid GPT data, bailing")
        return False

    part_entry_lba = disk_lbsize * readb(block_path, disk_lbsize + 72, 8)
    part_entry_lba = part_entry_lba + (entry - 1) * 128

    guid = [
        readb(block_path, part_entry_lba, 4, "I"),
        readb(block_path, part_entry_lba + 4, 2, "H"),
        readb(block_path, part_entry_lba + 6, 2, "H"),
        readb(block_path, part_entry_lba + 8, 2, ">H"),
        readb(block_path, part_entry_lba + 10, 8, ">Q") >> 16
    ]

    return guid == spdk_guid


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Checks if SPDK GUID is present on given block device')
    parser.add_argument('block', type=str, help='block device to check')
    parser.add_argument('-e', '--entry', dest='entry', help='GPT partition entry',
                        required=False, type=int, default=1)
    args = parser.parse_args()
    try:
        if is_spdk_gpt(args.block.replace("/dev/", ""), args.entry):
            exit(0)
        exit(1)
    except Exception as e:
        print("Failed to read GPT data from %s (%s)" % (args.block, e))
        exit(1)
