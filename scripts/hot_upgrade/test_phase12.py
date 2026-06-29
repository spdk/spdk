#!/usr/bin/env python3
"""Phase 12 test: TSC timeline measurement + IO interruption verification.

Tests:
  1. test_timeline_tsc_nonzero        — VT-IO-002: 6 个 TSC 时间戳全部非零
  2. test_io_interruption_under_200ms — VT-IO-004: TSC 测量 IO 中断 < 200ms
  3. test_timeline_phases_ordered     — 时间戳单调递增

Each test runs a full hot upgrade cycle, then queries the hot_upgrade_get_timeline
RPC on the new Primary (former Secondary) to verify the TSC timeline.
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
TIMELINE_FILE = "/var/tmp/spdk_hot_upgrade_timeline"
IPC_SOCK = "/var/tmp/spdk_hu_ipc.sock"
SCRIPT_DIR = "/root/spdk/scripts/hot_upgrade"
ENV = os.environ.copy()
ENV["LD_LIBRARY_PATH"] = "/root/spdk/build/lib:/root/spdk/dpdk/build/lib:" + ENV.get("LD_LIBRARY_PATH", "")

TIMELINE_FIELDS = [
    "tsc_primary_exit_start",
    "tsc_primary_drain_done",
    "tsc_primary_suspend_done",
    "tsc_secondary_init_start",
    "tsc_secondary_takeover_done",
    "tsc_reactor_running",
]


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


def get_timeline(sock):
    """Query hot_upgrade_get_timeline RPC, return result dict."""
    resp = rpc(sock, "hot_upgrade_get_timeline", timeout=5)
    return resp.get("result", {})


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
    for f in [PRIMARY_SOCK, SECONDARY_SOCK, STATE_FILE, TIMELINE_FILE, IPC_SOCK]:
        os.system(f"rm -f {f} 2>/dev/null")
    os.system("rm -f /var/tmp/spdk_cpu_lock_* 2>/dev/null")
    os.system("rm -rf /var/run/dpdk/spdk100 2>/dev/null")
    os.system("rm -f /dev/hugepages/spdk100map_* 2>/dev/null")


def start_primary_and_bdev():
    """Start Primary with Malloc0 bdev, return Popen handle."""
    p = subprocess.Popen(
        [BINARY, "-r", PRIMARY_SOCK, "--shm-id=100"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=ENV)
    time.sleep(3)
    print(f"  Primary PID: {p.pid}", flush=True)

    state = get_state(PRIMARY_SOCK)
    assert state == "IDLE", f"Expected IDLE, got {state}"

    resp = rpc(PRIMARY_SOCK, "bdev_malloc_create",
               {"block_size": 512, "num_blocks": 64, "name": "Malloc0"}, timeout=10)
    assert "error" not in resp, f"bdev create failed: {resp}"
    print("  Malloc0 bdev created", flush=True)
    return p


def run_hot_upgrade():
    """Run full hot upgrade, return (proc, new_primary_sock)."""
    proc = subprocess.run(
        [f"{SCRIPT_DIR}/spdk_hot_upgrade.sh", BINARY,
         "--primary-sock", PRIMARY_SOCK,
         "--secondary-sock", SECONDARY_SOCK, "-v"],
        capture_output=True, text=True, timeout=120, env=ENV)

    print(proc.stdout)
    if proc.stderr:
        print("STDERR:", proc.stderr)

    assert proc.returncode == 0, \
        f"Hot upgrade failed (rc={proc.returncode}):\n{proc.stdout}\n{proc.stderr}"

    # After commit, Secondary socket is renamed to PRIMARY_SOCK
    time.sleep(1)
    state_primary = get_state(PRIMARY_SOCK)
    state_secondary = get_state(SECONDARY_SOCK)
    print(f"  New Primary state: {state_primary}, Secondary state: {state_secondary}")

    if state_primary == "COMPLETE":
        return proc, PRIMARY_SOCK
    elif state_secondary == "COMPLETE":
        return proc, SECONDARY_SOCK
    else:
        raise AssertionError(f"Neither socket is COMPLETE (primary={state_primary}, secondary={state_secondary})")


def test_timeline_tsc_nonzero():
    """Test 1: VT-IO-002 — all 6 TSC timestamps are non-zero."""
    print("\n[Test 1] Timeline TSC fields non-zero (VT-IO-002)...", flush=True)

    proc, check_sock = run_hot_upgrade()
    tl = get_timeline(check_sock)
    print(f"  Timeline: {json.dumps(tl, indent=2)}", flush=True)

    assert tl.get("tsc_rate", 0) > 0, f"tsc_rate is zero: {tl}"
    for field in TIMELINE_FIELDS:
        val = tl.get(field, 0)
        assert val > 0, f"Field {field} is zero (val={val}): {tl}"
        print(f"  {field}: {val}")

    print("  PASSED: all 6 TSC fields non-zero", flush=True)
    return True


def test_io_interruption_under_200ms():
    """Test 2: VT-IO-004 — TSC-measured IO interruption < 200ms."""
    print("\n[Test 2] IO interruption < 200ms (VT-IO-004)...", flush=True)

    proc, check_sock = run_hot_upgrade()
    tl = get_timeline(check_sock)
    print(f"  Timeline: {json.dumps(tl, indent=2)}", flush=True)

    total_ms = tl.get("total_io_interruption_ms", 0)
    assert total_ms > 0, f"total_io_interruption_ms not computed (val={total_ms}): {tl}"
    print(f"  TSC total_io_interruption_ms: {total_ms}")

    assert total_ms < 200, \
        f"IO interruption {total_ms}ms >= 200ms threshold"

    # Cross-validate with shell-side [METRIC] measurement
    shell_io_ms = None
    for line in proc.stdout.splitlines():
        if "IO interruption" in line:
            print(f"  {line.strip()}")
            m = re.search(r'([\d.]+)s', line)
            if m:
                shell_io_ms = float(m.group(1)) * 1000
    if shell_io_ms is not None:
        print(f"  Shell-side IO interruption: {shell_io_ms:.1f}ms")
        diff = shell_io_ms - total_ms
        print(f"  TSC vs shell difference: +{diff:.1f}ms (shell includes RPC + verify overhead)")
        # Shell measurement is an upper bound (includes RPC round-trip + step6 verify);
        # TSC is the precise internal critical section. Only sanity-check shell < 500ms.
        assert shell_io_ms < 500, \
            f"Shell-side IO interruption {shell_io_ms:.1f}ms >= 500ms sanity limit"

    print(f"  PASSED: IO interruption {total_ms}ms < 200ms", flush=True)
    return True


def test_timeline_phases_ordered():
    """Test 3: timestamps are strictly monotonically increasing."""
    print("\n[Test 3] Timeline phases ordered (monotonic)...", flush=True)

    proc, check_sock = run_hot_upgrade()
    tl = get_timeline(check_sock)
    print(f"  Timeline: {json.dumps(tl, indent=2)}", flush=True)

    values = [tl.get(f, 0) for f in TIMELINE_FIELDS]
    for i, v in enumerate(values):
        print(f"  {TIMELINE_FIELDS[i]}: {v}")

    for i in range(len(values) - 1):
        assert values[i] < values[i + 1], \
            f"Phase order violated: {TIMELINE_FIELDS[i]}={values[i]} >= " \
            f"{TIMELINE_FIELDS[i+1]}={values[i+1]}"

    print("  PASSED: all 6 phases strictly monotonically increasing", flush=True)
    return True


def main():
    print("=" * 60, flush=True)
    print("  Phase 12 Test: TSC Timeline Measurement", flush=True)
    print("=" * 60, flush=True)

    results = []
    try:
        # Test 1
        print("\n=== Cleanup before Test 1 ===", flush=True)
        cleanup()
        p1 = start_primary_and_bdev()
        results.append(("test_timeline_tsc_nonzero", test_timeline_tsc_nonzero()))
        cleanup()

        # Test 2
        print("\n=== Cleanup before Test 2 ===", flush=True)
        p2 = start_primary_and_bdev()
        results.append(("test_io_interruption_under_200ms", test_io_interruption_under_200ms()))
        cleanup()

        # Test 3
        print("\n=== Cleanup before Test 3 ===", flush=True)
        p3 = start_primary_and_bdev()
        results.append(("test_timeline_phases_ordered", test_timeline_phases_ordered()))
        cleanup()

    except Exception as e:
        print(f"\n[ERROR] Test failed with exception: {e}", flush=True)
        import traceback
        traceback.print_exc()
        results.append(("exception", False))
    finally:
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
        print("\n=== All Phase 12 tests PASSED ===", flush=True)
        sys.exit(0)
    else:
        print("\n=== Some tests FAILED ===", flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
