#!/usr/bin/env python3

import logging
import sys
import argparse
import time
import rpc


SPDK_CPU_STAT = "/proc/stat"
SPDK_UPTIME = "/proc/uptime"

SPDK_CPU_STAT_HEAD = ['cpu_stat:', 'user_stat', 'nice_stat',
                      'system_stat', 'iowait_stat', 'steal_stat', 'idle_stat']
SPDK_BDEV_KB_STAT_HEAD = ['Device', 'tps', 'KB_read/s',
                          'KB_wrtn/s', 'KB_dscd/s', 'KB_read', 'KB_wrtn', 'KB_dscd']
SPDK_BDEV_MB_STAT_HEAD = ['Device', 'tps', 'MB_read/s',
                          'MB_wrtn/s', 'MB_dscd/s', 'MB_read', 'MB_wrtn', 'MB_dscd']
SPDK_BDEV_EXT_STAT_HEAD = ['qu-sz', 'aqu-sz', 'wareq-sz', 'rareq-sz', 'w_await(us)', 'r_await(us)', 'util']


SPDK_MAX_SECTORS = 0xffffffff


class BdevStat:

    def __init__(self, dictionary):
        if dictionary is None:
            return
        self.qd_period = 0
        for k, value in dictionary.items():
            if k == 'name':
                self.bdev_name = value
            elif k == 'bytes_read':
                self.rd_sectors = value >> 9
            elif k == 'bytes_written':
                self.wr_sectors = value >> 9
            elif k == 'bytes_unmapped':
                self.dc_sectors = value >> 9
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
            elif k == 'queue_depth_polling_period':
                self.qd_period = value
            elif k == 'queue_depth':
                self.queue_depth = value
            elif k == 'io_time':
                self.io_time = value
            elif k == 'weighted_io_time':
                self.weighted_io_time = value
        self.upt = 0.0

    def __getattr__(self, name):
        return 0


def uptime():
    with open(SPDK_UPTIME, 'r') as f:
        return float(f.readline().split()[0])


def _stat_format(data, header, leave_first=False):
    list_size = len(data)
    header_len = len(header)

    if list_size == 0:
        raise AssertionError
    list_len = len(data[0])

    for ll in data:
        if len(ll) != list_len:
            raise AssertionError
        for i, r in enumerate(ll):
            ll[i] = str(r)

    if (leave_first and list_len + 1 != header_len) or \
            (not leave_first and list_len != header_len):
        raise AssertionError

    item_sizes = [0 for i in range(header_len)]

    for i in range(0, list_len):
        if leave_first and i == 0:
            item_sizes[i] = len(header[i + 1])

        data_len = 0
        for x in data:
            data_len = max(data_len, len(x[i]))
        index = i + 1 if leave_first else i
        item_sizes[index] = max(len(header[index]), data_len)

    _format = '  '.join('%%-%ss' % item_sizes[i] for i in range(0, header_len))
    print(_format % tuple(header))
    if leave_first:
        print('\n'.join(_format % ('', *tuple(ll)) for ll in data))
    else:
        print('\n'.join(_format % tuple(ll) for ll in data))

    print()
    sys.stdout.flush()


def read_cpu_stat(last_cpu_info, cpu_info):
    jiffies = 0
    for i in range(0, 7):
        jiffies += cpu_info[i] - \
            (last_cpu_info[i] if last_cpu_info else 0)

    if last_cpu_info:
        info_stat = [
            "{:.2%}".format((cpu_info[0] - last_cpu_info[0]) / jiffies),
            "{:.2%}".format((cpu_info[1] - last_cpu_info[1]) / jiffies),
            "{:.2%}".format(((cpu_info[2] + cpu_info[5] + cpu_info[6]) -
                             (last_cpu_info[2] + last_cpu_info[5] + last_cpu_info[6])) / jiffies),
            "{:.2%}".format((cpu_info[4] - last_cpu_info[4]) / jiffies),
            "{:.2%}".format((cpu_info[7] - last_cpu_info[7]) / jiffies),
            "{:.2%}".format((cpu_info[3] - last_cpu_info[3]) / jiffies),
        ]
    else:
        info_stat = [
            "{:.2%}".format(cpu_info[0] / jiffies),
            "{:.2%}".format(cpu_info[1] / jiffies),
            "{:.2%}".format((cpu_info[2] + cpu_info[5]
                             + cpu_info[6]) / jiffies),
            "{:.2%}".format(cpu_info[4] / jiffies),
            "{:.2%}".format(cpu_info[7] / jiffies),
            "{:.2%}".format(cpu_info[3] / jiffies),
        ]

    _stat_format([info_stat], SPDK_CPU_STAT_HEAD, True)


def check_positive(value):
    v = int(value)
    if v <= 0:
        raise argparse.ArgumentTypeError("%s should be positive int value" % v)
    return v


def get_cpu_stat():
    with open(SPDK_CPU_STAT, "r") as cpu_file:
        cpu_dump_info = []
        line = cpu_file.readline()
        while line:
            line = line.strip()
            if "cpu " in line:
                cpu_dump_info = [int(data) for data in line[5:].split(' ')]
                break

            line = cpu_file.readline()
    return cpu_dump_info


def read_bdev_stat(last_stat, stat, mb, use_upt, ext_info):
    if use_upt:
        upt_cur = uptime()
    else:
        upt_cur = stat['ticks']

    upt_rate = stat['tick_rate']

    info_stats = []
    unit = 2048 if mb else 2

    bdev_stats = []
    if last_stat:
        for bdev in stat['bdevs']:
            _stat = BdevStat(bdev)
            _stat.upt = upt_cur
            bdev_stats.append(_stat)
            _last_stat = None
            for last_bdev in last_stat:
                if (_stat.bdev_name == last_bdev.bdev_name):
                    _last_stat = last_bdev
                    break

            # get the interval time
            if use_upt:
                upt = _stat.upt - _last_stat.upt
            else:
                upt = (_stat.upt - _last_stat.upt) / upt_rate

            rd_sec = _stat.rd_sectors - _last_stat.rd_sectors
            if (_stat.rd_sectors < _last_stat.rd_sectors) and (_last_stat.rd_sectors <= SPDK_MAX_SECTORS):
                rd_sec &= SPDK_MAX_SECTORS

            wr_sec = _stat.wr_sectors - _last_stat.wr_sectors
            if (_stat.wr_sectors < _last_stat.wr_sectors) and (_last_stat.wr_sectors <= SPDK_MAX_SECTORS):
                wr_sec &= SPDK_MAX_SECTORS

            dc_sec = _stat.dc_sectors - _last_stat.dc_sectors
            if (_stat.dc_sectors < _last_stat.dc_sectors) and (_last_stat.dc_sectors <= SPDK_MAX_SECTORS):
                dc_sec &= SPDK_MAX_SECTORS

            tps = ((_stat.rd_ios + _stat.dc_ios + _stat.wr_ios) -
                   (_last_stat.rd_ios + _last_stat.dc_ios + _last_stat.wr_ios)) / upt

            info_stat = [
                _stat.bdev_name,
                "{:.2f}".format(tps),
                "{:.2f}".format(
                    (_stat.rd_sectors - _last_stat.rd_sectors) / upt / unit),
                "{:.2f}".format(
                    (_stat.wr_sectors - _last_stat.wr_sectors) / upt / unit),
                "{:.2f}".format(
                    (_stat.dc_sectors - _last_stat.dc_sectors) / upt / unit),
                "{:.2f}".format(rd_sec / unit),
                "{:.2f}".format(wr_sec / unit),
                "{:.2f}".format(dc_sec / unit),
            ]
            if ext_info:
                if _stat.qd_period > 0:
                    tot_sampling_time = upt * 1000000 / _stat.qd_period
                    busy_times = (_stat.io_time - _last_stat.io_time) / _stat.qd_period

                    wr_ios = _stat.wr_ios - _last_stat.wr_ios
                    rd_ios = _stat.rd_ios - _last_stat.rd_ios
                    if busy_times != 0:
                        aqu_sz = (_stat.weighted_io_time - _last_stat.weighted_io_time) / _stat.qd_period / busy_times
                    else:
                        aqu_sz = 0

                    if wr_ios != 0:
                        wareq_sz = wr_sec / wr_ios
                        w_await = (_stat.wr_ticks * 1000000 / upt_rate -
                                   _last_stat.wr_ticks * 1000000 / upt_rate) / wr_ios
                    else:
                        wareq_sz = 0
                        w_await = 0

                    if rd_ios != 0:
                        rareq_sz = rd_sec / rd_ios
                        r_await = (_stat.rd_ticks * 1000000 / upt_rate -
                                   _last_stat.rd_ticks * 1000000 / upt_rate) / rd_ios
                    else:
                        rareq_sz = 0
                        r_await = 0

                    util = busy_times / tot_sampling_time

                    info_stat += [
                        "{:.2f}".format(_stat.queue_depth),
                        "{:.2f}".format(aqu_sz),
                        "{:.2f}".format(wareq_sz),
                        "{:.2f}".format(rareq_sz),
                        "{:.2f}".format(w_await),
                        "{:.2f}".format(r_await),
                        "{:.2f}".format(util),
                    ]
                else:
                    info_stat += ["N/A"] * len(SPDK_BDEV_EXT_STAT_HEAD)

            info_stats.append(info_stat)
    else:
        for bdev in stat['bdevs']:
            _stat = BdevStat(bdev)
            _stat.upt = upt_cur
            bdev_stats.append(_stat)

            if use_upt:
                upt = _stat.upt
            else:
                upt = _stat.upt / upt_rate

            tps = (_stat.rd_ios + _stat.dc_ios + _stat.wr_ios) / upt
            info_stat = [
                _stat.bdev_name,
                "{:.2f}".format(tps),
                "{:.2f}".format(_stat.rd_sectors / upt / unit),
                "{:.2f}".format(_stat.wr_sectors / upt / unit),
                "{:.2f}".format(_stat.dc_sectors / upt / unit),
                "{:.2f}".format(_stat.rd_sectors / unit),
                "{:.2f}".format(_stat.wr_sectors / unit),
                "{:.2f}".format(_stat.dc_sectors / unit),
            ]

            # add extended statistics
            if ext_info:
                if _stat.qd_period > 0:
                    tot_sampling_time = upt * 1000000 / _stat.qd_period
                    busy_times = _stat.io_time / _stat.qd_period
                    if busy_times != 0:
                        aqu_sz = _stat.weighted_io_time / _stat.qd_period / busy_times
                    else:
                        aqu_sz = 0

                    if _stat.wr_ios != 0:
                        wareq_sz = _stat.wr_sectors / _stat.wr_ios
                        w_await = _stat.wr_ticks * 1000000 / upt_rate / _stat.wr_ios
                    else:
                        wareq_sz = 0
                        w_await = 0

                    if _stat.rd_ios != 0:
                        rareq_sz = _stat.rd_sectors / _stat.rd_ios
                        r_await = _stat.rd_ticks * 1000000 / upt_rate / _stat.rd_ios
                    else:
                        rareq_sz = 0
                        r_await = 0

                    util = busy_times / tot_sampling_time

                    info_stat += [
                        "{:.2f}".format(_stat.queue_depth),
                        "{:.2f}".format(aqu_sz),
                        "{:.2f}".format(wareq_sz),
                        "{:.2f}".format(rareq_sz),
                        "{:.2f}".format(w_await),
                        "{:.2f}".format(r_await),
                        "{:.2f}".format(util),
                    ]
                else:
                    info_stat += ["N/A"] * len(SPDK_BDEV_EXT_STAT_HEAD)

            info_stats.append(info_stat)

    head = []
    head += SPDK_BDEV_MB_STAT_HEAD if mb else SPDK_BDEV_KB_STAT_HEAD
    if ext_info:
        head += SPDK_BDEV_EXT_STAT_HEAD

    _stat_format(info_stats, head)
    return bdev_stats


def get_bdev_stat(client, name):
    return rpc.bdev.bdev_get_iostat(client, name=name)


def io_stat_display(args, cpu_info, stat):
    if args.cpu_stat and not args.bdev_stat:
        _cpu_info = get_cpu_stat()
        read_cpu_stat(cpu_info, _cpu_info)
        return _cpu_info, None

    if args.bdev_stat and not args.cpu_stat:
        _stat = get_bdev_stat(args.client, args.name)
        bdev_stats = read_bdev_stat(
            stat, _stat, args.mb_display, args.use_uptime, args.extended_display)
        return None, bdev_stats

    _cpu_info = get_cpu_stat()
    read_cpu_stat(cpu_info, _cpu_info)

    _stat = get_bdev_stat(args.client, args.name)
    bdev_stats = read_bdev_stat(stat, _stat, args.mb_display, args.use_uptime, args.extended_display)
    return _cpu_info, bdev_stats


def io_stat_display_loop(args):
    interval = args.interval
    time_in_second = args.time_in_second
    args.client = rpc.client.JSONRPCClient(
        args.server_addr, args.port, args.timeout, log_level=getattr(logging, args.verbose.upper()))

    last_cpu_stat = None
    bdev_stats = None

    cur = 0
    while True:
        last_cpu_stat, bdev_stats = io_stat_display(
            args, last_cpu_stat, bdev_stats)

        time.sleep(interval)
        cur += interval
        if cur >= time_in_second:
            break


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='SPDK iostats command line interface')

    parser.add_argument('-c', '--cpu-status', dest='cpu_stat',
                        action='store_true', help="Only display cpu status",
                        required=False, default=False)

    parser.add_argument('-d', '--bdev-status', dest='bdev_stat',
                        action='store_true', help="Only display Blockdev io stats",
                        required=False, default=False)

    parser.add_argument('-k', '--kb-display', dest='kb_display',
                        action='store_true', help="Display drive stats in KiB",
                        required=False, default=False)

    parser.add_argument('-m', '--mb-display', dest='mb_display',
                        action='store_true', help="Display drive stats in MiB",
                        required=False, default=False)

    parser.add_argument('-u', '--use-uptime', dest='use_uptime',
                        action='store_true', help='Use uptime or spdk ticks(default) as \
                        the interval variable to calculate iostat changes.',
                        required=False, default=False)

    parser.add_argument('-i', '--interval', dest='interval',
                        type=check_positive, help='Time interval (in seconds) on which \
                        to poll I/O stats. Used in conjunction with -t',
                        required=False, default=0)

    parser.add_argument('-t', '--time', dest='time_in_second',
                        type=check_positive, help='The number of second to display stats \
                        before returning. Used in conjunction with -i',
                        required=False, default=0)

    parser.add_argument('-s', "--server", dest='server_addr',
                        help='RPC domain socket path or IP address',
                        default='/var/tmp/spdk.sock')

    parser.add_argument('-p', "--port", dest='port',
                        help='RPC port number (if server_addr is IP address)',
                        default=4420, type=int)

    parser.add_argument('-b', '--name', dest='name',
                        help="Name of the Blockdev. Example: Nvme0n1", required=False)

    parser.add_argument('-o', '--timeout', dest='timeout',
                        help='Timeout as a floating point number expressed in seconds \
                        waiting for response. Default: 60.0',
                        default=60.0, type=float)

    parser.add_argument('-v', dest='verbose', action='store_const', const="INFO",
                        help='Set verbose mode to INFO', default="ERROR")

    parser.add_argument('-x', '--extended', dest='extended_display',
                        action='store_true', help="Display extended statistics.",
                        required=False, default=False)

    args = parser.parse_args()
    if ((args.interval == 0 and args.time_in_second != 0) or
            (args.interval != 0 and args.time_in_second == 0)):
        raise argparse.ArgumentTypeError(
            "interval and time_in_second should be greater than 0 at the same time")

    if args.kb_display and args.mb_display:
        parser.print_help()
        exit()

    io_stat_display_loop(args)
