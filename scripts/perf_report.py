#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turns perf.csv rows (host/perf.h) into rates.

    scripts/perf_report.py perf.csv.1 perf.csv            whole run, oldest file first
    scripts/perf_report.py perf.csv --skip 300            ignore the first 5 minutes after each start
    scripts/perf_report.py perf.csv --from-mark boot --to-mark done
    scripts/perf_report.py perf.csv --minutes             one line per minute as well

Counters are cumulative; a value that goes backwards (a new start, a restart) begins a new segment.
Names ending in _now are levels and _max_us are maxima since the previous row.
"""
import argparse
import statistics
import sys

GRID_W, GRID_H = 8, 6


def read_rows(paths):
    """Yields ('comment', text), ('mark', t_ms, label) and ('row', dict) in file order."""
    for path in paths:
        cols = None
        with open(path, encoding='ascii', errors='replace') as f:
            for line in f:
                line = line.rstrip('\n')
                if not line:
                    continue
                if line.startswith('#mark '):
                    parts = line.split(' ', 2)
                    yield ('mark', int(parts[1]), parts[2] if len(parts) > 2 else '')
                elif line.startswith('#'):
                    yield ('comment', line[1:].strip())
                elif line.startswith('t_ms,'):
                    cols = line.split(',')
                elif cols:
                    vals = line.split(',')
                    if len(vals) != len(cols):
                        continue
                    try:
                        yield ('row', dict(zip(cols, (int(v) for v in vals))))
                    except ValueError:
                        continue


def is_cumulative(name):
    return not (name.endswith('_now') or name.endswith('_max_us'))


def intervals(rows, cpus):
    """Consecutive row pairs as derived per-interval metrics."""
    out = []
    prev = None
    for r in rows:
        if prev is not None:
            dt_ms = r['t_ms'] - prev['t_ms']
            back = any(r[k] < prev[k] for k in ('uptime_ms', 'translations', 'wfi_ns') if k in r and k in prev)
            if dt_ms > 0 and not back:
                out.append(derive(prev, r, dt_ms, cpus))
        prev = r
    return out


def d(a, b, k):
    return max(0, b.get(k, 0) - a.get(k, 0))


def derive(a, b, dt_ms, cpus):
    dt = dt_ms / 1000.0
    ns = dt_ms * 1e6
    m = {'t_ms': b['t_ms'], 'uptime_s': b['uptime_ms'] / 1000.0, 'dt': dt}
    m['idle_pct'] = 100.0 * d(a, b, 'wfi_ns') / (ns * cpus) if cpus else 0.0
    for i in range(min(cpus, 4)):
        m[f'cpu{i}_idle'] = 100.0 * d(a, b, f'wfi{i}_ns') / ns
        run = d(a, b, f'run{i}_ns')
        m[f'cpu{i}_oncore'] = 100.0 * run / ns if run else float('nan')
    m['translations_s'] = d(a, b, 'translations') / dt
    m['flushes'] = d(a, b, 'block_flushes')
    m['flush_full_req'] = d(a, b, 'flush_full')
    m['smc_s'] = d(a, b, 'flush_smc') / dt
    m['inval_dev_s'] = d(a, b, 'inval_dev') / dt
    m['inval_ms_s'] = d(a, b, 'inval_ns') / 1e6 / dt
    m['inval_scan_k_s'] = d(a, b, 'inval_scanned') / 1e3 / dt
    m['code_writes_s'] = d(a, b, 'code_writes') / dt
    m['code_hits_s'] = d(a, b, 'code_write_hits') / dt
    m['code_forced_s'] = d(a, b, 'code_write_forced') / dt
    m['code_retrans_s'] = d(a, b, 'code_retranslates') / dt
    m['code_kills_s'] = d(a, b, 'code_block_kills') / dt
    chained = d(a, b, 'chained')
    m['chain_far_pct'] = 100.0 * d(a, b, 'chain_far') / chained if chained else 0.0
    m['stw'] = d(a, b, 'stw_count')
    m['stw_ms'] = d(a, b, 'stw_ns') / 1e6
    m['stw_max_ms'] = b.get('stw_max_us', 0) / 1000.0
    m['jit_used_mb'] = b.get('jit_used_kb_now', 0) / 1024.0
    m['xchained'] = b.get('xchained_now', 0)
    m['exc_s'] = d(a, b, 'exceptions') / dt
    m['irq_s'] = d(a, b, 'interrupts') / dt
    m['tlb_flush_s'] = d(a, b, 'tlb_flushes') / dt
    m['ijc_reset_s'] = d(a, b, 'ijc_resets') / dt
    m['ijc_light_s'] = d(a, b, 'ijc_light') / dt
    m['ijc_fill_s'] = d(a, b, 'ijc_fills') / dt
    m['disp_miss_s'] = d(a, b, 'disp_miss') / dt
    m['disp_s'] = sum(d(a, b, k) for k in ('disp_chainfill', 'disp_kick', 'disp_exc', 'disp_irq', 'disp_cached', 'disp_miss')) / dt
    m['translate_pct'] = 100.0 * d(a, b, 'translate_ns') / (ns * cpus) if cpus else 0.0
    m['dev_wait_pct'] = 100.0 * d(a, b, 'dev_wait_ns') / (ns * cpus) if cpus else 0.0
    m['tlbi_wait_pct'] = 100.0 * d(a, b, 'tlbi_wait_ns') / (ns * cpus) if cpus else 0.0
    m['gpu_flush_s'] = d(a, b, 'gpu_flushes') / dt
    m['gpu_mpx_s'] = d(a, b, 'gpu_flush_px') / 1e6 / dt
    m['gpu_xfer_mb_s'] = d(a, b, 'gpu_xfer_bytes') / 1e6 / dt
    m['gpu_xfer_pct'] = 100.0 * d(a, b, 'gpu_xfer_ns') / ns
    m['frames_s'] = d(a, b, 'frames_presented') / dt
    m['fb'] = f"{b.get('fb_w_now', 0)}x{b.get('fb_h_now', 0)}"
    rd, wr = d(a, b, 'nvme_rd_n'), d(a, b, 'nvme_wr_n')
    m['nvme_rd_s'] = rd / dt
    m['nvme_rd_mb_s'] = d(a, b, 'nvme_rd_bytes') / 1e6 / dt
    m['nvme_rd_avg_us'] = d(a, b, 'nvme_rd_ns') / 1e3 / rd if rd else 0.0
    m['nvme_wr_s'] = wr / dt
    m['nvme_wr_avg_us'] = d(a, b, 'nvme_wr_ns') / 1e3 / wr if wr else 0.0
    m['nvme_max_ms'] = max(b.get('nvme_rd_max_us', 0), b.get('nvme_wr_max_us', 0)) / 1000.0
    m['nvme_slow'] = d(a, b, 'nvme_lat_10ms') + d(a, b, 'nvme_lat_slow')
    n_in = d(a, b, 'in_n')
    m['in_s'] = n_in / dt
    m['in_avg_ms'] = d(a, b, 'in_q_ns') / 1e6 / n_in if n_in else 0.0
    m['in_max_ms'] = b.get('in_q_max_us', 0) / 1000.0
    m['tick0_gap_max_ms'] = b.get('tick0_gap_max_us', 0) / 1000.0
    m['avail_mb'] = b.get('avail_mem_mb_now', 0)
    m['thermal'] = b.get('thermal_now', 0)
    m['low_power'] = b.get('low_power_now', 0)
    m['grid'] = [d(a, b, f'g{i}') for i in range(GRID_W * GRID_H)]
    return m


def pct(values, q):
    vals = sorted(v for v in values if v == v)
    if not vals:
        return float('nan')
    k = min(len(vals) - 1, max(0, int(round(q / 100.0 * (len(vals) - 1)))))
    return vals[k]


SUMMARY = [
    ('idle %', 'idle_pct', '{:6.1f}'),
    ('translations/s', 'translations_s', '{:8.0f}'),
    ('code invalidations/s', 'smc_s', '{:7.1f}'),
    ('  from devices/s', 'inval_dev_s', '{:7.1f}'),
    ('  ms per second', 'inval_ms_s', '{:7.2f}'),
    ('  chains unlinked K/s', 'inval_scan_k_s', '{:7.1f}'),
    ('writes to code pages/s', 'code_writes_s', '{:8.0f}'),
  # of them, the ones over translated code: they drop the page (code invalidations above) or retire single blocks
    ('  that hit translated code/s', 'code_hits_s', '{:7.1f}'),
    ('  forced by misses/s', 'code_forced_s', '{:7.1f}'),
    ('blocks retired alone/s', 'code_kills_s', '{:7.1f}'),
    ('translations redone (words changed)/s', 'code_retrans_s', '{:7.1f}'),
    ('far chains % of chained', 'chain_far_pct', '{:6.2f}'),
    ('exceptions/s', 'exc_s', '{:8.0f}'),
    ('interrupts/s', 'irq_s', '{:8.0f}'),
    ('TLB flushes/s', 'tlb_flush_s', '{:8.0f}'),
    ('branch-cache resets/s', 'ijc_reset_s', '{:8.0f}'),
    ('branch-cache light resolves/s', 'ijc_light_s', '{:8.0f}'),
    ('branch-cache fills/s (stats on)', 'ijc_fill_s', '{:8.0f}'),
    ('dispatcher entries/s (stats on)', 'disp_s', '{:8.0f}'),
    ('  of them cache misses/s', 'disp_miss_s', '{:8.0f}'),
    ('translating % (stats on)', 'translate_pct', '{:6.2f}'),
    ('device lock wait % (stats on)', 'dev_wait_pct', '{:6.2f}'),
    ('TLBI ack wait % (stats on)', 'tlbi_wait_pct', '{:6.2f}'),
    ('gpu flushes/s', 'gpu_flush_s', '{:7.1f}'),
    ('gpu Mpx flushed/s', 'gpu_mpx_s', '{:7.2f}'),
    ('gpu copy MB/s', 'gpu_xfer_mb_s', '{:7.1f}'),
    ('gpu copy % of one CPU', 'gpu_xfer_pct', '{:6.2f}'),
    ('frames presented/s', 'frames_s', '{:7.1f}'),
    ('disk reads/s', 'nvme_rd_s', '{:7.1f}'),
    ('disk read MB/s', 'nvme_rd_mb_s', '{:7.2f}'),
    ('disk read avg us', 'nvme_rd_avg_us', '{:7.0f}'),
    ('disk write avg us', 'nvme_wr_avg_us', '{:7.0f}'),
    ('disk slowest ms', 'nvme_max_ms', '{:7.1f}'),
    ('input reports/s', 'in_s', '{:7.1f}'),
    ('input queue avg ms', 'in_avg_ms', '{:7.2f}'),
    ('input queue max ms', 'in_max_ms', '{:7.1f}'),
    ('CPU 0 poll gap max ms', 'tick0_gap_max_ms', '{:7.1f}'),
    ('JIT used MB', 'jit_used_mb', '{:7.0f}'),
    ('free memory MB', 'avail_mb', '{:7.0f}'),
    ('thermal state (0-3)', 'thermal', '{:4.0f}'),
]


def summarize(ivs, cpus, out):
    if not ivs:
        print('no intervals in range', file=out)
        return
    span = sum(m['dt'] for m in ivs)
    print(f'{len(ivs)} intervals, {span / 60:.1f} min, display {ivs[-1]["fb"]}, cpus {cpus}', file=out)
    print(f'{"":32} {"mean":>9} {"p50":>9} {"p95":>9} {"max":>9}', file=out)
    for label, key, fmt in SUMMARY:
        vals = [m[key] for m in ivs]
        weights = [m['dt'] for m in ivs]
        good = [(v, w) for v, w in zip(vals, weights) if v == v]
        mean = sum(v * w for v, w in good) / sum(w for _, w in good) if good else float('nan')
        cells = [fmt.format(x) for x in (mean, pct(vals, 50), pct(vals, 95), max(v for v in vals if v == v) if good else float('nan'))]
        print(f'{label:32} ' + ' '.join(f'{c:>9}' for c in cells), file=out)
    for i in range(min(cpus, 4)):
        idle = [m[f'cpu{i}_idle'] for m in ivs]
        oncore = [m[f'cpu{i}_oncore'] for m in ivs]
        on_mean = statistics.fmean([v for v in oncore if v == v]) if any(v == v for v in oncore) else float('nan')
        idle_mean = statistics.fmean(idle)
        resid = 100.0 - idle_mean - on_mean if on_mean == on_mean else float('nan')
        print(f'cpu {i}: idle {idle_mean:5.1f}%  on a core {on_mean:5.1f}%  waiting for a core or blocked {resid:5.1f}%', file=out)
    flushes = sum(m['flushes'] for m in ivs)
    stw_ms = sum(m['stw_ms'] for m in ivs)
    stw_max = max(m['stw_max_ms'] for m in ivs)
    print(f'translation-cache flushes {flushes} ({flushes / (span / 60):.2f}/min), stop-the-world {stw_ms:.1f} ms total, '
          f'longest {stw_max:.1f} ms; flush requests counted the old way {sum(m["flush_full_req"] for m in ivs)}', file=out)
    lows = sum(1 for m in ivs if m['low_power'])
    hot = sum(1 for m in ivs if m['thermal'] >= 2)
    if lows or hot:
        print(f'low power mode in {lows} intervals, thermal serious or worse in {hot}', file=out)
    grid = [sum(m['grid'][i] for m in ivs) for i in range(GRID_W * GRID_H)]
    total = sum(grid)
    if total:
        print('screen redraw by area (% of flushed pixels, 8x6 over the screen):', file=out)
        for y in range(GRID_H):
            print('  ' + ' '.join(f'{100.0 * grid[y * GRID_W + x] / total:5.1f}' for x in range(GRID_W)), file=out)
    idle_anim = [m for m in ivs if m['in_s'] == 0 and m['gpu_flush_s'] > 5]
    if idle_anim:
        print(f'{len(idle_anim)} intervals redraw over 5 times a second with no input (something animates)', file=out)


def per_minute(ivs, cpus, out):
    if not ivs:
        return
    print(f'\n{"min":>5} {"idle%":>6} {"tr/s":>7} {"smc/s":>6} {"flush":>5} {"frm/s":>6} {"Mpx/s":>6} {"rd/s":>6} '
          f'{"in/s":>5} {"inms":>5} {"therm":>5}', file=out)
    start = ivs[0]['t_ms']
    bucket = []
    for m in ivs + [None]:
        if m is not None and (not bucket or (m['t_ms'] - start) // 60000 == (bucket[0]['t_ms'] - start) // 60000):
            bucket.append(m)
            continue
        span = sum(x['dt'] for x in bucket)
        w = lambda k: sum(x[k] * x['dt'] for x in bucket) / span
        print(f'{(bucket[0]["t_ms"] - start) / 60000:5.0f} {w("idle_pct"):6.1f} {w("translations_s"):7.0f} {w("smc_s"):6.1f} '
              f'{sum(x["flushes"] for x in bucket):5d} {w("frames_s"):6.1f} {w("gpu_mpx_s"):6.2f} {w("nvme_rd_s"):6.1f} '
              f'{w("in_s"):5.1f} {w("in_avg_ms"):5.1f} {max(x["thermal"] for x in bucket):5d}', file=out)
        bucket = [m] if m is not None else []


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+')
    ap.add_argument('--skip', type=float, default=0, help='seconds to ignore after each start (boot noise)')
    ap.add_argument('--from-mark')
    ap.add_argument('--to-mark')
    ap.add_argument('--cpus', type=int, default=0, help='vCPU count (default: from the comment line, else 4)')
    ap.add_argument('--minutes', action='store_true')
    args = ap.parse_args()

    rows, marks, comments = [], [], []
    for kind, *rest in read_rows(args.files):
        if kind == 'row':
            rows.append(rest[0])
        elif kind == 'mark':
            marks.append((rest[0], rest[1]))
        else:
            comments.append(rest[0])
    cpus = args.cpus
    for c in comments:
        for tok in c.split():
            if tok.startswith('cpus=') and not cpus:
                cpus = int(tok[5:])
        print('#', c)
    cpus = cpus or 4
    if args.skip:
        rows = [r for r in rows if r['uptime_ms'] >= args.skip * 1000]
    if args.from_mark:
        t = next((t for t, l in marks if l == args.from_mark), None)
        rows = [r for r in rows if t is None or r['t_ms'] >= t]
    if args.to_mark:
        t = next((t for t, l in marks if l == args.to_mark), None)
        rows = [r for r in rows if t is None or r['t_ms'] <= t]
    for t, label in marks:
        print(f'mark {label} at t={t}')
    ivs = intervals(rows, cpus)
    summarize(ivs, cpus, sys.stdout)
    if args.minutes:
        per_minute(ivs, cpus, sys.stdout)


if __name__ == '__main__':
    main()
