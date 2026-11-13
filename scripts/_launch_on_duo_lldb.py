#!/usr/bin/env python3
"""
Launch WindowsiOS (`com.RRGSyndicate.WindowsiOS`) on Alan's physical iPhone Duo
(UDID from --device, $WINIOS_DUO_UDID or ~/.config/windowsios/duo-udid), service the iOS 26/27 TXM BreakpointJIT traps
(`brk #0x69` probe, `brk #0xf00d` RX allocation + 16 KiB page blessing + clean detach),
and let Windows 11 ARM64 run natively at full speed with zero debugger attached.
"""

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

# Import Xcode's LLDB Python bindings
lldb_python = subprocess.check_output(["xcrun", "lldb", "-P"], text=True).strip()
if lldb_python not in sys.path:
    sys.path.insert(0, lldb_python)
import lldb  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "work"
DEFAULT_BUNDLE_ID = "com.RRGSyndicate.WindowsiOS"


def configured_duo_udid() -> str:
    """$WINIOS_DUO_UDID, else ~/.config/windowsios/duo-udid; kept out of the repo."""
    udid = os.environ.get("WINIOS_DUO_UDID", "").strip()
    if not udid:
        cfg = Path.home() / ".config" / "windowsios" / "duo-udid"
        udid = cfg.read_text().strip() if cfg.exists() else ""
    return udid


def run_cmd(debugger, cmd: str) -> str:
    res = lldb.SBCommandReturnObject()
    debugger.GetCommandInterpreter().HandleCommand(cmd, res)
    out = (res.GetOutput() or "") + (res.GetError() or "")
    if not res.Succeeded():
        raise RuntimeError(f"LLDB command failed ({cmd}): {out.strip()}")
    return out


def find_stopped_thread(process):
    for i in range(process.GetNumThreads()):
        t = process.GetThreadAtIndex(i)
        sr = t.GetStopReason()
        if sr in (lldb.eStopReasonException, lldb.eStopReasonSignal, lldb.eStopReasonBreakpoint, lldb.eStopReasonPlanComplete):
            return t
    return process.GetSelectedThread()


def bless_txm_region(debugger, process, rx_addr: int, size: int):
    t0 = time.time()
    err = lldb.SBError()
    # First try fast 1 MiB MachVMWrite chunks via SBProcess.WriteMemory (touches every 16 KiB page)
    chunk_size = 1 << 20
    payload = b"\x69" * chunk_size
    wrote = process.WriteMemory(rx_addr, payload[: min(chunk_size, size)], err)
    if err.Success() and wrote == min(chunk_size, size):
        off = wrote
        while off < size:
            cur = min(chunk_size, size - off)
            w = process.WriteMemory(rx_addr + off, payload[:cur], err)
            if not err.Success() or w != cur:
                raise RuntimeError(f"WriteMemory failed at +{off:#x}: {err.GetCString()}")
            off += w
        dt = time.time() - t0
        print(f"[txm-jit] Blessed {size >> 20} MiB RX region at {rx_addr:#x} via bulk WriteMemory in {dt:.2f}s", flush=True)
        return

    print(f"[txm-jit] Bulk WriteMemory returned ({wrote}, {err.GetCString()}); falling back to per-16K-page M packets...", flush=True)
    pages = size // 0x4000
    for idx in range(pages):
        addr = rx_addr + idx * 0x4000
        w = process.WriteMemory(addr, b"\x69", err)
        if not err.Success() or w != 1:
            out = run_cmd(debugger, f"process plugin packet send M{addr:x},1:69")
            if "OK" not in out:
                raise RuntimeError(f"Failed to bless page {addr:#x}: {out}")
        if (idx + 1) % 4096 == 0 or idx + 1 == pages:
            print(f"[txm-jit] Blessed {idx + 1}/{pages} pages ({time.time() - t0:.1f}s)", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default=configured_duo_udid())
    ap.add_argument("--bundle-id", default=DEFAULT_BUNDLE_ID)
    args = ap.parse_args()
    if not args.device:
        ap.error("pass --device or set WINIOS_DUO_UDID")

    WORK.mkdir(parents=True, exist_ok=True)
    launch_json = WORK / "duo_launch.json"
    if launch_json.exists():
        launch_json.unlink()

    print(f"[duo] Launching {args.bundle_id} (--start-stopped) on {args.device}...", flush=True)
    for attempt in range(1, 6):
        r = subprocess.run(
            [
                "xcrun", "devicectl", "device", "process", "launch",
                "--start-stopped",
                "--terminate-existing",
                "--device", args.device,
                "--json-output", str(launch_json),
                args.bundle_id,
            ]
        )
        if r.returncode == 0 and launch_json.exists():
            break
        print(f"[duo] devicectl launch attempt {attempt} returned {r.returncode}; retrying in 2s...", flush=True)
        time.sleep(2)
    else:
        raise RuntimeError("devicectl device process launch failed after 5 attempts")
    info = json.loads(launch_json.read_text())
    pid = info["result"]["process"]["processIdentifier"]
    # devicectl JSON v5 reports the CoreDevice id as result.deviceIdentifier; older builds nest it.
    core_device_id = (
        info.get("result", {}).get("deviceIdentifier")
        or info.get("result", {}).get("device", {}).get("identifier")
        or args.device
    )
    print(f"[duo] Launched stopped PID = {pid} on CoreDevice {core_device_id}; attaching LLDB...", flush=True)

    lldb.SBDebugger.Initialize()
    debugger = lldb.SBDebugger.Create(True)
    debugger.SetAsync(False)

    # Xcode 27's LLDB resolves `device select` only after `device list` has populated its registry.
    run_cmd(debugger, "device list")
    run_cmd(debugger, f"device select {core_device_id}")
    run_cmd(debugger, f"device process attach -p {pid}")
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()
    print(f"[duo] Attached to PID {process.GetProcessID()} (state={process.GetState()})", flush=True)

    detached = False
    for step in range(1, 64):
        process.Continue()
        state = process.GetState()
        if state != lldb.eStateStopped:
            raise RuntimeError(f"Process entered unexpected state {state} at step {step}")

        thread = find_stopped_thread(process)
        frame = thread.GetFrameAtIndex(0)
        pc = frame.FindRegister("pc").GetValueAsUnsigned()
        x0 = frame.FindRegister("x0").GetValueAsUnsigned()
        x1 = frame.FindRegister("x1").GetValueAsUnsigned()
        x16 = frame.FindRegister("x16").GetValueAsUnsigned()

        err = lldb.SBError()
        raw = process.ReadMemory(pc, 4, err)
        if not err.Success() or len(raw) != 4:
            print(f"[duo] Stop #{step}: pc={pc:#x} (could not read insn: {err.GetCString()}); continuing", flush=True)
            continue

        insn = struct.unpack("<I", raw)[0]
        if (insn & 0xFFE0001F) != 0xD4200000:
            desc = thread.GetStopDescription(128)
            print(f"[duo] Stop #{step}: non-BRK stop at pc={pc:#x} insn={insn:#010x} ({desc}); continuing", flush=True)
            continue

        imm = (insn >> 5) & 0xFFFF
        print(f"[duo] Stop #{step}: BRK #{imm:#x} at pc={pc:#x} (x0={x0:#x}, x1={x1:#x}, x16={x16:#x})", flush=True)

        if imm == 0x69:
            frame.FindRegister("x0").SetValueFromCString("0xE0000069")
            frame.FindRegister("pc").SetValueFromCString(hex(pc + 4))
            print("[txm-jit] Answered probe BRK #0x69 -> x0 = 0xE0000069", flush=True)
        elif imm == 0xF00D:
            if x16 == 1:
                rx_addr = x0
                if rx_addr == 0:
                    pkt_out = run_cmd(debugger, f"process plugin packet send _M{x1:x},rx")
                    m = re.search(r"response:\s*([0-9a-fA-F]+)", pkt_out)
                    if not m:
                        raise RuntimeError(f"Unexpected _M packet response: {pkt_out}")
                    rx_addr = int(m.group(1), 16)
                    print(f"[txm-jit] Allocated {x1 >> 20} MiB RX region at {rx_addr:#x}", flush=True)
                bless_txm_region(debugger, process, rx_addr, x1)
                frame.FindRegister("x0").SetValueFromCString(hex(rx_addr))
                frame.FindRegister("pc").SetValueFromCString(hex(pc + 4))
            elif x16 == 0:
                frame.FindRegister("pc").SetValueFromCString(hex(pc + 4))
                print("[txm-jit] Servicing CMD_DETACH -> detaching LLDB cleanly!", flush=True)
                err = process.Detach()
                if not err.Success():
                    raise RuntimeError(f"Detach failed: {err.GetCString()}")
                detached = True
                break
            else:
                print(f"[txm-jit] Unknown BRK #0xf00d command x16={x16:#x}; skipping", flush=True)
                frame.FindRegister("pc").SetValueFromCString(hex(pc + 4))
        else:
            print(f"[duo] Unhandled BRK #{imm:#x}; advancing pc+4", flush=True)
            frame.FindRegister("pc").SetValueFromCString(hex(pc + 4))

    lldb.SBDebugger.Destroy(debugger)
    lldb.SBDebugger.Terminate()
    if not detached:
        raise RuntimeError("Exited loop without seeing CMD_DETACH")
    print("[duo] SUCCESS: TXM JIT + Ledger-Exempt Guest RAM active and debugger detached!", flush=True)


if __name__ == "__main__":
    main()
