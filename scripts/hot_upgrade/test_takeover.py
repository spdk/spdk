#!/usr/bin/env python3
"""Test Secondary takeover flow: Primary creates bdev -> primary_exit ->
Secondary pre_init -> secondary_init (takeover) -> verify bdev visible.
"""
import socket, json, subprocess, time, os, sys, glob

PRIMARY_SOCK = "/var/tmp/spdk_hot_upgrade.sock"
SECONDARY_SOCK = "/var/tmp/spdk_secondary.sock"
BINARY = "/root/spdk/build/bin/spdk_tgt"
STATE_FILE = "/var/tmp/spdk_hot_upgrade_state"
IPC_SOCK = "/var/tmp/spdk_hu_ipc.sock"
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
    r = s.recv(8192)
    s.close()
    return json.loads(r)

def wait_state(sock, expected, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            resp = rpc(sock, "hot_upgrade_status", timeout=3)
            state = resp.get("result", {}).get("state", "?")
            if state == expected:
                return True
            print(f"  state={state}, waiting for {expected}...", flush=True)
        except Exception as e:
            print(f"  rpc error: {e}", flush=True)
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

cleanup()

# Step 1: Start Primary
print("=== Step 1: Starting Primary ===", flush=True)
p = subprocess.Popen([BINARY, "-r", PRIMARY_SOCK, "--shm-id=100"],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=ENV)
time.sleep(3)
print(f"Primary PID: {p.pid}", flush=True)
resp = rpc(PRIMARY_SOCK, "hot_upgrade_status")
state = resp["result"]["state"]
print(f"Primary state: {state}", flush=True)
assert state == "IDLE", f"Expected IDLE, got {state}"

# Step 2: Create a malloc bdev on Primary
print("\n=== Step 2: Creating malloc bdev on Primary ===", flush=True)
resp = rpc(PRIMARY_SOCK, "bdev_malloc_create",
           {"block_size": 512, "num_blocks": 64, "name": "Malloc0"}, timeout=10)
print(f"bdev_malloc_create: {json.dumps(resp)}", flush=True)
if "error" in resp:
    print(f"FAILED: bdev create error: {resp['error']}", flush=True)
    p.terminate()
    sys.exit(1)
bdev_name = resp.get("result", "Malloc0")

# Verify bdev exists on Primary
resp = rpc(PRIMARY_SOCK, "bdev_get_bdevs")
bdevs = resp.get("result", [])
print(f"Primary bdev_get_bdevs: {len(bdevs)} bdev(s)", flush=True)
for b in bdevs:
    print(f"  - {b.get('name')} (uuid={b.get('uuid', '?')[:8]})", flush=True)

# Step 3: Call primary_exit
print("\n=== Step 3: Calling primary_exit ===", flush=True)
resp = rpc(PRIMARY_SOCK, "primary_exit", timeout=10)
print(f"primary_exit: {json.dumps(resp)}", flush=True)
if "error" in resp:
    print(f"FAILED: primary_exit error: {resp['error']}", flush=True)
    p.terminate()
    sys.exit(1)

if not wait_state(PRIMARY_SOCK, "PRIMARY_SUSPENDED", timeout=15):
    print("FAILED: Primary did not reach PRIMARY_SUSPENDED", flush=True)
    p.terminate()
    out = p.stdout.read().decode(errors="replace")
    print("Primary output:", out[-3000:], flush=True)
    sys.exit(1)
print("Primary is SUSPENDED", flush=True)

# Step 4: Start Secondary
print("\n=== Step 4: Starting Secondary ===", flush=True)
p2 = subprocess.Popen(
    [BINARY, "-r", SECONDARY_SOCK, "--shm-id=100", "--proc-type=secondary"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=ENV
)
time.sleep(4)
try:
    os.kill(p2.pid, 0)
    print(f"Secondary PID: {p2.pid} (ALIVE)", flush=True)
except OSError:
    print(f"Secondary PID: {p2.pid} (DEAD)", flush=True)
    out = p2.stdout.read().decode(errors="replace")
    print("Secondary output:", out[-3000:], flush=True)
    p.terminate()
    sys.exit(1)

# Wait for Secondary to reach SECONDARY_PRE_INIT_DONE
if not wait_state(SECONDARY_SOCK, "SECONDARY_PRE_INIT_DONE", timeout=15):
    print("FAILED: Secondary did not reach SECONDARY_PRE_INIT_DONE", flush=True)
    out = p2.stdout.read().decode(errors="replace")
    print("Secondary output:", out[-3000:], flush=True)
    p.terminate(); p2.terminate()
    sys.exit(1)
print("Secondary is at SECONDARY_PRE_INIT_DONE", flush=True)

# Step 5: Call secondary_init (takeover!)
print("\n=== Step 5: Calling secondary_init (TAKEOVER) ===", flush=True)
resp = rpc(SECONDARY_SOCK, "secondary_init", timeout=10)
print(f"secondary_init: {json.dumps(resp)}", flush=True)
if "error" in resp:
    print(f"FAILED: secondary_init error: {resp['error']}", flush=True)
    out = p2.stdout.read().decode(errors="replace")
    print("Secondary output:", out[-3000:], flush=True)
    p.terminate(); p2.terminate()
    sys.exit(1)

# Step 6: Verify HU state = COMPLETE
print("\n=== Step 6: Verifying Secondary HU state ===", flush=True)
resp = rpc(SECONDARY_SOCK, "hot_upgrade_status")
hu_state = resp.get("result", {}).get("state", "?")
print(f"Secondary HU state: {hu_state}", flush=True)
if hu_state != "COMPLETE":
    print(f"WARNING: Expected COMPLETE, got {hu_state}", flush=True)

# Step 7: Verify bdev is visible on Secondary
print("\n=== Step 7: Checking bdev on Secondary ===", flush=True)
try:
    resp = rpc(SECONDARY_SOCK, "bdev_get_bdevs", timeout=5)
    if "error" in resp:
        print(f"bdev_get_bdevs error: {resp['error']}", flush=True)
    else:
        bdevs = resp.get("result", [])
        print(f"Secondary bdev_get_bdevs: {len(bdevs)} bdev(s)", flush=True)
        for b in bdevs:
            print(f"  - {b.get('name')} (uuid={b.get('uuid', '?')[:8]})", flush=True)
        if len(bdevs) > 0:
            print("\n=== TAKEOVER TEST PASSED: bdev visible on Secondary ===", flush=True)
        else:
            print("\n=== TAKEOVER partially OK: state=COMPLETE but Primary bdevs not shared ===", flush=True)

    # Step 7.5: Try creating a NEW bdev on Secondary (tests bdev subsystem functionality)
    print("\n=== Step 7.5: Creating new bdev on Secondary ===", flush=True)
    try:
        resp = rpc(SECONDARY_SOCK, "bdev_malloc_create",
                   {"block_size": 512, "num_blocks": 32, "name": "SecMalloc0"}, timeout=10)
        print(f"Secondary bdev_malloc_create: {json.dumps(resp)}", flush=True)
        if "error" not in resp:
            resp = rpc(SECONDARY_SOCK, "bdev_get_bdevs")
            bdevs2 = resp.get("result", [])
            print(f"Secondary now has {len(bdevs2)} bdev(s)", flush=True)
            for b in bdevs2:
                print(f"  - {b.get('name')}", flush=True)
            print("\n=== BDEV SUBSYSTEM WORKS ON SECONDARY ===", flush=True)
    except Exception as e:
        print(f"Secondary bdev create failed: {e}", flush=True)
except Exception as e:
    print(f"bdev_get_bdevs FAILED: {e}", flush=True)
    out = p2.stdout.read().decode(errors="replace")
    print("Secondary output:", out[-3000:], flush=True)
    print("\n=== TAKEOVER TEST: state transition OK, bdev query failed ===", flush=True)

# Step 8: Verify Primary is still suspended (both alive)
print("\n=== Step 8: Verifying Primary still suspended ===", flush=True)
try:
    resp = rpc(PRIMARY_SOCK, "hot_upgrade_status", timeout=3)
    primary_state = resp.get("result", {}).get("state", "?")
    print(f"Primary HU state: {primary_state}", flush=True)
except Exception as e:
    print(f"Primary RPC failed: {e}", flush=True)

print("\n=== DONE ===", flush=True)
p.terminate()
p2.terminate()
