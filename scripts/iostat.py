#!/usr/bin/env python3

import logging
import sys
import json
import argparse
import os
import time
from rpc.client import print_dict, print_json, JSONRPCException
import rpc


SPDK_CPU_STAT = "/proc/stat"
SPDK_UPTIME = "/proc/uptime"

SPDK_CPU_STAT_HEAD = ['cpu_stat:', 'user_stat', 'nice_stat',
                      'system_stat', 'iowait_stat', 'steal_stat', 'idle_stat']
SPDK_BDEV_KB_STAT_HEAD = ['Device', 'tps', 'KB_read/s',
                          'KB_wrtn/s', 'KB_dscd/s', 'KB_read', 'KB_wrtn', 'KB_dscd']
SPDK_BDEV_MB_STAT_HEAD = ['Device', 'tps', 'MB_read/s',
                          'MB_wrtn/s', 'MB_dscd/s', 'MB_read', 'MB_wrtn', 'MB_dscd']

SPDK_MAX_SECTORS = 0xffffffff


class bdev_stat:

    def __init__(self, dictionary):
        if dictionary is None:
            return
        for k, value in dictionary.items():
            if k == 'name':
                self.bdev_name = value
            elif k == 'bytes_read':
                self.rd_sectors = int(value) >> 9
            elif k == 'bytes_written':
                self.wr_sectors = int(value) >> 9
            elif k == 'bytes_unmapped':
                self.dc_sectors = int(value) >> 9
            elif k == 'num_read_ops':
                self.rd_ios = value
            elif k == 'num_write_ops':
                self.wr_ios = value
            elif k == 'num_unmap_ops':
                self.dc_ios = value
            elif k == 'read_latency_ticks':
                self.rd_ticks = value
            elif k == 'write_latency_ticks':
                self.wr_ticks = value
            elif k == 'unmap_latency_ticks':
                self.dc_ticks = value
            elif k == 'queue_depth':
                self.ios_pgr = value
            elif k == 'io_time':
                self.tot_ticks = value
            elif k == 'weighted_io_time':
                self.rq_ticks = value

        self.rd_merges = 0
        self.wr_merges = 0
        self.dc_merges = 0
        self.upt = 0.0

    def __getattr__(self, name):
        return 0


def uptime():
    with open(SPDK_UPTIME, 'r') as f:
        return float(f.readline().split()[0])


def _cpu_percent_cal_diff(n, m, p):
    p = round((int(n) - int(m)) / int(p) * 100, 2)
    return str(p) + "%"


def _cpu_percent_cal(n, p):
    p = round(int(n) / int(p) * 100, 2)
    return str(p) + "%"


def _percent_cal_diff(n, m, p, s):
    p = round((n - m) / p, s)
    return p


def _percent_cal(n, p, s):
    p = round(n / p, s)
    return p


def _stat_format(data, header, leave_first=False):
    list_size = len(data)
    header_len = len(header)

    if list_size == 0:
        raise AssertionError
    list_len = len(data[0])

    for ll in data:
        if len(ll) != list_len:
            raise AssertionError

    if (leave_first and list_len + 1 != header_len) or \
            (not leave_first and list_len != header_len):
        raise AssertionError

    longg = [0 for i in range(header_len)]

    for i in range(0, list_len):
        if leave_first and i == 0:
            longg[i] = len(header[i + 1])

        data_len = 0
        for x in data:
            data_len = max(data_len, len(x[i]))
        index = i + 1 if leave_first else i
        longg[index] = max(len(header[index]), data_len)

    format = '  '.join('%%-%ss' % longg[i] for i in range(0, header_len))
    print(format % tuple(header))
    if leave_first:
        print('\n'.join(format % ('', *tuple(ll)) for ll in data))
    else:
        print('\n'.join(format % tuple(ll) for ll in data))

    print()


def read_cpu_stat(last_cpu_info, cpu_info):
    jiffies = 0
    for i in range(0, 7):
        jiffies += int(cpu_info[i]) - \
            (int(last_cpu_info[i]) if last_cpu_info else 0)

    if last_cpu_info:
        info_stat = [
            _cpu_percent_cal_diff(cpu_info[0], last_cpu_info[0], jiffies),
            _cpu_percent_cal_diff(cpu_info[1], last_cpu_info[1], jiffies),
            _cpu_percent_cal_diff(int(cpu_info[2]) + int(cpu_info[5])
                                  + int(cpu_info[6]), int(last_cpu_info[2])
                                  + int(last_cpu_info[5]) + int(last_cpu_info[6]), jiffies),
            _cpu_percent_cal_diff(cpu_info[4], last_cpu_info[4], jiffies),
            _cpu_percent_cal_diff(cpu_info[7], last_cpu_info[7], jiffies),
            _cpu_percent_cal_diff(cpu_info[3], last_cpu_info[3], jiffies),
        ]
    else:
        info_stat = [
            _cpu_percent_cal(cpu_info[0], jiffies),
            _cpu_percent_cal(cpu_info[1], jiffies),
            _cpu_percent_cal(int(cpu_info[2]) + int(cpu_info[5])
                             + int(cpu_info[6]), jiffies),
            _cpu_percent_cal(cpu_info[4], jiffies),
            _cpu_percent_cal(cpu_info[7], jiffies),
            _cpu_percent_cal(cpu_info[3], jiffies),
        ]

    _stat_format([info_stat], SPDK_CPU_STAT_HEAD, True)


def check_positive(value):
    v = int(value)
    if v <= 0:
        raise argparse.ArgumentTypeError("%s should be positive int value" % v)
    return v


def get_cpu_stat(flags=0):
    cpu_file = open(SPDK_CPU_STAT, "r")
    cpu_dump_info = []
    if not cpu_file:
        return
    line = cpu_file.readline()
    while line:
        line = line.strip()
        if "cpu " in line and flags == 0:
            cpu_dump_info = [data for data in line[5:].split(' ')]
            break
        elif ("cpu" + str(flags)) in line and flags != 0:
            cpu_dump_info = [data for data in line.split(' ')]
            if len(cpu_dump_info) != 0:
                del cpu_dump_info[0]
            break

        line = cpu_file.readline()

    cpu_file.close()
    return cpu_dump_info


def read_bdev_stat(last_stat, stat, mb):
    upt_cur = uptime()
    info_stats = []
    unit = 2048 if mb else 2

    bdev_stats = []
    if last_stat:
        for bdev in stat['bdevs']:
            _stat = bdev_stat(bdev)
            _stat.upt = upt_cur
            bdev_stats.append(_stat)
            _last_stat = None
            for last_bdev in last_stat:
                if (_stat.bdev_name == last_bdev.bdev_name):
                    _last_stat = last_bdev
                    break

            # get the interval time
            upt = _stat.upt - _last_stat.upt

            rd_sec = _stat.rd_sectors - _last_stat.rd_sectors
            if (_stat.rd_sectors < _last_stat.rd_sectors) and (_last_stat.rd_sectors <= SPDK_MAX_SECTORS):
                rd_sec &= SPDK_MAX_SECTORS

            wr_sec = _stat.wr_sectors - _last_stat.wr_sectors
            if (_stat.wr_sectors < _last_stat.wr_sectors) and (_last_stat.wr_sectors <= SPDK_MAX_SECTORS):
                wr_sec &= SPDK_MAX_SECTORS

            dc_sec = _stat.dc_sectors - _last_stat.dc_sectors
            if (_stat.dc_sectors < _last_stat.dc_sectors) and (_last_stat.dc_sectors <= SPDK_MAX_SECTORS):
                dc_sec &= SPDK_MAX_SECTORS

            tps = _percent_cal_diff(_stat.rd_ios + _stat.dc_ios + _stat.wr_ios,
                                    _last_stat.rd_ios + _last_stat.dc_ios + _last_stat.wr_ios,
                                    upt, 2)

            info_stat = [
                _stat.bdev_name,
                str(tps),
                str(round(_percent_cal_diff(_stat.rd_sectors,
                                            _last_stat.rd_sectors, upt, 2) / unit, 2)),
                str(round(_percent_cal_diff(_stat.wr_sectors,
                                            _last_stat.wr_sectors, upt, 2) / unit, 2)),
                str(round(_percent_cal_diff(_stat.dc_sectors,
                                            _last_stat.dc_sectors, upt, 2) / unit, 2)),
                str(round(rd_sec / unit, 2)),
                str(round(wr_sec / unit, 2)),
                str(round(dc_sec / unit, 2))
            ]
            info_stats.append(info_stat)
    else:
        for bdev in stat['bdevs']:
            _stat = bdev_stat(bdev)
            _stat.upt = upt_cur
            bdev_stats.append(_stat)
            tps = _percent_cal(
                _stat.rd_ios + _stat.dc_ios + _stat.wr_ios, upt_cur, 2)
            info_stat = [
                _stat.bdev_name,
                str(tps),
                str(round(_percent_cal(_stat.rd_sectors, upt_cur, 2) / unit, 2)),
                str(round(_percent_cal(_stat.wr_sectors, upt_cur, 2) / unit, 2)),
                str(round(_percent_cal(_stat.dc_sectors, upt_cur, 2) / unit, 2)),
                str(round(_stat.rd_sectors / unit, 2)),
                str(round(_stat.dc_sectors / unit, 2)),
                str(round(_stat.dc_sectors / unit, 2))
            ]
            info_stats.append(info_stat)

    _stat_format(
        info_stats, SPDK_BDEV_MB_STAT_HEAD if mb else SPDK_BDEV_KB_STAT_HEAD)
    return bdev_stats


def get_bdev_stat(client, name):
    return rpc.bdev.bdev_get_iostat(args.client, name=args.name)


def io_stat_display(args, interval, times, cpu_info, stat):
    if args.cpu_stat:
        _cpu_info = get_cpu_stat()
        read_cpu_stat(cpu_info, _cpu_info)
        return _cpu_info, None

    if args.nvme_stat:
        _stat = get_bdev_stat(args.client, args.name)
        bdev_stats = read_bdev_stat(stat, _stat, args.mb_display)
        return None, bdev_stats

    _cpu_info = get_cpu_stat()
    read_cpu_stat(cpu_info, _cpu_info)

    _stat = get_bdev_stat(args.client, args.name)
    bdev_stats = read_bdev_stat(stat, _stat, args.mb_display)
    return _cpu_info, bdev_stats


def io_stat_display_loop(args):
    interval = args.interval
    times = args.times
    args.client = rpc.client.JSONRPCClient(
        args.server_addr, args.port, args.timeout, log_level=getattr(logging, args.verbose.upper()))

    last_cpu_stat = None
    bdev_stats = None

    if interval == 0 and times == 0:
        io_stat_display(args, interval, times, last_cpu_stat, bdev_stats)
        return

    cur = 0
    while (True):  # for do...while
        last_cpu_stat, bdev_stats = io_stat_display(
            args, interval, times, last_cpu_stat, bdev_stats)
        cur += 1
        if cur >= times:
            break

        time.sleep(interval)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='SPDK iostats command line interface')

    parser.add_argument('-c', '--cpu-status', dest='cpu_stat',
                        action='store_true', help="Only display cpu status",
                        required=False, default=False)

    parser.add_argument('-d', '--nvme-status', dest='nvme_stat',
                        action='store_true', help="Only display nvme status",
                        required=False, default=False)

    parser.add_argument('-k', '--kb-display', dest='kb_display',
                        action='store_true', help="Only display nvme status",
                        required=False, default=False)

    parser.add_argument('-m', '--mb-display', dest='mb_display',
                        action='store_true', help="Only display nvme status",
                        required=False, default=False)

    parser.add_argument('-i', '--interval', dest='interval',
                        type=check_positive, help='Time of interval',
                        required=False, default=0)

    parser.add_argument('-t', '--times', dest='times',
                        type=check_positive, help='Times of shows',
                        required=False, default=0)

    parser.add_argument('-s', "--server", dest='server_addr',
                        help='RPC domain socket path or IP address', default='/var/tmp/spdk.sock')

    parser.add_argument('-p', "--port", dest='port',
                        help='RPC port number (if server_addr is IP address)',
                        default=5260, type=int)

    parser.add_argument('-b', '--name', dest='name',
                        help="Name of the Blockdev. Example: Nvme0n1", required=False)

    parser.add_argument('-o', '--timeout', dest='timeout',
                        help='Timeout as a floating point number expressed in seconds waiting for response. Default: 60.0',
                        default=60.0, type=float)

    parser.add_argument('-v', dest='verbose', action='store_const', const="INFO",
                        help='Set verbose mode to INFO', default="ERROR")

    args = parser.parse_args()
    if ((args.interval == 0 and args.times != 0) or
            (args.interval != 0 and args.times == 0)):
        raise argparse.ArgumentTypeError(
            "interval and times should be greater than 0 at the same time")

    if args.kb_display and args.mb_display:
        parser.print_help()
        exit()

    io_stat_display_loop(args)
