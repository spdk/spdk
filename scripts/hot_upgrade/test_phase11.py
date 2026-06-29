#!/usr/bin/env python3
"""Phase 11 test: parallel hot upgrade flow + IO interruption measurement.

Flow:
  1. Clean up stale processes/sockets
  2. Start Primary with --shm-id=100
  3. Create Malloc0 bdev on Primary
  4. Run spdk_hot_upgrade.sh (precheck -> do -> commit)
  5. Verify Secondary is COMPLETE and inherited Malloc0
  6. Measure IO interruption time
  7. Clean up
"""
import socket
import json
import subprocess
import time
import os
import sys
import glob
import re

PRIMARY_SOCK = "/var/tmp/spdk_hot_upgrade.sock"
SECONDARY_SOCK = "/var/tmp/spdk_secondary.sock"
BINARY = "/root/spdk/build/bin/spdk_tgt"
STATE_FILE = "/var/tmp/spdk_hot_upgrade_state"
IPC_SOCK = "/var/tmp/spdk_hu_ipc.sock"
SCRIPT_DIR = "/root/spdk/scripts/hot_upgrade"
ENV = os.environ.copy()
ENV["LD_LIBRARY_PATH"] = "/root/spdk/build/lib:/root/spdk/dpdk/build/lib:" + ENV.get("LD_LIBRARY_PATH", "")


def rpc(sock, method, params=None, timeout=5):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock)
    req = {"jsonrpc": "2.0", "method": method, "id": 1}
    if params:
        req["params"] = params
    s.sendall(json.dumps(req).encode())
    r = s.recv(65536)
    s.close()
    return json.loads(r)


def get_state(sock):
    try:
        resp = rpc(sock, "hot_upgrade_status", timeout=3)
        return resp.get("result", {}).get("state", "UNKNOWN")
    except Exception:
        return "UNKNOWN"


def wait_state(sock, expected, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        state = get_state(sock)
        if state == expected:
            return True
        time.sleep(0.5)
    return False


def kill_stale_spdk():
    my_pid = os.getpid()
    for exe_link in glob.glob("/proc/[0-9]*/exe"):
        try:
            target = os.readlink(exe_link)
        except OSError:
            continue
        if target == BINARY:
            pid = int(exe_link.split("/")[2])
            if pid != my_pid:
                try:
                    os.kill(pid, 9)
                except OSError:
                    pass


def cleanup():
    kill_stale_spdk()
    time.sleep(1)
    for f in [PRIMARY_SOCK, SECONDARY_SOCK, STATE_FILE, IPC_SOCK]:
        os.system(f"rm -f {f} 2>/dev/null")
    os.system("rm -f /var/tmp/spdk_cpu_lock_* 2>/dev/null")
    os.system("rm -rf /var/run/dpdk/spdk100 2>/dev/null")
    os.system("rm -f /dev/hugepages/spdk100map_* 2>/dev/null")


def test_precheck_passes():
    """Test 1: precheck passes with valid binary and running Primary."""
    print("\n[Test 1] Precheck passes with valid binary...", flush=True)
    rc = subprocess.run(
        [f"{SCRIPT_DIR}/spdk_hot_upgrade_precheck.sh",
         "-p", PRIMARY_SOCK, "-n", BINARY, "-s", SECONDARY_SOCK, "-v"],
        capture_output=True, text=True, env=ENV)
    print(rc.stdout)
    if rc.stderr:
        print("STDERR:", rc.stderr)
    assert rc.returncode == 0, f"Precheck failed (rc={rc.returncode})"
    assert "version" in rc.stdout.lower(), "Version check missing in output"
    assert "--shm-id" in rc.stdout, "shm-id check missing in output"
    assert "SPDK" in rc.stdout, "SPDK binary validation missing"
    print("  PASSED", flush=True)
    return True


def test_precheck_rejects_bad_binary():
    """Test 2: precheck rejects non-SPDK binary."""
    print("\n[Test 2] Precheck rejects non-SPDK binary...", flush=True)
    fake = "/tmp/fake_spdk_tgt"
    with open(fake, "w") as f:
        f.write("#!/bin/bash\necho 'not spdk'\n")
    os.chmod(fake, 0o755)
    rc = subprocess.run(
        [f"{SCRIPT_DIR}/spdk_hot_upgrade_precheck.sh",
         "-p", PRIMARY_SOCK, "-n", fake, "-s", SECONDARY_SOCK],
        capture_output=True, text=True, env=ENV)
    print(rc.stdout)
    os.unlink(fake)
    assert rc.returncode != 0, "Precheck should reject non-SPDK binary"
    assert "SPDK" in rc.stdout or "does not contain" in rc.stdout, "Should report SPDK validation failure"
    print("  PASSED", flush=True)
    return True


def test_parallel_flow():
    """Test 3: full parallel hot upgrade flow via single entry point."""
    print("\n[Test 3] Full parallel hot upgrade flow...", flush=True)

    # Verify Primary is IDLE before start
    state = get_state(PRIMARY_SOCK)
    assert state == "IDLE", f"Primary not IDLE (got {state})"
    print(f"  Primary state before upgrade: {state}", flush=True)

    # Record start time
    t_start = time.time()

    # Run full hot upgrade (precheck -> do -> commit)
    proc = subprocess.run(
        [f"{SCRIPT_DIR}/spdk_hot_upgrade.sh", BINARY,
         "--primary-sock", PRIMARY_SOCK,
         "--secondary-sock", SECONDARY_SOCK, "-v"],
        capture_output=True, text=True, timeout=120, env=ENV)

    t_total = time.time() - t_start

    print(proc.stdout)
    if proc.stderr:
        print("STDERR:", proc.stderr)

    assert proc.returncode == 0, \
        f"Hot upgrade failed (rc={proc.returncode}):\n{proc.stdout}\n{proc.stderr}"

    print(f"\n  Total wall-clock time: {t_total:.2f}s")

    # Extract IO interruption from output
    io_dur = None
    for line in proc.stdout.splitlines():
        if "IO interruption" in line:
            print(f"  {line.strip()}")
            m = re.search(r'([\d.]+)s', line)
            if m:
                io_dur = float(m.group(1))

    if io_dur is not None:
        print(f"  IO interruption: {io_dur:.3f}s")
        if io_dur < 1.0:
            print("  IO interruption < 1s: PASSED")
        else:
            print(f"  WARNING: IO interruption >= 1s ({io_dur:.3f}s)")
    else:
        print("  WARNING: IO interruption metric not found in output")

    # After commit, Primary is terminated and Secondary socket is renamed to PRIMARY_SOCK
    # Wait a moment for socket rename to take effect
    time.sleep(1)

    # Verify the new Primary (former Secondary) is COMPLETE and serving on PRIMARY_SOCK
    state = get_state(PRIMARY_SOCK)
    print(f"  New Primary state: {state}")
    if state != "COMPLETE":
        # Maybe socket is still at SECONDARY_SOCK
        state2 = get_state(SECONDARY_SOCK)
        print(f"  Secondary socket state: {state2}")
        check_sock = SECONDARY_SOCK if state2 == "COMPLETE" else PRIMARY_SOCK
    else:
        check_sock = PRIMARY_SOCK

    assert state == "COMPLETE" or get_state(SECONDARY_SOCK) == "COMPLETE", \
        f"Neither Primary nor Secondary is COMPLETE (primary={state}, secondary={get_state(SECONDARY_SOCK)})"

    # Verify bdev inherited
    bdevs_resp = rpc(check_sock, "bdev_get_bdevs")
    bdevs = bdevs_resp.get("result", [])
    print(f"  Bdev count after upgrade: {len(bdevs)}")
    for b in bdevs:
        print(f"    - {b.get('name')} (bs={b.get('block_size')}, nb={b.get('num_blocks')})")

    assert len(bdevs) > 0, "No bdevs inherited by Secondary"
    bdev_names = [b.get("name") for b in bdevs]
    assert "Malloc0" in bdev_names, f"Malloc0 not found in bdev list: {bdev_names}"

    print("  PASSED: bdev Malloc0 inherited successfully", flush=True)
    return True


def main():
    print("=" * 60, flush=True)
    print("  Phase 11 Test: Parallel Hot Upgrade Flow", flush=True)
    print("=" * 60, flush=True)

    # Cleanup any stale state
    print("\n=== Cleanup ===", flush=True)
    cleanup()

    # Start Primary
    print("\n=== Starting Primary ===", flush=True)
    p = subprocess.Popen(
        [BINARY, "-r", PRIMARY_SOCK, "--shm-id=100"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=ENV)
    time.sleep(3)
    print(f"Primary PID: {p.pid}", flush=True)

    state = get_state(PRIMARY_SOCK)
    print(f"Primary state: {state}", flush=True)
    assert state == "IDLE", f"Expected IDLE, got {state}"

    # Create Malloc0 bdev
    print("\n=== Creating Malloc0 bdev ===", flush=True)
    resp = rpc(PRIMARY_SOCK, "bdev_malloc_create",
               {"block_size": 512, "num_blocks": 64, "name": "Malloc0"}, timeout=10)
    print(f"bdev_malloc_create: {json.dumps(resp)}", flush=True)
    assert "error" not in resp, f"bdev create failed: {resp}"

    # Verify bdev exists
    resp = rpc(PRIMARY_SOCK, "bdev_get_bdevs")
    bdevs = resp.get("result", [])
    print(f"Primary bdev count: {len(bdevs)}", flush=True)
    assert len(bdevs) > 0, "No bdevs on Primary"

    # Run tests
    results = []
    try:
        results.append(("test_precheck_passes", test_precheck_passes()))
        results.append(("test_precheck_rejects_bad_binary", test_precheck_rejects_bad_binary()))
        results.append(("test_parallel_flow", test_parallel_flow()))
    except Exception as e:
        print(f"\n[ERROR] Test failed with exception: {e}", flush=True)
        import traceback
        traceback.print_exc()
        results.append(("exception", False))
    finally:
        # Cleanup: kill all SPDK processes
        print("\n=== Final Cleanup ===", flush=True)
        cleanup()

    # Summary
    print("\n" + "=" * 60, flush=True)
    print("  Test Summary", flush=True)
    print("=" * 60, flush=True)
    all_passed = True
    for name, passed in results:
        status = "PASSED" if passed else "FAILED"
        print(f"  {name}: {status}", flush=True)
        if not passed:
            all_passed = False

    if all_passed:
        print("\n=== All Phase 11 tests PASSED ===", flush=True)
        sys.exit(0)
    else:
        print("\n=== Some tests FAILED ===", flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
