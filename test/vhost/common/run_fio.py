#!/usr/bin/env python3

import os
import sys
import getopt
import subprocess
import signal
import re

fio_bin = "fio"


def show_help():
    print("""Usage: {} run_fio.py [options] [args]
    Description:
        Run FIO job file 'fio.job' on remote machines.
        NOTE: The job file must exist on remote machines on '/root/' directory.
    Args:
          [VMs] (ex. vm1_IP:vm1_port:vm1_disk1:vm_disk2,vm2_IP:vm2_port:vm2_disk1,etc...)
    Options:
        -h, --help        Show this message.
        -j, --job-file    Paths to file with FIO job configuration on remote host.
        -f, --fio-bin     Location of FIO binary on local host (Default "fio")
        -o, --out         Directory used to save generated job files and
                          files with test results
        -J, --json        Use JSON format for output
        -p, --perf-vmex   Enable aggregating statistic for VMEXITS for VMs
    """.format(os.path.split(sys.executable)[-1]))


def exec_cmd(cmd, blocking):
    # Print result to STDOUT for now, we don't have json support yet.
    p = subprocess.Popen(cmd.split(" "), stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
    if blocking is True:
        out, _ = p.communicate()
        return p.returncode, out.decode()
    return p


def save_file(path, mode, contents):
    with open(path, mode) as fh:
        fh.write(contents)
    fh.close()


def run_fio(vms, fio_cfg_fname, out_path, perf_vmex=False, json=False):
    global fio_bin
    job_name = os.path.splitext(os.path.basename(fio_cfg_fname))[0]

    # Build command for FIO
    fio_cmd = " ".join([fio_bin, "--eta=never"])
    if json:
        fio_cmd = " ".join([fio_bin, "--output-format=json"])
    for vm in vms:
        # vm[0] = IP address, vm[1] = Port number
        fio_cmd = " ".join([fio_cmd,
                            "--client={vm_ip},{vm_port}".format(vm_ip=vm[0], vm_port=vm[1]),
                            "--remote-config {cfg}".format(cfg=fio_cfg_fname)])
    print(fio_cmd)

    if perf_vmex:
        perf_dir = os.path.join(out_path, "perf_stats")
        try:
            os.mkdir(perf_dir)
        except OSError:
            pass

        # Start gathering perf statistics for host and VM guests
        perf_rec_file = os.path.join(perf_dir, "perf.data.kvm")
        perf_run_cmd = "perf kvm --host --guest " + \
                       "-o {0} stat record -a".format(perf_rec_file)
        print(perf_run_cmd)
        perf_p = exec_cmd(perf_run_cmd, blocking=False)

    # Run FIO test on VMs
    rc, out = exec_cmd(fio_cmd, blocking=True)

    # if for some reason output contains lines with "eta" - remove them
    out = re.sub(r'.+\[eta\s+\d{2}m:\d{2}s\]', '', out)

    print(out)

    if rc != 0:
        print("ERROR! While executing FIO jobs - RC: {rc}".format(rc=rc, out=out))
        sys.exit(rc)
    else:
        save_file(os.path.join(out_path, ".".join([job_name, "log"])), "w", out)

    if perf_vmex:
        # Stop gathering perf statistics and prepare some result files
        perf_p.send_signal(signal.SIGINT)
        perf_p.wait()

        perf_stat_cmd = "perf kvm --host -i {perf_rec} stat report --event vmexit"\
            .format(perf_rec=perf_rec_file)

        rc, out = exec_cmd(" ".join([perf_stat_cmd, "--event vmexit"]),
                           blocking=True)
        print("VMexit host stats:")
        print("{perf_out}".format(perf_out=out))
        save_file(os.path.join(perf_dir, "vmexit_stats_" + job_name),
                  "w", "{perf_out}".format(perf_out=out))
        try:
            os.remove(perf_rec_file)
        except OSError:
            pass


def main():
    global fio_bin

    abspath = os.path.abspath(__file__)
    dname = os.path.dirname(abspath)

    vms = []
    fio_cfg = None
    out_dir = None
    perf_vmex = False
    json = False

    try:
        opts, args = getopt.getopt(sys.argv[1:], "hJj:f:o:p",
                                   ["help", "job-file=", "fio-bin=",
                                    "out=", "perf-vmex", "json"])
    except getopt.GetoptError:
        show_help()
        sys.exit(1)

    if len(args) < 1:
        show_help()
        sys.exit(1)

    for o, a in opts:
        if o in ("-j", "--job-file"):
            fio_cfg = a
        elif o in ("-h", "--help"):
            show_help()
            sys.exit(1)
        elif o in ("-p", "--perf-vmex"):
            perf_vmex = True
        elif o in ("-o", "--out"):
            out_dir = a
        elif o in ("-f", "--fio-bin"):
            fio_bin = a
        elif o in ("-J", "--json"):
            json = True

    if fio_cfg is None:
        print("ERROR! No FIO job provided!")
        sys.exit(1)

    if out_dir is None or not os.path.exists(out_dir):
        print("ERROR! Folder {out_dir} does not exist ".format(out_dir=out_dir))
        sys.exit(1)

    # Get IP, port and fio 'filename' information from positional args
    for arg in args[0].split(","):
        _ = arg.split(":")
        ip, port, filenames = _[0], _[1], ":".join(_[2:])
        vms.append((ip, port, filenames))

    print("Running job file: {0}".format(fio_cfg))
    run_fio(vms, fio_cfg, out_dir, perf_vmex, json)


if __name__ == "__main__":
    sys.exit(main())
