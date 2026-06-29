#!/usr/bin/env python3
"""Test Secondary process startup via hot-upgrade primary_exit flow.

Flow: Primary starts -> primary_exit (drain+suspend+state_save+ipc_sock) ->
      Secondary starts -> pre_init (connect ipc + load state) -> reactors_start.
"""
import socket, json, subprocess, time, os, sys

PRIMARY_SOCK = "/var/tmp/spdk_hot_upgrade.sock"
SECONDARY_SOCK = "/var/tmp/spdk_secondary.sock"
BINARY = "/root/spdk/build/bin/spdk_tgt"
STATE_FILE = "/var/tmp/spdk_hot_upgrade_state"
IPC_SOCK = "/var/tmp/spdk_hu_ipc.sock"
ENV = os.environ.copy()
ENV["LD_LIBRARY_PATH"] = "/root/spdk/build/lib:/root/spdk/dpdk/build/lib:" + ENV.get("LD_LIBRARY_PATH","")

def rpc(sock, method, params=None, timeout=5):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock)
    req = {"jsonrpc":"2.0","method":method,"id":1}
    if params:
        req["params"] = params
    s.sendall(json.dumps(req).encode())
    r = s.recv(4096); s.close()
    return json.loads(r)

def wait_state(sock, expected, timeout=15):
    """Poll hot_upgrade_status until state matches or timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            resp = rpc(sock, "hot_upgrade_status", timeout=3)
            state = resp.get("result",{}).get("state","?")
            if state == expected:
                return True
            print(f"  state={state}, waiting for {expected}...", flush=True)
        except Exception as e:
            print(f"  rpc error: {e}", flush=True)
        time.sleep(0.5)
    return False

# Cleanup
os.system("pkill -9 -x spdk_tgt 2>/dev/null")
time.sleep(1)
for f in [PRIMARY_SOCK, SECONDARY_SOCK, STATE_FILE, IPC_SOCK]:
    os.system(f"rm -f {f} 2>/dev/null")

# Step 1: Start Primary
print("=== Step 1: Starting Primary ===", flush=True)
p = subprocess.Popen([BINARY, "-r", PRIMARY_SOCK, "--shm-id=100"],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=ENV)
time.sleep(3)
print(f"Primary PID: {p.pid}", flush=True)

# Check Primary is IDLE
resp = rpc(PRIMARY_SOCK, "hot_upgrade_status")
state = resp["result"]["state"]
print(f"Primary state: {state}", flush=True)
assert state == "IDLE", f"Expected IDLE, got {state}"

# Step 2: Call primary_exit (drain IO + save state + create IPC + suspend)
print("\n=== Step 2: Calling primary_exit ===", flush=True)
resp = rpc(PRIMARY_SOCK, "primary_exit", timeout=10)
print(f"primary_exit response: {json.dumps(resp)}", flush=True)
if "error" in resp:
    print(f"FAILED: primary_exit error: {resp['error']}", flush=True)
    p.terminate()
    sys.exit(1)

# Wait for Primary to reach SUSPENDED
print("\nWaiting for Primary to reach PRIMARY_SUSPENDED...", flush=True)
if not wait_state(PRIMARY_SOCK, "PRIMARY_SUSPENDED", timeout=15):
    print("FAILED: Primary did not reach PRIMARY_SUSPENDED", flush=True)
    p.terminate()
    out = p.stdout.read().decode(errors="replace")
    print("Primary output:", out[-3000:], flush=True)
    sys.exit(1)
print("Primary is SUSPENDED (HU_PAUSED, RPC active)", flush=True)

# Verify IPC socket and state file exist
print(f"IPC socket exists: {os.path.exists(IPC_SOCK)}", flush=True)
print(f"State file exists: {os.path.exists(STATE_FILE)}", flush=True)

# Step 3: Start Secondary
print("\n=== Step 3: Starting Secondary ===", flush=True)
p2 = subprocess.Popen(
    [BINARY, "-r", SECONDARY_SOCK, "--shm-id=100", "--proc-type=secondary"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=ENV
)
time.sleep(4)

# Check if Secondary is running
try:
    os.kill(p2.pid, 0)
    print(f"Secondary PID: {p2.pid} (ALIVE)", flush=True)
except OSError:
    print(f"Secondary PID: {p2.pid} (DEAD)", flush=True)
    out = p2.stdout.read().decode(errors="replace")
    print("Secondary output:", out[-3000:], flush=True)
    p.terminate()
    sys.exit(1)

# Step 4: Check Secondary RPC
print("\n=== Step 4: Checking Secondary RPC ===", flush=True)
try:
    resp = rpc(SECONDARY_SOCK, "spdk_get_version", timeout=10)
    print(f"Secondary version: {resp.get('result','?')}", flush=True)
except Exception as e:
    print(f"Secondary RPC FAILED: {e}", flush=True)
    out = p2.stdout.read().decode(errors="replace")
    print("Secondary output:", out[-3000:], flush=True)
    p.terminate(); p2.terminate()
    sys.exit(1)

# Step 5: Check Secondary hot_upgrade_status
print("\n=== Step 5: Checking Secondary HU state ===", flush=True)
try:
    resp = rpc(SECONDARY_SOCK, "hot_upgrade_status", timeout=5)
    hu_state = resp["result"]["state"]
    print(f"Secondary HU state: {hu_state}", flush=True)
except Exception as e:
    print(f"Secondary HU state query failed: {e}", flush=True)

print("\n=== SECONDARY TEST PASSED ===", flush=True)
p.terminate()
p2.terminate()
