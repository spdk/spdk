#!/usr/bin/env python

import os
import sys
import getopt
import subprocess
import signal
import re

fio_bin = "fio"


def show_help():
    print("""Usage: python run_fio.py [options] [args]
    Args:
          [VMs] (ex. vm1_IP:vm1_port:vm1_disk1:vm_disk2,vm2_IP:vm2_port:vm2_disk1,etc...)
    Options:
        -h, --help        Show this message.
        -j, --job-files   Paths to files with custom FIO jobs configuration.
        -f, --fio-bin     Location of FIO binary (Default "fio")
        -o, --out         Directory used to save generated job files and
                          files with test results (Default: same dir where
                          this script is located)
        -p, --perf-vmex   Enable aggregating statistic for VMEXITS for VMs
    """)


def exec_cmd(cmd, blocking):
    # Print result to STDOUT for now, we don't have json support yet.
    p = subprocess.Popen(cmd.split(" "), stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
    if blocking is True:
        out, _ = p.communicate()
        return p.returncode, out
    return p


def save_file(path, mode, contents):
    with open(path, mode) as fh:
        fh.write(contents)
    fh.close()


def run_fio(vms, fio_cfg_fname, out_path, perf_vmex=False):
        global fio_bin
        fio_cfg_prefix = fio_cfg_fname.split(".")[0]

        # Build command for FIO
        fio_cmd = " ".join([fio_bin, "--eta=never"])
        for vm in vms:
            # vm[0] = IP address, vm[1] = Port number
            fio_cmd = " ".join([fio_cmd,
                                "--client={vm_ip},{vm_port}".format(vm_ip=vm[0], vm_port=vm[1]),
                                "--remote-config /root/{cfg}".format(cfg=fio_cfg_fname)])
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

        if rc != 0:
            print("ERROR! While executing FIO jobs - RC: {rc}, Err message: {out}".format(rc=rc, out=out))
            sys.exit(rc)
        else:
            print(out)
            save_file(os.path.join(out_path, ".".join([fio_cfg_prefix, "log"])), "w", out)

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
            save_file(os.path.join(perf_dir, "vmexit_stats_" + fio_cfg_prefix),
                      "w", "{perf_out}".format(perf_out=out))
            try:
                os.remove(perf_rec_file)
            except OSError:
                pass


def main():
    global fio_bin

    abspath = os.path.abspath(__file__)
    dname = os.path.dirname(abspath)
    os.chdir(os.path.join(dname, "../../.."))

    vms = []
    fio_cfgs = []
    perf_vmex = False
    out_dir = os.path.join(os.getcwd(), "fio_results")

    try:
        opts, args = getopt.getopt(sys.argv[1:], "hj:f:o:p",
                                   ["help", "job-file=", "fio-bin=",
                                    "out=", "perf-vmex"])
    except getopt.GetoptError:
        show_help()
        sys.exit(1)

    for o, a in opts:
        if o in ("-j", "--job-file"):
            fio_cfgs = a.split(",")
        elif o in ("-h", "--help"):
            show_help()
            sys.exit(1)
        elif o in ("-p", "--perf-vmex"):
            perf_vmex = True
        elif o in ("-o", "--out"):
            out_dir = os.path.join(a, "fio_results")
        elif o in ("-f", "--fio-bin"):
            fio_bin = a

    if len(fio_cfgs) < 1:
        print("ERROR! No FIO jobs provided!")
        sys.exit(1)

    if len(args) < 1:
        show_help()
        sys.exit(1)
    else:
        # Get IP, port and fio 'filename' information from positional args
        for arg in args[0].split(","):
            _ = arg.split(":")
            ip, port, filenames = _[0], _[1], ":".join(_[2:])
            vms.append((ip, port, filenames))

    if not os.path.exists(out_dir):
        os.mkdir(out_dir)

    for fio_cfg in fio_cfgs:
        fio_cfg_fname = os.path.basename(fio_cfg)
        print("Running job file: {0}".format(fio_cfg_fname))

        for i, vm in enumerate(vms):
            # VM - tuple of IP / Port / Filename for VM to run test
            print("Preparing VM {0} - {1} for FIO job".format(i, vm[0]))

            exec_cmd("./test/vhost/fiotest/vm_ssh.sh {vm_num} sh -c 'rm {cfg}'"
                     .format(vm_num=i, cfg=fio_cfg_fname), blocking=True)

            # Copy FIO config to VM
            with open(fio_cfg, "r") as fio_cfg_fh:
                for line in fio_cfg_fh.readlines():
                    if "filename" in line:
                        line = "filename=" + vm[2]
                    out = exec_cmd("./test/vhost/fiotest/vm_ssh.sh {vm_num} sh -c 'echo {line} >> {cfg}'"
                                   .format(vm_num=i, line=line.strip(), cfg=fio_cfg_fname), blocking=True)
                    if out[0] != 0:
                        print("ERROR! While copying FIO job config file to VM {vm_num} - {vm_ip}"
                              .format(vm_num=1, vm_ip=vm[0]))
                        sys.exit(1)

        run_fio(vms, fio_cfg_fname, out_dir, perf_vmex)

if __name__ == "__main__":
    sys.exit(main())
