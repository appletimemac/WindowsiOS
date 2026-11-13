#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Benchmark matrix on the Mac runner (build/winvm --headless).

Every run boots a fresh APFS clone of the guest image, waits for the desktop to go quiet, starts
scripts/bench/winios_bench.ps1 from a private copy of the tools disk and collects the guest's result
file (PUT to the share) and the runner's perf CSV. Runs of different configurations are interleaved
(A B A B), never all A then all B.

  scripts/bench_mac.py --jit 384,768 --rounds 3 --label jit
  scripts/bench_mac.py --size 1426x952,952x636 --cpus 4,3 --qos default,ui
  scripts/bench_mac.py --aa 5                 same configuration 5 times: the noise floor
  scripts/bench_mac.py --summarize DIR        rebuild the summary of a finished results directory

Axes (comma lists; the matrix is their product): --jit MiB, --size WxH, --cpus N, --qos default|ui|init
(the HV_QOS variable of the runner). Output goes to build/bench/<label>-<time>/ (or --out): one folder
per run (cmds, winvm.log, perf.csv, bench.txt, run.json), summary.txt and summary.csv.

The guest image, its vars and the tools disk are never written: the runs use clones, and the tools disk
is a private copy of work/tools.img with the bench script added. Only files this script created are
deleted, and a VM it started is stopped when it exits for any reason.
"""
import argparse
import atexit
import bisect
import csv
import datetime
import itertools
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(os.path.expanduser('~'), 'build', 'windowsios', 'work')
BENCH_PS1 = os.path.join(ROOT, 'scripts', 'bench', 'winios_bench.ps1')
RESULT_NAME = 'winios_bench_result.txt'

created_files = []
created_dirs = []
running = []


def log(msg):
    print('[%s] %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


def stop_vm(proc, grace=60):
    if proc.poll() is not None:
        return
    for sig, wait in ((signal.SIGTERM, grace), (signal.SIGKILL, 10)):
        try:
            os.killpg(proc.pid, sig)
        except ProcessLookupError:
            return
        try:
            proc.wait(wait)
            return
        except subprocess.TimeoutExpired:
            pass


def cleanup():
    for proc in running:
        stop_vm(proc, grace=10)
    for path in created_files:
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
        except OSError as e:
            print('bench_mac: could not delete %s: %s' % (path, e), file=sys.stderr)
    for path in created_dirs:
        shutil.rmtree(path, ignore_errors=True)
    del running[:], created_files[:], created_dirs[:]


def on_signal(signum, frame):
    sys.exit(128 + signum)


def forget(path):
    """Delete one file or folder this script made, now."""
    if path in created_files:
        created_files.remove(path)
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
    elif path in created_dirs:
        created_dirs.remove(path)
        shutil.rmtree(path, ignore_errors=True)


# ---- configuration ----

def parse_size(text):
    m = re.fullmatch(r'(\d+)x(\d+)', text)
    if not m:
        raise argparse.ArgumentTypeError('size must be WxH: %r' % text)
    return int(m.group(1)), int(m.group(2))


def csv_list(conv):
    def parse(text):
        return [conv(v) for v in text.split(',') if v]
    return parse


def qos_name(text):
    if text not in ('default', 'ui', 'init'):
        raise argparse.ArgumentTypeError('qos must be default, ui or init: %r' % text)
    return text


def config_id(c):
    name = 'jit%d_%dx%d_c%d_%s' % (c['jit'], c['size'][0], c['size'][1], c['cpus'], c['qos'])
    return name if c['runner'][0] == 'base' else name + '_' + c['runner'][0]


def build_configs(args):
    combos = itertools.product(args.runner, args.jit, args.size, args.cpus, args.qos)
    return [dict(runner=r, jit=j, size=s, cpus=n, qos=q) for r, j, s, n, q in combos]


def parse_runners(text):
    """'path' or 'name=path,name=path': builds of the runner to compare (one name is 'base' unless all are named)."""
    out = []
    for i, item in enumerate(x for x in text.split(',') if x):
        name, _, path = item.rpartition('=')
        out.append((name or ('base' if i == 0 else 'r%d' % i), os.path.expanduser(path)))
    return out


# ---- tools disk ----

def build_tools_disk(src):
    """A private copy of the tools disk with the bench script on it: src itself is never written."""
    dst = '/tmp/wb_tools_%d.img' % os.getpid()
    mnt = '/tmp/wb_mnt_%d' % os.getpid()
    shutil.copyfile(src, dst)
    created_files.append(dst)
    os.makedirs(mnt)
    created_dirs.append(mnt)
    r = subprocess.run(['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-noverify',
                        '-mountpoint', mnt, dst], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('hdiutil attach %s: %s' % (dst, r.stderr.strip()))
    dev = r.stdout.split()[0]
    try:
        shutil.copyfile(BENCH_PS1, os.path.join(mnt, 'winios_bench.ps1'))
        for name in os.listdir(mnt):  # what macOS leaves on a FAT volume
            p = os.path.join(mnt, name)
            if name.startswith('._') or name in ('.fseventsd', '.Spotlight-V100', '.Trashes', '.DS_Store'):
                shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else os.remove(p)
        subprocess.run(['sync'])
    finally:
        for attempt in range(4):
            d = subprocess.run(['hdiutil', 'detach', '-quiet'] + (['-force'] if attempt >= 2 else []) + [dev],
                               capture_output=True)
            if d.returncode == 0:
                break
            time.sleep(1)
        else:
            raise RuntimeError('could not detach %s' % dev)
    forget(mnt)
    return dst


# ---- one run ----

def clone(src, n):
    d, base = os.path.split(src)
    stem, ext = os.path.splitext(base)
    dst = os.path.join(d, '%s.wb%d_%d%s' % (stem, os.getpid(), n, ext))
    created_files.append(dst)
    t = time.time()
    subprocess.run(['cp', '-c', src, dst], check=True)
    if time.time() - t > 20:
        log('warning: cloning %s took %.0f s: is %s on APFS?' % (base, time.time() - t, d))
    return dst


def append_cmds(path, lines):
    with open(path, 'a') as f:
        f.write(''.join(l + '\n' for l in lines))


def guest_cmd_lines(cfg, args):
    ps = '& ((Get-Volume -FileSystemLabel WINIOSTOOLS).DriveLetter + \':\\winios_bench.ps1\')'
    if args.quick:
        ps += ' -Quick'
    w, h = cfg['size']
    return [
        'size %d %d' % (w, h),
        'mark vm_start',
        'wait %d' % (args.boot_min * 1000),
        'settle %d %d' % (args.boot_quiet * 1000, args.boot_max * 1000),
        'mark boot',
        'wait %d' % (args.post_boot_wait * 1000),
        'mark bench_launch',
        'winr',
        'wait 3000',
        'type powershell -ep bypass -c "%s"' % ps,
        'enter',
        'mark bench_typed',
    ]


def read_text(path):
    try:
        with open(path, 'r', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


CLOCK_RE = re.compile(r'^# (?:begin \S+|end \S+|start|finish) (\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d+)\s*$')


def last_clock(text):
    """Guest clock (naive ms since epoch) of the last section boundary in a result file, or None."""
    for line in reversed(text.splitlines()):
        m = CLOCK_RE.match(line)
        if m:
            return guest_ms(m.group(1))
    return None


def guest_ms(stamp):
    d = datetime.datetime.strptime(stamp[:23], '%Y-%m-%dT%H:%M:%S.%f')
    return d.replace(tzinfo=datetime.timezone.utc).timestamp() * 1000.0


def run_one(cfg, n, total, rnd, args, outdir, tools_img):
    cid = config_id(cfg)
    rid = 'r%02d_%s' % (rnd, cid)
    rdir = os.path.join(outdir, rid)
    os.makedirs(rdir)
    log('run %d/%d: round %d %s' % (n, total, rnd, cid))
    img = clone(args.image, n)
    vars_ = clone(args.vars, n)
    share = tempfile.mkdtemp(prefix='wb_', dir='/tmp')
    created_dirs.append(share)
    cmds = os.path.join(rdir, 'cmds')
    perf = os.path.join(rdir, 'perf.csv')
    logf = os.path.join(rdir, 'winvm.log')
    append_cmds(cmds, guest_cmd_lines(cfg, args))

    deadline_s = args.boot_min + args.boot_max + args.post_boot_wait + 60 + args.bench_timeout
    cmd = [cfg['runner'][1], '--headless', '--code', args.code, '--vars', vars_, '--disk', img, '--disk1', tools_img,
           '--cpus', str(cfg['cpus']), '--ram', str(args.ram), '--jit', str(cfg['jit']), '--share', share,
           '--perf-csv', perf, '--perf-ms', str(args.perf_ms), '--cmd-file', cmds, '--timeout', str(deadline_s + 90)]
    env = dict(os.environ, HV_QOS=cfg['qos'])
    res = {'id': rid, 'round': rnd, 'config': cfg, 'cid': cid, 'flags': [], 'metrics': {}}
    result_path = os.path.join(share, RESULT_NAME)
    first = last = None  # (mtime_ms, guest clock ms) of the first and the latest state of the result file
    seen = None
    done = False
    t_start = time.time()
    t_typed = None
    t_typed_check = 0
    with open(logf, 'w') as lf:
        proc = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    running.append(proc)
    try:
        while True:
            time.sleep(0.25)
            try:
                st = os.stat(result_path)
            except FileNotFoundError:
                st = None
            if st is not None and (st.st_mtime_ns, st.st_size) != seen:
                seen = (st.st_mtime_ns, st.st_size)
                text = read_text(result_path)
                g = last_clock(text)
                if g is not None:
                    last = (st.st_mtime_ns / 1e6, g)
                    if first is None:
                        first = last
                if re.search(r'^done\s*$', text, re.M):
                    done = True
                    break
            if t_typed is None and time.time() - t_typed_check > 5:
                t_typed_check = time.time()
                if re.search(r'^MARK \d+ bench_typed$', read_text(logf), re.M):
                    t_typed = time.time()
            if t_typed is not None and seen is None and time.time() - t_typed > args.start_timeout:
                res['flags'].append('bench_never_started')
                break
            if proc.poll() is not None:
                res['flags'].append('vm_exited_early')
                break
            if time.time() - t_start > deadline_s:
                res['flags'].append('bench_timeout')
                break
        if proc.poll() is None:
            append_cmds(cmds, ['mark bench_end' if done else 'mark bench_abort', 'quit'])
            try:
                proc.wait(90)
            except subprocess.TimeoutExpired:
                res['flags'].append('vm_killed')
                stop_vm(proc, grace=15)
    finally:
        stop_vm(proc, grace=15)
        if proc in running:
            running.remove(proc)
    res['wall_s'] = round(time.time() - t_start, 1)
    res['exit'] = proc.returncode

    bench_txt = os.path.join(rdir, 'bench.txt')
    if os.path.exists(result_path):
        shutil.copyfile(result_path, bench_txt)
    forget(share)
    forget(img)
    forget(vars_)

    text = read_text(bench_txt)
    res['bench_done'] = done
    res['metrics'].update(parse_guest(text))
    res['metrics'].update(parse_perf_run(perf, text, first, last, cfg))
    m = res['metrics']
    if m.get('boot.settle_timeout') == 0.0 and 'boot.ms' in m:
        m['boot.desktop_ms'] = m['boot.ms'] - args.boot_quiet * 1000  # when the display went quiet
    log('  done in %.0f s: %d metrics%s' % (res['wall_s'], len(res['metrics']),
                                            (', flags: ' + ','.join(res['flags'])) if res['flags'] else ''))
    with open(os.path.join(rdir, 'run.json'), 'w') as f:
        json.dump(res, f, indent=1, sort_keys=True)
    return res


# ---- parsing ----

NUM_RE = re.compile(r'^(\S+) (-?\d+(?:\.\d+)?)$')
ROUND_RE = re.compile(r'\.r\d+$')


def parse_guest(text):
    """'name value' lines to {metric: value}: rounds (.rN) of one metric collapse to their median."""
    groups = {}
    for line in text.splitlines():
        m = NUM_RE.match(line.strip())
        if m:
            groups.setdefault(ROUND_RE.sub('', m.group(1)), []).append(float(m.group(2)))
    out = {}
    for name, vals in groups.items():
        out['guest.' + name] = statistics.median(vals)
    for line in text.splitlines():
        m = re.match(r'^(\S+) timeout$', line.strip())
        if m:
            key = 'guest.' + ROUND_RE.sub('', m.group(1)) + '.timeouts'
            out[key] = out.get(key, 0) + 1
    return out


def read_perf(path):
    comment, marks, cols, rows = '', [], None, []
    for line in read_text(path).splitlines():
        if line.startswith('#mark '):
            parts = line.split(' ', 2)
            marks.append((int(parts[1]), parts[2] if len(parts) > 2 else ''))
        elif line.startswith('# '):
            comment = line[2:]
        elif line.startswith('t_ms,'):
            cols = line.split(',')
        elif cols and line and line[0].isdigit():
            vals = line.split(',')
            if len(vals) == len(cols):
                rows.append([int(v) for v in vals])
    return comment, marks, ({c: i for i, c in enumerate(cols)} if cols else {}), rows


def window_metrics(prefix, rows, col, t0, t1, ncpus):
    """Rates and totals between the rows around host times t0 and t1 (wall-clock ms)."""
    if len(rows) < 2 or t1 <= t0:
        return {}
    ts = [r[col['t_ms']] for r in rows]
    i0 = max(bisect.bisect_right(ts, t0) - 1, 0)
    i1 = min(bisect.bisect_left(ts, t1), len(rows) - 1)
    if i1 <= i0:
        return {}
    a, b = rows[i0], rows[i1]
    dt = (b[col['t_ms']] - a[col['t_ms']]) / 1000.0
    if dt <= 0:
        return {}

    def d(name):
        return b[col[name]] - a[col[name]]

    m = {}
    p = 'perf.%s.' % prefix
    m[p + 'seconds'] = dt
    m[p + 'translations/s'] = d('translations') / dt
    m[p + 'block_flushes'] = d('block_flushes')
    m[p + 'flush_smc/s'] = d('flush_smc') / dt
    if d('chained') > 0:
        m[p + 'chain_far/chained'] = d('chain_far') / d('chained')
    m[p + 'stw_count'] = d('stw_count')
    m[p + 'stw_ms'] = d('stw_ns') / 1e6
    m[p + 'stw_max_ms'] = max(r[col['stw_max_us']] for r in rows[i0 + 1:i1 + 1]) / 1000.0
    wfi = sum(d('wfi%d_ns' % i) for i in range(min(ncpus, 4)))
    run = sum(d('run%d_ns' % i) for i in range(min(ncpus, 4)))
    m[p + 'wfi_idle%'] = 100.0 * wfi / (ncpus * dt * 1e9)
    m[p + 'vcpu_cpu%'] = 100.0 * run / (ncpus * dt * 1e9)
    m[p + 'gpu_flush_px/s'] = d('gpu_flush_px') / dt
    m[p + 'gpu_flushes/s'] = d('gpu_flushes') / dt
    return m


def parse_perf_run(path, bench_text, first, last, cfg):
    comment, marks, col, rows = read_perf(path)
    bench_text = bench_text.replace('\r', '')
    out = {}
    if not rows:
        return out
    by_label = {}
    for t, label in marks:
        by_label.setdefault(label.split(' ')[0], []).append((t, label))
    t_first = rows[0][col['t_ms']]
    if 'boot' in by_label:
        out['boot.ms'] = by_label['boot'][0][0] - t_first
    if 'settle' in by_label:
        m = re.match(r'settle (\d+) (\w+)', by_label['settle'][0][1])
        if m:
            out['boot.settle_ms'] = float(m.group(1))
            out['boot.settle_timeout'] = 1.0 if m.group(2) == 'timeout' else 0.0
    out['perf.rows'] = float(len(rows))

    # Section boundaries are the guest's clock; two anchors (the first and the latest PUT of the result
    # file, host mtime against guest clock) turn it into host wall-clock time, drift included.
    def to_host(g):
        if not first:
            return None
        if last and last[1] != first[1]:
            return first[0] + (g - first[1]) * (last[0] - first[0]) / (last[1] - first[1])
        return first[0] + (g - first[1])

    bounds = {}
    for m in re.finditer(r'^# (begin|end) (\S+) (\S+)$', bench_text, re.M):
        bounds.setdefault(m.group(2), {})[m.group(1)] = guest_ms(m.group(3))
    start = re.search(r'^# start (\S+)$', bench_text, re.M)
    finish = re.search(r'^# finish (\S+)$', bench_text, re.M)
    if first and start:
        end_g = guest_ms(finish.group(1)) if finish else (last[1] if last else None)
        if end_g is not None:
            out.update(window_metrics('bench', rows, col, to_host(guest_ms(start.group(1))), to_host(end_g), cfg['cpus']))
    idle = bounds.get('idle', {})
    if first and 'begin' in idle and 'end' in idle:
        out.update(window_metrics('idle', rows, col, to_host(idle['begin']), to_host(idle['end']), cfg['cpus']))
    return out


# ---- summary ----

def group_order(name):
    for i, p in enumerate(('boot.', 'guest.idle', 'guest.launch', 'guest.compute', 'guest.jitter',
                           'perf.idle.', 'perf.bench.', 'perf.')):
        if name.startswith(p):
            return (i, name)
    return (99, name)


def fmt(v):
    if v is None:
        return '-'
    a = abs(v)
    if a >= 1000:
        return '%.0f' % v
    if a >= 100:
        return '%.1f' % v
    if a >= 1:
        return '%.2f' % v
    return '%.3f' % v


def stats(vals):
    med = statistics.median(vals)
    spread = 100.0 * (max(vals) - min(vals)) / abs(med) if med else 0.0
    cv = 100.0 * statistics.stdev(vals) / abs(statistics.mean(vals)) if len(vals) > 1 and statistics.mean(vals) else 0.0
    return med, min(vals), max(vals), spread, cv


def summarize(results, outdir):
    cids = []
    for r in results:
        if r['cid'] not in cids:
            cids.append(r['cid'])
    names = sorted({m for r in results for m in r['metrics']}, key=group_order)
    table = {}  # (cid, metric) -> list of values
    for r in results:
        for k, v in r['metrics'].items():
            table.setdefault((r['cid'], k), []).append(v)
    runs = {c: sum(1 for r in results if r['cid'] == c) for c in cids}

    with open(os.path.join(outdir, 'summary.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['config', 'metric', 'n', 'median', 'min', 'max', 'spread_pct', 'cv_pct', 'values'])
        for c in cids:
            for k in names:
                vals = table.get((c, k))
                if vals:
                    med, lo, hi, spread, cv = stats(vals)
                    w.writerow([c, k, len(vals), med, lo, hi, '%.2f' % spread, '%.2f' % cv,
                                ' '.join('%g' % v for v in vals)])

    lines = []
    bad = [r for r in results if r['flags'] or not r.get('bench_done')]
    lines.append('runs: %d, configs: %d%s' % (len(results), len(cids),
                                              (', incomplete: ' + ', '.join('%s(%s)' % (r['id'], ','.join(r['flags']) or 'no done line')
                                                                           for r in bad)) if bad else ''))
    lines.append('cell: median [min-max]; n shown when it is less than the run count')
    if len(cids) == 1:
        c = cids[0]
        lines.append('')
        lines.append('config %s, %d runs' % (c, runs[c]))
        width = max(len(k) for k in names) if names else 10
        lines.append('%-*s %3s %10s %10s %10s %8s %7s' % (width, 'metric', 'n', 'median', 'min', 'max', 'spread%', 'cv%'))
        for k in names:
            vals = table.get((c, k))
            if vals:
                med, lo, hi, spread, cv = stats(vals)
                lines.append('%-*s %3d %10s %10s %10s %8.1f %7.1f' % (width, k, len(vals), fmt(med), fmt(lo), fmt(hi), spread, cv))
    else:
        per_block = 3
        for b in range(0, len(cids), per_block):
            block = cids[b:b + per_block]
            lines.append('')
            width = max(len(k) for k in names) if names else 10
            head = '%-*s' % (width, 'metric')
            for c in block:
                head += '  %-34s' % ('%s (%d runs)' % (c, runs[c]))
            lines.append(head)
            for k in names:
                row = '%-*s' % (width, k)
                for c in block:
                    vals = table.get((c, k))
                    if vals:
                        med, lo, hi, spread, cv = stats(vals)
                        cell = '%s [%s-%s]' % (fmt(med), fmt(lo), fmt(hi))
                        if len(vals) < runs[c]:
                            cell += ' n=%d' % len(vals)
                    else:
                        cell = '-'
                    row += '  %-34s' % cell
                lines.append(row)
    text = '\n'.join(lines) + '\n'
    with open(os.path.join(outdir, 'summary.txt'), 'w') as f:
        f.write(text)
    return text


def load_results(outdir):
    out = []
    for name in sorted(os.listdir(outdir)):
        p = os.path.join(outdir, name, 'run.json')
        if os.path.exists(p):
            with open(p) as f:
                out.append(json.load(f))
    return out


# ---- main ----

def main():
    ap = argparse.ArgumentParser(description='Benchmark matrix on the Mac runner.',
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__.split('\n\n', 1)[1])
    ap.add_argument('--jit', type=csv_list(int), default=[768], help='JIT region MiB (default 768)')
    ap.add_argument('--size', type=csv_list(parse_size), default=[(1426, 952)], help='display WxH (default 1426x952)')
    ap.add_argument('--cpus', type=csv_list(int), default=[4], help='vCPU count (default 4)')
    ap.add_argument('--qos', type=csv_list(qos_name), default=['ui'],
                    help='HV_QOS: default, ui or init (default ui, what the runner uses when HV_QOS is unset)')
    ap.add_argument('--rounds', type=int, default=1, help='interleaved rounds over the whole matrix (default 1)')
    ap.add_argument('--aa', type=int, metavar='N', help='run the first configuration N times: the noise floor')
    ap.add_argument('--label', default='bench')
    ap.add_argument('--out', help='results directory (default build/bench/<label>-<time>)')
    ap.add_argument('--summarize', metavar='DIR', help='only rebuild summary.txt and summary.csv of DIR')
    ap.add_argument('--dry-run', action='store_true', help='print the run plan and stop')
    ap.add_argument('--quick', action='store_true', help='short guest bench (1 round, 3 s idle): a smoke test')
    ap.add_argument('--image', default=os.path.join(WORK, 'vg_new6.img'))
    ap.add_argument('--vars', default=os.path.join(WORK, 'vg_new6_vars.fd'))
    ap.add_argument('--code', default=os.path.join(WORK, 'code.fd'))
    ap.add_argument('--tools-img', default=os.path.join(WORK, 'tools.img'), help='copied, never written')
    ap.add_argument('--runner', type=parse_runners, default=[('base', os.path.join(ROOT, 'build', 'winvm'))],
                    help="runner build, or several to compare: name=path,name=path (an axis like --jit)")
    ap.add_argument('--ram', type=int, default=4096, help='guest RAM MiB (default 4096)')
    ap.add_argument('--perf-ms', type=int, default=1000, help='perf row interval (default 1000)')
    ap.add_argument('--boot-min', type=int, default=30, help='seconds to wait before watching for the quiet desktop')
    ap.add_argument('--boot-quiet', type=int, default=10, help='seconds without a display flush that count as booted')
    ap.add_argument('--boot-max', type=int, default=240, help='longest wait for that quiet, seconds')
    ap.add_argument('--post-boot-wait', type=int, default=30, help='seconds to let the desktop settle after boot')
    ap.add_argument('--bench-timeout', type=int, default=900, help='longest guest bench, seconds (default 900)')
    ap.add_argument('--start-timeout', type=int, default=180,
                    help='give up when the bench has not reported this many seconds after its command was typed')
    args = ap.parse_args()

    if args.summarize:
        results = load_results(args.summarize)
        if not results:
            sys.exit('bench_mac: no run.json under %s' % args.summarize)
        print(summarize(results, args.summarize), end='')
        return

    configs = build_configs(args)
    if args.aa:
        configs = configs[:1]
        args.rounds = args.aa
        if args.label == 'bench':
            args.label = 'aa'
    plan = [(rnd, cfg) for rnd in range(1, args.rounds + 1) for cfg in configs]
    per_run = (args.boot_min + 40 + args.post_boot_wait + 30 + (60 if args.quick else 420)) / 60.0
    log('%d runs (%d configs x %d rounds), about %.0f min' % (len(plan), len(configs), args.rounds, len(plan) * per_run))
    for rnd, cfg in plan:
        print('  round %d  %s' % (rnd, config_id(cfg)))
    if args.dry_run:
        return
    for what, path in [('runner ' + n, p) for n, p in args.runner] + [('image', args.image), ('vars', args.vars), ('code', args.code),
                       ('tools disk', args.tools_img), ('bench script', BENCH_PS1)]:
        if not os.path.exists(path):
            sys.exit('bench_mac: %s not found: %s' % (what, path))

    outdir = args.out or os.path.join(ROOT, 'build', 'bench', '%s-%s' % (args.label, time.strftime('%Y%m%d-%H%M%S')))
    os.makedirs(outdir)
    with open(os.path.join(outdir, 'plan.json'), 'w') as f:
        json.dump({'argv': sys.argv, 'configs': configs, 'rounds': args.rounds, 'quick': args.quick}, f, indent=1)

    atexit.register(cleanup)
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    log('results: %s' % outdir)
    tools_img = build_tools_disk(args.tools_img)

    results = []
    for i, (rnd, cfg) in enumerate(plan, 1):
        results.append(run_one(cfg, i, len(plan), rnd, args, outdir, tools_img))
        summarize(results, outdir)
    print(summarize(results, outdir), end='', flush=True)
    log('summary: %s' % os.path.join(outdir, 'summary.txt'))


if __name__ == '__main__':
    main()
