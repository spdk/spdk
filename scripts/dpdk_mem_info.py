#!/usr/bin/env python3

import argparse
import os
from enum import Enum


class memory:
    def __init__(self, size):
        self.size = size
        self.heaps = []
        self.mempools = []
        self.memzones = []

    def get_size(self):
        return self.size

    def add_mempool(self, pool):
        self.mempools.append(pool)

    def add_memzone(self, zone):
        self.memzones.append(zone)

    def add_heap(self, heap):
        self.heaps.append(heap)

    def print_info(self):
        print("DPDK memory size {} in {} heap(s), {} mempool(s), and {} independent memzone(s)"
              .format(human_readable_size(self.size), len(self.heaps), len(self.mempools), len(self.memzones)))
        print("heaps--------------")
        for heap in self.heaps:
            heap.print_info()
        print("end heaps----------")
        print("mempools-----------")
        for pool in self.mempools:
            pool.print_info()
        print("end mempools-------")
        print("memzones-----------")
        for zone in self.memzones:
            zone.print_info()
        print("end memzones-------")

    def associate_memzones_and_mempools(self):
        for pool in self.mempools:
            for zone in self.memzones:
                if pool.name in zone.name:
                    pool.add_memzone(zone)

        for pool in self.mempools:
            for zone in pool.memzones:
                if zone in self.memzones:
                    self.memzones.remove(zone)


class heap:
    def __init__(self, id, size, num_allocations):
        self.id = id
        self.size = size
        self.num_allocations = num_allocations
        self.free_elements = []
        self.busy_elements = []

    def add_element(self, element):
        if element.status == heap_elem_status.FREE:
            self.free_elements.append(element)
        else:
            self.busy_elements.append(element)

    def print_info(self):
        print("heap id: {} of size {} with {} allocations".format(self.id, human_readable_size(self.size), self.num_allocations))


class mempool:
    def __init__(self, name, num_objs, num_populated_objs, obj_size):
        self.name = name
        self.num_objs = num_objs
        self.num_populated_objs = num_populated_objs
        self.obj_size = obj_size
        self.memzones = []

    def add_memzone(self, memzone):
        self.memzones.append(memzone)

    def print_info(self):
        print("mempool name: {} of size: {} comprised of {} memzones"
              .format(self.name, human_readable_size(self.obj_size * self.num_objs), len(self.memzones)))
        # Not sure about this yet.
        # for zone in self.memzones:
        #    zone.print_info()


class memzone:
    def __init__(self, name, size, address):
        self.name = name
        self.size = size
        self.address = address
        self.segments = []

    def add_segment(self, segment):
        self.segments.append(segment)

    def print_info(self):
        print("memzone_name: {} of size: {} at address {}".format(self.name, human_readable_size(self.size), hex(self.address)))


class segment:
    def __init__(self, size, address):
        self.size = size
        self.address = address


class heap_elem_status(Enum):
    FREE = 0
    BUSY = 1


class heap_element:
    def __init__(self, size, status, addr):
        self.status = status
        self.size = size
        self.addr = addr


class parse_state(Enum):
    PARSE_MEMORY_SIZE = 0
    PARSE_MEMZONES = 1
    PARSE_MEMZONE_SEGMENTS = 2
    PARSE_MEMPOOLS = 3
    PARSE_MEMPOOL_INFO = 4
    PARSE_HEAPS = 5
    PARSE_HEAP_ELEMENTS = 6


def human_readable_size(raw_value):
    power = 0
    while (raw_value > 100.0):
        raw_value = raw_value / 1024.0
        power = power + 1

    suffix = "Undefined"
    if power == 0:
        suffix = "B"
    if power == 1:
        suffix = "KiB"
    if power == 2:
        suffix = "MiB"
    if power == 3:
        suffix = "GiB"
    if power == 4:
        suffix = "TiB"
    if power == 5:
        suffix = "PiB"

    return "%.2f %s" % (raw_value, suffix)


def parse_zone(line):
    zone, info = line.split(':', 1)
    name, length, addr, trash = info.split(',', 3)

    trash, name = name.split(':', 1)
    name = name.replace("<", "")
    name = name.replace(">", "")
    trash, length = length.split(':', 1)
    trash, addr = addr.split(':', 1)

    return memzone(name, int(length, 0), int(addr, 0))


def parse_segment(line):
    trash, addr, iova, length, pagesz = line.split(':')
    addr, trash = addr.strip().split(' ')
    length, trash = length.strip().split(' ')

    return segment(int(length, 0), int(addr, 0))


def parse_mempool_name(line):
    trash, info = line.split()
    name, addr = line.split('@')
    name = name.replace("<", "")
    name = name.replace(">", "")
    trash, name = name.split()

    return name


def parse_mem_stats(stat_path):
    state = parse_state.PARSE_MEMORY_SIZE
    with open(stat_path, "r") as stats:

        line = stats.readline()
        while line != '':
            if state == parse_state.PARSE_MEMORY_SIZE:
                if "DPDK memory size" in line:
                    mem_size = int(line.replace("DPDK memory size ", ""))
                    memory_struct = memory(mem_size)
                    state = parse_state.PARSE_MEMZONES
                line = stats.readline()

            if state == parse_state.PARSE_MEMZONES:
                if line.find("Zone") == 0:
                    zone = parse_zone(line)
                    state = parse_state.PARSE_MEMZONE_SEGMENTS
                line = stats.readline()

            if state == parse_state.PARSE_MEMZONE_SEGMENTS:
                if line.find("Zone") == 0:
                    memory_struct.add_memzone(zone)
                    state = parse_state.PARSE_MEMZONES
                    continue
                elif line.lstrip().find("addr:") == 0:
                    segment = parse_segment(line)
                    zone.add_segment(segment)
                elif "DPDK mempools." in line:
                    state = parse_state.PARSE_MEMPOOLS
                    continue
                line = stats.readline()

            if state == parse_state.PARSE_MEMPOOLS:
                mempool_info = {}
                if line.find("mempool") == 0:
                    mempool_info['name'] = parse_mempool_name(line)
                    state = parse_state.PARSE_MEMPOOL_INFO
                line = stats.readline()

            if state == parse_state.PARSE_MEMPOOL_INFO:
                if line.find("mempool") == 0:
                    try:
                        new_mempool = mempool(mempool_info['name'], int(mempool_info['size'], 0),
                                              int(mempool_info['populated_size'], 0), int(mempool_info['total_obj_size'], 0))
                        memory_struct.add_mempool(new_mempool)
                    except KeyError:
                        print("proper key values not provided for mempool.")
                    state = parse_state.PARSE_MEMPOOLS
                    continue
                elif "cache" in line:
                    pass
                elif "DPDK malloc stats." in line:
                    try:
                        new_mempool = mempool(mempool_info['name'], int(mempool_info['size'], 0),
                                              int(mempool_info['populated_size'], 0), int(mempool_info['total_obj_size'], 0))
                        memory_struct.add_mempool(new_mempool)
                    except KeyError:
                        print("proper key values not provided for mempool.")
                    while "DPDK malloc heaps." not in line:
                        line = stats.readline()
                    state = parse_state.PARSE_HEAPS
                else:
                    try:
                        field, value = line.strip().split('=')
                        mempool_info[field] = value
                    except Exception as e:
                        pass
                line = stats.readline()

            if state == parse_state.PARSE_HEAPS:
                trash, heap_id = line.strip().split(':')
                line = stats.readline()
                trash, heap_size = line.split(':')
                line = stats.readline()
                trash, num_allocations = line.split(':')
                if int(heap_size, 0) == 0:
                    pass
                else:
                    new_heap = heap(heap_id, int(heap_size, 0), int(num_allocations, 0))
                    memory_struct.add_heap(new_heap)
                    state = parse_state.PARSE_HEAP_ELEMENTS

                line = stats.readline()

            if state == parse_state.PARSE_HEAP_ELEMENTS:
                if line.find("Heap id") == 0:
                    state = parse_state.PARSE_HEAPS
                    continue
                elif line.find("Malloc element at") == 0:
                    trash, address, status = line.rsplit(maxsplit=2)
                    line = stats.readline()
                    trash, length, trash = line.split(maxsplit=2)
                    line = stats.readline()
                    if "FREE" in status:
                        element = heap_element(int(length, 0), heap_elem_status.FREE, int(address, 0))
                    else:
                        element = heap_element(int(length, 0), heap_elem_status.BUSY, int(address, 0))
                    new_heap.add_element(element)
                line = stats.readline()

    memory_struct.associate_memzones_and_mempools()
    memory_struct.print_info()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Dumps memory stats for DPDK')
    parser.add_argument('-f', dest="stats_file", help='path to a dpdk memory stats file.', default='/tmp/spdk_mem_dump.txt')

    args = parser.parse_args()

    if not os.path.exists(args.stats_file):
        print("Error, specified stats file does not exist. Please make sure you have run the"
              "env_dpdk_get_mem_stats rpc on the spdk app you want to analyze.")
        exit(1)

    mem_info = parse_mem_stats(args.stats_file)
