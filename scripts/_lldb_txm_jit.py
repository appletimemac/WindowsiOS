"""
LLDB script module imported inside `xcrun lldb` so Xcode's built-in CoreDevice
`device select` + `device process attach` commands are available alongside full
in-process `lldb.SBProcess` speed!
"""

import re
import struct
import time
from pathlib import Path
import lldb

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "work"


def run_cmd(debugger, cmd: str) -> str:
    res = lldb.SBCommandReturnObject()
    debugger.GetCommandInterpreter().HandleCommand(cmd, res)
    out = (res.GetOutput() or "") + (res.GetError() or "")
    if not res.Succeeded():
        raise RuntimeError(f"LLDB command failed ({cmd}): {out.strip()}")
    return out


def write_reg(debugger, process, thread, reg_name: str, val_int: int):
    process.SetSelectedThread(thread)
    frame = thread.GetFrameAtIndex(0)
    val_hex = f"0x{val_int:x}"
    reg = frame.FindRegister(reg_name)
    ok = reg.IsValid() and reg.SetValueFromCString(val_hex)
    if not ok or frame.FindRegister(reg_name).GetValueAsUnsigned() != val_int:
        run_cmd(debugger, f"register write {reg_name} {val_hex}")


def find_brk_or_stopped_thread(process):
    err = lldb.SBError()
    fallback = None
    for i in range(process.GetNumThreads()):
        t = process.GetThreadAtIndex(i)
        sr = t.GetStopReason()
        if sr in (
            lldb.eStopReasonException,
            lldb.eStopReasonSignal,
            lldb.eStopReasonBreakpoint,
            lldb.eStopReasonPlanComplete,
        ):
            if fallback is None:
                fallback = t
            f = t.GetFrameAtIndex(0)
            if f and f.IsValid():
                pc = f.FindRegister("pc").GetValueAsUnsigned()
                if pc != 0:
                    raw = process.ReadMemory(pc, 4, err)
                    if err.Success() and len(raw) == 4:
                        insn = struct.unpack("<I", raw)[0]
                        if (insn & 0xFFE0001F) == 0xD4200000:
                            return t
    return fallback or process.GetSelectedThread()


def bless_txm_region(debugger, process, rx_addr: int, size: int):
    t0 = time.time()
    err = lldb.SBError()
    chunk_size = 1 << 20
    payload = b"\x69" * chunk_size
    wrote = process.WriteMemory(rx_addr, payload[: min(chunk_size, size)], err)
    if err.Success() and wrote == min(chunk_size, size):
        off = wrote
        while off < size:
            cur = min(chunk_size, size - off)
            # Over wireless debugging a packet send can fail transiently; retry the chunk.
            for attempt in range(1, 5):
                err = lldb.SBError()  # a failed attempt must not leave its error on the retry
                w = process.WriteMemory(rx_addr + off, payload[:cur], err)
                if err.Success() and w == cur:
                    break
                print(f"[txm-jit] WriteMemory at +{off:#x} failed (attempt {attempt}): {err.GetCString()}", flush=True)
                time.sleep(0.5 * attempt)
            else:
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


def txm_jit_cmd(debugger, command, exe_ctx, result, internal_dict):
    debugger.SetAsync(False)
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()
    if not process or not process.IsValid() or process.GetProcessID() == 0:
        raise RuntimeError("No valid attached process in LLDB")
    print(f"[txm-jit] Attached to PID {process.GetProcessID()} (state={process.GetState()})", flush=True)

    # Wait for initial --start-stopped attach stop to settle with valid thread registers before Continue()
    listener = debugger.GetListener()
    ev = lldb.SBEvent()
    t_wait = time.time()
    while time.time() - t_wait < 10.0:
        if process.GetState() == lldb.eStateStopped and process.GetNumThreads() > 0:
            t0_pc = process.GetThreadAtIndex(0).GetFrameAtIndex(0).FindRegister("pc").GetValueAsUnsigned()
            if t0_pc != 0:
                print(f"[txm-jit] Initial attach settled: state=Stopped, threads={process.GetNumThreads()}, pc={t0_pc:#x}", flush=True)
                break
        if listener.WaitForEvent(1, ev):
            pass
        else:
            time.sleep(0.05)

    detached = False
    for step in range(1, 64):
        process.Continue()
        state = process.GetState()
        if state != lldb.eStateStopped:
            raise RuntimeError(f"Process entered unexpected state {state} at step {step}")

        thread = find_brk_or_stopped_thread(process)
        process.SetSelectedThread(thread)
        frame = thread.GetFrameAtIndex(0)
        pc = frame.FindRegister("pc").GetValueAsUnsigned()
        x0 = frame.FindRegister("x0").GetValueAsUnsigned()
        x1 = frame.FindRegister("x1").GetValueAsUnsigned()
        x16 = frame.FindRegister("x16").GetValueAsUnsigned()

        err = lldb.SBError()
        raw = process.ReadMemory(pc, 4, err) if pc != 0 else b""
        if not err.Success() or len(raw) != 4:
            desc = thread.GetStopDescription(128)
            print(f"[txm-jit] Stop #{step}: tid={thread.GetIndexID()} pc={pc:#x} ({desc}); continuing", flush=True)
            continue

        insn = struct.unpack("<I", raw)[0]
        if (insn & 0xFFE0001F) != 0xD4200000:
            desc = thread.GetStopDescription(128)
            print(f"[txm-jit] Stop #{step}: non-BRK stop tid={thread.GetIndexID()} pc={pc:#x} insn={insn:#010x} ({desc}); continuing", flush=True)
            continue

        imm = (insn >> 5) & 0xFFFF
        print(f"[txm-jit] Stop #{step}: BRK #{imm:#x} tid={thread.GetIndexID()} at pc={pc:#x} (x0={x0:#x}, x1={x1:#x}, x16={x16:#x})", flush=True)

        if imm == 0x69:
            write_reg(debugger, process, thread, "x0", 0xE0000069)
            write_reg(debugger, process, thread, "pc", pc + 4)
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
                write_reg(debugger, process, thread, "x0", rx_addr)
                write_reg(debugger, process, thread, "pc", pc + 4)
            elif x16 == 0:
                write_reg(debugger, process, thread, "pc", pc + 4)
                print("[txm-jit] Servicing CMD_DETACH -> detaching LLDB cleanly!", flush=True)
                err = process.Detach()
                if not err.Success():
                    raise RuntimeError(f"Detach failed: {err.GetCString()}")
                detached = True
                break
            else:
                write_reg(debugger, process, thread, "pc", pc + 4)
        else:
            write_reg(debugger, process, thread, "pc", pc + 4)

    if not detached:
        raise RuntimeError("Exited loop without seeing CMD_DETACH")
    print("[txm-jit] SUCCESS: TXM JIT + Ledger-Exempt Guest RAM active and debugger detached!", flush=True)


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f _lldb_txm_jit.txm_jit_cmd txm_jit_run")
