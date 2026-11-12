#!/opt/homebrew/bin/python3
"""
End-to-end post-OOBE verification, sparse packing, iPhone Duo staging/transfer,
TXM JIT + 4096 MiB RAM launch, and live Windows 11 Pro 24H2 ARM64 Desktop capture
on Alan's physical foldable iPhone Duo ($WINIOS_DUO_UDID or ~/.config/windowsios/duo-udid).
"""

import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "work"
WINVM = ROOT / "build" / "winvm"
HVSPARSE = ROOT / "build" / "hvsparse"
LAUNCH_DUO = ROOT / "scripts" / "launch_on_duo.sh"

def configured_duo_udid() -> str:
    """$WINIOS_DUO_UDID, else ~/.config/windowsios/duo-udid; kept out of the repo."""
    udid = os.environ.get("WINIOS_DUO_UDID", "").strip()
    if not udid:
        cfg = Path.home() / ".config" / "windowsios" / "duo-udid"
        udid = cfg.read_text().strip() if cfg.exists() else ""
    return udid


UDID = configured_duo_udid() or sys.exit("set WINIOS_DUO_UDID or write it to ~/.config/windowsios/duo-udid")
BUNDLE_ID = "com.RRGSyndicate.WindowsiOS"

CODE_FD = WORK / "code.fd"
VARS_CLEAN = WORK / "vars_clean.fd"
VARS_MAC = WORK / "vars_mac_verify.fd"
WIN_IMG = WORK / "windows11.img"
WIN_SPARSE = WORK / "windows11.hvsparse"
MAC_DESKTOP_PNG = WORK / "mac_win11_desktop.png"
DUO_DESKTOP_PNG = WORK / "duo_win11_desktop.png"
DUO_STATUS_TXT = WORK / "duo_boot_status.txt"


def log(msg: str):
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def run(cmd: list[str], check: bool = True, **kwargs):
    log(f"+ {' '.join(str(c) for c in cmd)}")
    return subprocess.run([str(c) for c in cmd], check=check, **kwargs)


def analyze_png_is_desktop(png_path: Path) -> tuple[bool, str]:
    """
    Use macOS `sips -s format bmp` to inspect actual pixel brightness & color diversity
    so we distinguish black/dark-blue OOBE/boot screens ("Just a moment...", "Hi",
    "Getting things ready for you") from the real Windows 11 Bloom Desktop + Taskbar.
    """
    if not png_path.exists() or png_path.stat().st_size < 40_000:
        sz = png_path.stat().st_size if png_path.exists() else 0
        return False, f"png_size={sz}B (<40KB, still boot/spinner)"

    bmp_tmp = WORK / "_analyze_tmp.bmp"
    bmp_tmp.unlink(missing_ok=True)
    rc = subprocess.run(
        ["sips", "-s", "format", "bmp", str(png_path), "--out", str(bmp_tmp)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode
    if rc != 0 or not bmp_tmp.exists():
        return False, "sips bmp conversion failed"

    data = bmp_tmp.read_bytes()
    bmp_tmp.unlink(missing_ok=True)
    if len(data) < 54 or data[:2] != b"BM":
        return False, "invalid bmp header"

    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height_raw = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    top_down = height_raw < 0
    height = abs(height_raw)
    bytes_pp = bpp // 8
    if width <= 0 or height <= 0 or bytes_pp < 3:
        return False, f"unexpected bmp {width}x{height}@{bpp}bpp"

    row_stride = ((width * bytes_pp + 3) // 4) * 4

    def get_rgb(x: int, y: int) -> tuple[int, int, int]:
        bmp_row = y if top_down else (height - 1 - y)
        off = pixel_offset + bmp_row * row_stride + x * bytes_pp
        if off + 3 > len(data):
            return (0, 0, 0)
        b, g, r = data[off], data[off + 1], data[off + 2]
        return (r, g, b)

    # Sample wallpaper area (y in 10%..80%) and taskbar strip (y in 94%..99%)
    bright = 0
    colorful = 0
    dark = 0
    samples = 0
    sum_r = sum_g = sum_b = 0
    for sy in range(int(height * 0.10), int(height * 0.80), 8):
        for sx in range(int(width * 0.05), int(width * 0.95), 8):
            r, g, b = get_rgb(sx, sy)
            samples += 1
            sum_r += r
            sum_g += g
            sum_b += b
            lum = (r + g + b) // 3
            if lum > 140:
                bright += 1
            if lum < 30:
                dark += 1
            if max(r, g, b) - min(r, g, b) > 25:
                colorful += 1

    if samples == 0:
        return False, "no samples"

    mean_r = sum_r / samples
    mean_g = sum_g / samples
    mean_b = sum_b / samples
    bright_frac = bright / samples
    dark_frac = dark / samples
    color_frac = colorful / samples

    # Sample taskbar strip near bottom (Windows 11 light taskbar is light gray/mica ~210-245)
    tb_samples = 0
    tb_sum = 0
    for sy in range(height - 32, height - 6, 4):
        for sx in range(int(width * 0.2), int(width * 0.8), 8):
            r, g, b = get_rgb(sx, sy)
            tb_samples += 1
            tb_sum += (r + g + b) / 3.0
    tb_lum = tb_sum / max(tb_samples, 1)

    desc = (
        f"png={png_path.stat().st_size // 1024}KB {width}x{height} "
        f"meanRGB=({mean_r:.0f},{mean_g:.0f},{mean_b:.0f}) "
        f"bright={bright_frac:.2f} color={color_frac:.2f} dark={dark_frac:.2f} tbLum={tb_lum:.0f}"
    )

    # Windows 11 Default Light Bloom Desktop has bright light-blue/white background
    # (bright_frac ~0.53, color_frac ~1.00, dark_frac ~0.04, png ~480KB vs <0.03 bright on boot/OOBE/Welcome)
    is_desktop = (
        png_path.stat().st_size >= 180_000
        and bright_frac > 0.30
        and color_frac > 0.25
        and dark_frac < 0.30
    )
    return is_desktop, desc


def wait_for_oobe_completion():
    log("Step 1: Waiting for provision_windows_arm64.py (phase2_oobe) to finish cleanly...")
    while True:
        if VARS_CLEAN.exists():
            # Ensure no winvm process still holds windows11.img
            out = subprocess.run(
                ["pgrep", "-f", "build/winvm"],
                capture_output=True,
                text=True,
            ).stdout.strip()
            if not out:
                log(f"vars_clean.fd ready ({VARS_CLEAN.stat().st_size} bytes) and winvm exited cleanly!")
                return
        prov_png = WORK / "prov_phase2.png"
        sz = prov_png.stat().st_size if prov_png.exists() else 0
        log(f"  still in phase2_oobe (prov_phase2.png={sz} bytes)...")
        time.sleep(10)


def verify_mac_clean_desktop():
    log("Step 2: Booting provisioned Windows 11 Pro ARM64 on Mac to verify clean Desktop...")
    shutil.copyfile(VARS_CLEAN, VARS_MAC)
    cmd_file = WORK / "mac_verify.cmds"
    log_file = WORK / "mac_verify.log"
    cmd_file.write_text("")
    MAC_DESKTOP_PNG.unlink(missing_ok=True)

    cmd = [
        str(WINVM),
        "--headless",
        "--code", str(CODE_FD),
        "--vars", str(VARS_MAC),
        "--disk", str(WIN_IMG),
        "--cpus", "4",
        "--ram", "4096",
        "--no-net",
        "--cmd-file", str(cmd_file),
        "--screenshot", str(MAC_DESKTOP_PNG),
    ]
    t0 = time.time()
    desktop_hits = 0
    with open(log_file, "wb") as lf:
        proc = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT)
        try:
            while proc.poll() is None:
                time.sleep(8)
                elapsed = int(time.time() - t0)
                with open(cmd_file, "a", encoding="utf-8") as cf:
                    cf.write(f"screenshot {MAC_DESKTOP_PNG}\n")
                time.sleep(2)
                is_desk, desc = analyze_png_is_desktop(MAC_DESKTOP_PNG)
                log(f"  [mac +{elapsed}s] desktop={is_desk} ({desc})")
                if is_desk:
                    desktop_hits += 1
                    if desktop_hits >= 2:
                        # Save verified Mac desktop screenshot to a safe copy before shutting down!
                        verified_copy = WORK / "mac_win11_desktop_verified.png"
                        shutil.copyfile(MAC_DESKTOP_PNG, verified_copy)
                        log("  Mac Windows 11 Desktop confirmed! Triggering clean ACPI shutdown...")
                        with open(cmd_file, "a", encoding="utf-8") as cf:
                            cf.write("power\n")
                        # Wait up to 60s for clean ACPI shutdown
                        for _ in range(30):
                            if proc.poll() is not None:
                                break
                            time.sleep(2)
                        if proc.poll() is None:
                            log("  ACPI power button blocked by first-run toast; sending quit (flushes disks cleanly)...")
                            with open(cmd_file, "a", encoding="utf-8") as cf:
                                cf.write("quit\n")
                            proc.wait(timeout=20)
                        shutil.copyfile(verified_copy, MAC_DESKTOP_PNG)
                        verified_copy.unlink(missing_ok=True)
                        log(f"  Mac verification exited cleanly (rc={proc.returncode}).")
                        return
                if elapsed > 600:
                    raise TimeoutError("Mac clean desktop verification exceeded 600s")
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=10)


def pack_sparse_image():
    log("Step 3: Packing windows11.img -> windows11.hvsparse...")
    WIN_SPARSE.unlink(missing_ok=True)
    run([str(HVSPARSE), "pack", str(WIN_IMG), str(WIN_SPARSE)])
    sz_gb = WIN_SPARSE.stat().st_size / (1024 ** 3)
    log(f"  Packed {WIN_SPARSE.name}: {sz_gb:.2f} GiB")


def copy_to_iphone_duo(only_sparse: bool = False):
    log(f"Step 4: Copying machine files to iPhone Duo ({UDID})...")
    transfers = [
        (WIN_SPARSE, "Documents/Machines/Win11/windows11.hvsparse"),
    ]
    if not only_sparse:
        transfers = [
            (CODE_FD, "Documents/Machines/Win11/code.fd"),
            (VARS_MAC if VARS_MAC.exists() else VARS_CLEAN, "Documents/Machines/Win11/vars.fd"),
            *transfers,
        ]
    for src, dst in transfers:
        t0 = time.time()
        for attempt in range(1, 5):
            r = subprocess.run(
                [
                    "xcrun", "devicectl", "device", "copy", "to",
                    "--device", UDID,
                    "--domain-type", "appDataContainer",
                    "--domain-identifier", BUNDLE_ID,
                    "--source", str(src),
                    "--destination", dst,
                ],
                capture_output=True,
                text=True,
            )
            if r.returncode == 0:
                dt = time.time() - t0
                mb = src.stat().st_size / (1024 ** 2)
                log(f"  Copied {src.name} ({mb:.1f} MiB) -> {dst} in {dt:.1f}s")
                break
            log(f"  Copy attempt {attempt} failed for {src.name}: {r.stderr.strip() or r.stdout.strip()}")
            if attempt == 4:
                raise RuntimeError(f"Failed to copy {src} to iPhone Duo")
            time.sleep(3)


def pull_duo_file(remote_rel: str, local_dst: Path) -> bool:
    r = subprocess.run(
        [
            "xcrun", "devicectl", "device", "copy", "from",
            "--device", UDID,
            "--domain-type", "appDataContainer",
            "--domain-identifier", BUNDLE_ID,
            "--source", remote_rel,
            "--destination", str(local_dst),
        ],
        capture_output=True,
        text=True,
    )
    return r.returncode == 0


def launch_and_verify_on_duo():
    log("Step 5: Launching WindowsiOS on iPhone Duo with 256 MiB TXM JIT + 4096 MiB RAM...")
    for attempt in range(1, 4):
        r = run(["bash", str(LAUNCH_DUO), UDID, BUNDLE_ID], check=False)
        if r.returncode == 0:
            break
        log(f"  launch_on_duo.sh attempt {attempt} exited with {r.returncode}; retrying in 3s...")
        if attempt == 3:
            raise RuntimeError(f"launch_on_duo.sh failed after 3 attempts (rc={r.returncode})")
        time.sleep(3)

    log("Step 6: Monitoring sparse NVMe unpack & Windows 11 Pro ARM64 boot on iPhone Duo...")
    t0 = time.time()
    desktop_hits = 0
    while True:
        time.sleep(10)
        elapsed = int(time.time() - t0)
        DUO_STATUS_TXT.unlink(missing_ok=True)
        status_line = "status unavailable"
        if pull_duo_file("Documents/boot_status.txt", DUO_STATUS_TXT):
            kv = {}
            for line in DUO_STATUS_TXT.read_text(errors="replace").splitlines():
                if ":" in line:
                    k, v = line.split(":", 1)
                    kv[k.strip()] = v.strip()
            status_line = (
                f"vm_running={kv.get('vm_running')} status='{kv.get('vm_status')}' "
                f"uptime={kv.get('uptime_sec')}s fb={kv.get('fb_size')} "
                f"frames={kv.get('frames_presented')} avail={kv.get('avail_mem_mb')}MB "
                f"screen={kv.get('screen_pts')}"
            )

        tmp_png = WORK / "_duo_latest.png"
        tmp_png.unlink(missing_ok=True)
        desk_info = "no latest_screen.png yet"
        is_desk = False
        if pull_duo_file("Documents/latest_screen.png", tmp_png) and tmp_png.exists():
            shutil.copyfile(tmp_png, DUO_DESKTOP_PNG)
            tmp_png.unlink(missing_ok=True)
            is_desk, desk_info = analyze_png_is_desktop(DUO_DESKTOP_PNG)

        log(f"  [duo +{elapsed}s] {status_line} | {desk_info}")

        if is_desk:
            desktop_hits += 1
            if desktop_hits >= 4:
                log("SUCCESS: Live Windows 11 Pro 24H2 ARM64 Desktop confirmed on Alan's iPhone Duo!")
                return

        if elapsed > 1800:
            raise TimeoutError("iPhone Duo unpack + Windows 11 Desktop boot exceeded 1800s")


def main():
    if "--skip-mac-verify" not in sys.argv:
        wait_for_oobe_completion()
        verify_mac_clean_desktop()
    if "--skip-pack" not in sys.argv:
        pack_sparse_image()
    if "--skip-copy" not in sys.argv:
        copy_to_iphone_duo(only_sparse=("--only-sparse" in sys.argv))
    launch_and_verify_on_duo()


if __name__ == "__main__":
    main()
