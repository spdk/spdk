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

    def get_total_heap_size(self):
        size = 0
        for heap in self.heaps:
            size = size + heap.size
        return size

    def get_total_mempool_size(self):
        size = 0
        for pool in self.mempools:
            size = size + pool.get_memzone_size_sum()
        return size

    def get_total_memzone_size(self):
        size = 0
        for zone in self.memzones:
            size = size + zone.size
        return size

    def print_summary(self):
        print("DPDK memory size {} in {} heap(s)"
              .format(B_to_MiB(self.size), len(self.heaps)))
        print("{} heaps totaling size {}".format(len(self.heaps), B_to_MiB(self.get_total_heap_size())))
        for x in sorted(self.heaps, key=lambda x: x.size, reverse=True):
            x.print_summary('  ')
        print("end heaps----------")
        print("{} mempools totaling size {}".format(len(self.mempools), B_to_MiB(self.get_total_mempool_size())))
        for x in sorted(self.mempools, key=lambda x: x.get_memzone_size_sum(), reverse=True):
            x.print_summary('  ')
        print("end mempools-------")
        print("{} memzones totaling size {}".format(len(self.memzones), B_to_MiB(self.get_total_memzone_size())))
        for x in sorted(self.memzones, key=lambda x: x.size, reverse=True):
            x.print_summary('  ')
        print("end memzones-------")

    def print_heap_summary(self, heap_id):
        for heap in self.heaps:
            if heap_id == heap.id:
                heap.print_detailed_stats()
                break
        else:
            print("heap id {} is invalid. please see the summary for valid heaps.\n".format(heap_id))

    def print_mempool_summary(self, name):
        for pool in self.mempools:
            if name == pool.name:
                pool.print_detailed_stats()
                break
        else:
            print("mempool name {} is invalid. please see the summary for valid mempools.\n".format(name))

    def print_memzone_summary(self, name):
        for zone in self.memzones:
            if name == zone.name:
                zone.print_detailed_stats("")
                break
        else:
            print("memzone name {} is invalid. please see the summary for valid memzone.\n".format(name))

    def associate_heap_elements_and_memzones(self):
        for zone in self.memzones:
            for heap_obj in self.heaps:
                for element in heap_obj.busy_malloc_elements:
                    if element.check_memzone_compatibility(zone):
                        heap_obj.busy_memzone_elements.append(element)
                        heap_obj.busy_malloc_elements.remove(element)

    def associate_memzones_and_mempools(self):
        for pool in self.mempools:
            for zone in self.memzones:
                if pool.name in zone.name:
                    pool.add_memzone(zone)

        for pool in self.mempools:
            for zone in pool.memzones:
                if zone in self.memzones:
                    self.memzones.remove(zone)


class heap_elem_status(Enum):
    FREE = 0
    BUSY = 1


class heap_element:
    def __init__(self, size, status, addr):
        self.status = status
        self.size = size
        self.addr = addr
        self.memzone = None

    def print_summary(self, header):
        print("{}element at address: {} with size: {:>15}".format(header, hex(self.addr), B_to_MiB(self.size)))

    def check_memzone_compatibility(self, memzone):
        ele_fini_addr = self.addr + self.size
        memzone_fini_addr = memzone.address + memzone.size
        if (self.addr <= memzone.address and ele_fini_addr >= memzone_fini_addr):
            self.memzone = memzone
            return True
        return False


class heap:
    def __init__(self, id, size, num_allocations):
        self.id = id
        self.size = size
        self.num_allocations = num_allocations
        self.free_elements = []
        self.busy_malloc_elements = []
        self.busy_memzone_elements = []

    def add_element(self, element):
        if element.status == heap_elem_status.FREE:
            self.free_elements.append(element)
        else:
            self.busy_malloc_elements.append(element)

    def print_element_stats(self, list_to_print, list_type, header):
        print("{}list of {} elements. size: {}".format(header, list_type, B_to_MiB(self.get_element_size(list_to_print))))
        for x in sorted(list_to_print, key=lambda x: x.size, reverse=True):
            x.print_summary("{}  ".format(header))
            if x.memzone is not None:
                x.memzone.print_summary("    {}associated memzone info: ".format(header))

    def get_element_size(self, list_to_check):
        size = 0
        for element in list_to_check:
            size = size + element.size
        return size

    def print_summary(self, header):
        print("{}size: {:>15} heap id: {}".format(header, B_to_MiB(self.size), self.id))

    def print_detailed_stats(self):
        print("heap id: {} total size: {} number of busy elements: {} number of free elements: {}"
              .format(self.id, B_to_MiB(self.size), len(self.busy_malloc_elements), len(self.free_elements)))
        self.print_element_stats(self.free_elements, "free", "  ")
        self.print_element_stats(self.busy_malloc_elements, "standard malloc", "  ")
        self.print_element_stats(self.busy_memzone_elements, "memzone associated", "  ")


class mempool:
    def __init__(self, name, num_objs, num_populated_objs, obj_size):
        self.name = name
        self.num_objs = num_objs
        self.num_populated_objs = num_populated_objs
        self.obj_size = obj_size
        self.memzones = []

    def add_memzone(self, memzone):
        self.memzones.append(memzone)

    def get_memzone_size_sum(self):
        size = 0
        for zone in self.memzones:
            size = size + zone.size
        return size

    def print_summary(self, header):
        print("{}size: {:>15} name: {}"
              .format(header, B_to_MiB(self.get_memzone_size_sum()), self.name))

    def print_detailed_stats(self):
        print("size: {:>15} name: {} comprised of {} memzone(s):"
              .format(B_to_MiB(self.get_memzone_size_sum()), self.name, len(self.memzones)))
        for x in sorted(self.memzones, key=lambda x: x.size, reverse=True):
            x.print_detailed_stats("  ")


class memzone:
    def __init__(self, name, size, address):
        self.name = name
        self.size = size
        self.address = address
        self.segments = []

    def add_segment(self, segment):
        self.segments.append(segment)

    def print_summary(self, header):
        print("{}size: {:>15} name: {}".format(header,  B_to_MiB(self.size), self.name))

    def print_detailed_stats(self, header):
        self.print_summary(header)
        print("{}located at address {}".format(header, hex(self.address)))
        print("{}spanning {} segment(s):".format(header, len(self.segments)))
        for x in sorted(self.segments, key=lambda x: x.size, reverse=True):
            x.print_summary('  ')


class segment:
    def __init__(self, size, address):
        self.size = size
        self.address = address

    def print_summary(self, header):
        print("{}address: {} length: {:>15}".format(header, hex(self.address), B_to_MiB(self.size)))


class parse_state(Enum):
    PARSE_MEMORY_SIZE = 0
    PARSE_MEMZONES = 1
    PARSE_MEMZONE_SEGMENTS = 2
    PARSE_MEMPOOLS = 3
    PARSE_MEMPOOL_INFO = 4
    PARSE_HEAPS = 5
    PARSE_HEAP_ELEMENTS = 6


def B_to_MiB(raw_value):
    raw_value = raw_value / (1024.0 * 1024.0)

    return "%6f %s" % (raw_value, "MiB")


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
                    new_heap = heap(heap_id.lstrip(), int(heap_size, 0), int(num_allocations, 0))
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

    memory_struct.associate_heap_elements_and_memzones()
    memory_struct.associate_memzones_and_mempools()
    return memory_struct


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Dumps memory stats for DPDK. If no arguments are provided, it dumps a general summary.')
    parser.add_argument('-f', dest="stats_file", help='path to a dpdk memory stats file.', default='/tmp/spdk_mem_dump.txt')
    parser.add_argument('-m', '--heap', dest="heap", help='Print detailed information about the given heap.', default=None)
    parser.add_argument('-p', '--mempool', dest="mempool", help='Print detailed information about the given mempool.', default=None)
    parser.add_argument('-z', '--memzone', dest="memzone", help='Print detailed information about the given memzone.', default=None)

    args = parser.parse_args()

    if not os.path.exists(args.stats_file):
        print("Error, specified stats file does not exist. Please make sure you have run the"
              "env_dpdk_get_mem_stats rpc on the spdk app you want to analyze.")
        exit(1)

    mem_info = parse_mem_stats(args.stats_file)

    summary = True
    if args.heap is not None:
        mem_info.print_heap_summary(args.heap)
        summary = False
    if args.mempool is not None:
        mem_info.print_mempool_summary(args.mempool)
        summary = False
    if args.memzone is not None:
        mem_info.print_memzone_summary(args.memzone)
        summary = False

    if summary:
        mem_info.print_summary()
