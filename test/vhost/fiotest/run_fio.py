#!/usr/bin/env python

import os
import sys
import getopt
import subprocess
import itertools
import datetime
import signal
import re

fio_bin = "fio"
perf_vmex = False

fio_template = """
[global]
ioengine=%(ioengine)s
size=%(size)s
filename=%(filename)s
numjobs=%(numjobs)s
bs=%(blocksize)s
iodepth=%(iodepth)s
direct=%(direct)s
rw=%(testtype)s
group_reporting
thread
%(verify)s

[nvme-host]
"""


def show_help(fio_args_dict):
    print("""Usage: python run_fio.py [options] [args]
    Args:
          [VMs] (ex. vm1_IP:vm1_port,vm2_IP:vm2_port,etc...)
          [fio filename arg], ex. /dev/sda)
    Options:
        -h, --help        Show this message.
        -j, --job-files   Paths to files with custom FIO jobs configuration.
        -F, --fio-bin     Location of FIO binary (Default "fio")
        -s, --size        Size of IO for job. Will be distributed among
                          number of numjobs (Default: %(size)s)
        -t, --testtype    Type of FIO test (Default: %(testtype)s)
        -b, --blocksize   Blocksize for FIO test (Default: %(blocksize)s)
        -i, --iodepth     IO depth for FIO test (Default: %(iodepth)s)
        -I, --ioengine    Type of FIO ioengine to use (Default: %(ioengine)s)
        -n, --numjobs     Number of threads for job (Default: %(numjobs)s)
        -D, --direct      Use non-buffered IO? (Default: %(direct)s)
        -v, --verify      Verify after writing to file (Default: %(verify)s)
        -o, --out         Directory used to save generated job files and
                          files with test results (Default: same dir where
                          this script is located)
        -p, --perf-vmex   Enable aggregating statistic for VMEXITS
    """ % fio_args_dict)


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


def prep_fio_cfg_file(out_dir, fio_cfg, vm_nb):
    job_file = os.path.join(out_dir, "fio_job_vm{0}".format(vm_nb))
    print "file {0} written".format(job_file)
    save_file(job_file, "w", fio_cfg)
    return job_file


def calc_size(size, numjobs):
    return str(int(filter(lambda x: x.isdigit(), size)) / int(numjobs)) + \
        filter(lambda x: x.isalpha(), size)


def cfg_product(fio_args_dict):
    return (dict(zip(fio_args_dict, x)) for
            x in itertools.product(*fio_args_dict.itervalues()))


def run_fio(vms, fio_cfg_file, out_path):

        global perf_vmex
        # Prepare command template for FIO
        fio_cmd = fio_bin
        fio_cmd = " ".join([fio_cmd, "--eta=never"])
        print fio_cfg_file
        fio_cfg_name = (os.path.basename(fio_cfg_file)).split(".")[0]
        for i, vm in enumerate(vms):
            print("Starting thread {0} for VM: {1}".format(i, vm))

            # vm[0] = IP address, vm[1] = Port number
            fio_cmd = " ".join([fio_cmd,
                                "--client={0},{1}".format(vm[0], vm[1])])
            fio_cmd = " ".join([fio_cmd,
                                "--remote-config /root/fio.job{0}".format(i)])

        print fio_cmd

        if perf_vmex:
            # Start gathering perf statistics for host and VM guests
            perf_rec_file = os.path.join(out_path, "perf.data.kvm")
            perf_run_cmd = "perf kvm --host --guest " + \
                           "-o {0} stat record -a".format(perf_rec_file)
            print perf_run_cmd
            perf_p = exec_cmd(perf_run_cmd, blocking=False)

        # Run FIO test on VMs
        rc, out = exec_cmd(fio_cmd, blocking=True)

        # if for some reason output contains lines with "eta" - remove them
        out = re.sub(r'.+\[eta\s+\d{2}m\:\d{2}s\]', '', out)

        if rc != 0:
            print(rc, out)
            return rc
        else:
            print out
            save_file(os.path.join(out_path, "".join([fio_cfg_name, ".log"])), "w", out)
            # out = out[out.find("Disk"):]
            # out = out[out.find(":")+2:]
            # JSON format nos supported on Debian IMG for now, not parsing
            # data = json.loads(out)
            # pprint(data)
            pass

        if perf_vmex:
            # Stop gathering perf statistics and prepare some result files
            perf_p.send_signal(signal.SIGINT)
            perf_p.wait()

            perf_stat_cmd = "perf kvm --host " + \
                            "-i {0} stat report".format(perf_rec_file)

            print(" ".join([perf_stat_cmd, "--event vmexit"]))
            rc, out = exec_cmd(" ".join([perf_stat_cmd, "--event vmexit"]),
                               blocking=True)

            print("VMexit host stats:")
            print("{0}".format(out))
            save_file(os.path.join(out_path, "vmexit_stats"),
                      "w", "{0}".format(out))


def main():

    abspath = os.path.abspath(__file__)
    dname = os.path.dirname(abspath)
    os.chdir(os.path.join(dname, "../../.."))

    global fio_bin
    global perf_vmex
    job_file_opt = False
    vms = []
    split_disks = []
    filenames = ""
    out_dir = os.path.join(os.getcwd(), "fio_results")
    fio_cfg_files = []
    rc = 0

    fio_args_def = {
        'size': ["10G"],
        'numjobs': ["1"],
        'testtype': ["randread"],
        'blocksize': ["4k"],
        'iodepth': ["128"],
        'ioengine': ["libaio"],
        'direct': ["1"],
        'verify': [""]
    }

    fio_args = fio_args_def.copy()

    try:
        opts, args = getopt.getopt(sys.argv[1:], "hj:t:b:i:D:n:F:I:s:v:o:S:p",
                                   ["help", "job-file=", "testtype=",
                                    "blocksize=", "iodepth=", "direct=",
                                    "numjobs=", "fio-bin=", "ioengine=",
                                    "size=", "verify=", "out=",
                                    "split-disks=", "perf"])
    except:
        show_help(fio_args_def)
        sys.exit(1)

    for o, a in opts:
        print o, a
        if o in ("-j", "--job-file"):
            fio_cfg_files = a.split(",")
            job_file_opt = True
            fio_args = fio_args_def.copy()
        elif o in ("-h", "--help"):
            show_help(fio_args_def)
            sys.exit(1)
        elif o in ("-p", "--perf-vmex"):
            perf_vmex = True
        elif o in ("-o", "--out"):
            out_dir = os.path.join(a, "fio_results")
        elif o in ("-F", "--fio-bin"):
            fio_bin = a
        elif o in ("-S", "--split-disks"):
            split_disks = [x.split("-") for x in a.split(",")]
            split_disks = [[int(x) - 1 for x in y] for y in split_disks]
            print split_disks
        elif o in ("-s", "--size"):
            fio_args["size"] = a.split(",")
        elif o in ("-t", "--testtype"):
            fio_args["testtype"] = a.split(",")
        elif o in ("-b", "--blocksize"):
            fio_args["blocksize"] = a.split(",")
        elif o in ("-i", "--iodepth"):
            fio_args["iodepth"] = a.split(",")
        elif o in ("-D", "--direct"):
            fio_args["direct"] = a.split(",")
        elif o in ("-n", "--numjobs"):
            fio_args["numjobs"] = a.split(",")
        elif o in ("-I", "--ioengine"):
            fio_args["ioengine"] = a.split(",")
        elif o in ("-v", "--verify"):
            fio_args["verify"] = a.split(",")
            fio_args["verify"] = ["" if x in "0" else
                                  "verify=crc32" for x in fio_args["verify"]]

    if len(args) < 1:
        show_help(fio_args_def)
        sys.exit(1)
    else:
        # Get IP, Port tuples from args and filename for fio config
        vms = [tuple(x.split(":")) for x in args[0].split(",")]
        filenames = [["/dev/" + y for y in x[2:]] for x in vms]
        vms = [x[0:2] for x in vms]

    if not os.path.exists(out_dir):
        os.mkdir(out_dir)

    if job_file_opt is True:
        for fio_cfg in fio_cfg_files:
            print("Running job file: {0}".format(fio_cfg))

            for i, vm in enumerate(zip(vms, filenames)):
                fnames = vm[1]
                if split_disks:
                    if len(split_disks[i]) < 2:
                        filename = fnames[split_disks[i][0]:split_disks[i][0] + 1]
                        filename = ":".join(filename)
                    else:
                        filename = fnames[split_disks[i][0]:split_disks[i][1] + 1]
                        filename = ":".join(filename)
                else:
                    filename = ":".join(fnames)

                a = exec_cmd("./test/vhost/fiotest/vm_ssh.sh " +
                             "{0} sh -c 'rm fio.job{1}'"
                             .format(i, i), blocking=True)

                for cfg in fio_cfg.split("\n"):
                    with open(cfg, "r") as fh:
                        lines = fh.readlines()
                        for line in lines:
                            if "filename" in line:
                                line = "filename=" + filename
                            a = exec_cmd("./test/vhost/fiotest/vm_ssh.sh " +
                                         "{0} sh -c 'echo {1} >> fio.job{2}'"
                                         .format(i, line.strip(), i), blocking=True)
                    fh.close()
            rc = run_fio(vms, fio_cfg, out_dir)
    else:
        for cfg in cfg_product(fio_args):
            # Update fio "size" parameter so that total work done by
            # all numjobs is equal to assigned size and not size*numjobs
            cfg["size"] = calc_size(cfg["size"], cfg["numjobs"])

        # Prepare this test run FIO job file
            for i, vm in enumerate(zip(vms, filenames)):
                fnames = vm[1]
                if split_disks:
                    if len(split_disks[i]) < 2:
                        filename = fnames[split_disks[i][0]:split_disks[i][0] + 1]
                        filename = ":".join(filename)
                    else:
                        filename = fnames[split_disks[i][0]:split_disks[i][1] + 1]
                        filename = ":".join(filename)
                else:
                    filename = ":".join(fnames)

                cfg.update({"filename": filename})
                fio_cfg = fio_template % cfg
                fio_cfg_files.append(prep_fio_cfg_file(out_dir,
                                                       fio_cfg, i))
                a = exec_cmd("./test/vhost/fiotest/vm_ssh.sh " +
                             "{0} sh -c 'rm fio.job{1}'"
                             .format(i, i), blocking=True)
                for line in fio_cfg.split("\n"):
                    a = exec_cmd("./test/vhost/fiotest/vm_ssh.sh " +
                                 "{0} sh -c 'echo {1} >> fio.job{2}'"
                                 .format(i, line.strip(), i), blocking=True)

            rc = run_fio(vms, cfg, out_dir)

    return rc

if __name__ == "__main__":
    sys.exit(main())
