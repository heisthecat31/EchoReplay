"""Simulate the 7.3.2 accumulator against the invariant AND against bit-exactness.

F-10: the first version's fidelity column compared each engine step to the tick it had just
been assigned from -- steps[i] was literally ticks[i], so the column was X == X and reported
100% for any implementation. It measured nothing. The property is real (an independent
auditor simulator confirmed it) but this file did not measure it.

The column now discriminates because `step_source` selects between the design's rule and the
round-2 bug, and the two must produce different numbers:
    'tick' -> the engine step is the RECORDED dt of the tick being consumed   (the design)
    'acc'  -> the engine step is the wall-derived accumulator amount          (round 2)
A fidelity figure that does not collapse under 'acc' is not measuring fidelity.
"""
import random
import sys


def sim(cap_hz, rend_hz, r, secs=60.0, K=8, microseek=True, step_source='tick', zero_acc=True,
        jitter=0.001, tick_jitter_us=50, stall_at=None, stall_us=0, seed=7):
    rnd = random.Random(seed)
    d_nom = 1e6 / cap_hz
    ticks, tsum = [], 0.0
    while tsum < secs * abs(r) * 1e6 + 5e6:
        d = d_nom + rnd.uniform(-tick_jitter_us, tick_jitter_us)
        ticks.append(d)
        tsum += d
    sgn = 1.0 if r >= 0 else -1.0
    idx, acc, playhead, wall = 0, 0.0, 0.0, 0.0
    steps, microseeks, maxdev = [], 0, 0.0
    frame = 1e6 / rend_hz
    while wall < secs * 1e6:
        wdt = frame * (1.0 + rnd.uniform(-jitter, jitter))
        if stall_at is not None and wall <= stall_at * 1e6 < wall + wdt:
            wdt += stall_us
        wall += wdt
        acc += r * wdt
        consumed = 0
        # signed, direction-aware: F-1 -- `acc -= d` unsigned runs reverse away at 5.6x
        while consumed < K and idx < len(ticks) and abs(acc) >= ticks[idx]:
            d = ticks[idx]
            step = d if step_source == 'tick' else abs(acc)
            steps.append((step, d))
            acc -= sgn * d
            playhead += sgn * d
            idx += 1
            consumed += 1
        if microseek and consumed == K and idx < len(ticks) and abs(acc) >= ticks[idx]:
            # F-8: micro-seek advances the playhead by the outstanding accumulator and
            # ZEROES it. Not zeroing it leaves the surplus to be consumed twice.
            playhead += acc
            while idx < len(ticks) and abs(acc) >= ticks[idx]:
                acc -= sgn * ticks[idx]
                idx += 1
            if zero_acc:
                acc = 0.0   # F-8: not zeroing leaves the surplus to be consumed a second time
            microseeks += 1
        maxdev = max(maxdev, abs(playhead - r * wall))
    ideal = r * wall
    err = (playhead - ideal) / abs(ideal) * 100 if ideal else 0.0
    exact = sum(1 for s, d in steps if s == d)
    return dict(err=err, n=len(steps), exact=exact, acc=acc, micro=microseeks,
                maxdev=maxdev, maxtick=max(ticks) if ticks else 0.0)


CAPS = (44, 72, 90, 120)
RENDS = (60, 72, 90, 144, 165)
RATES = (0.05, 0.25, 1.0, 2.0, 4.0, -0.25, -1.0, -2.0)

if __name__ == '__main__':
    print("=== F-10: the fidelity column must COLLAPSE under the round-2 step rule ===")
    print("%-26s %10s %10s" % ("cell", "step='tick'", "step='acc'"))
    for cap, rend, r in ((72, 90, 1.0), (90, 90, 1.0), (120, 60, 2.0), (44, 165, 0.25)):
        a = sim(cap, rend, r, step_source='tick')
        b = sim(cap, rend, r, step_source='acc')
        print("cap%-4d rend%-4d r=%-5.2f %6d/%-4d %6d/%-4d"
              % (cap, rend, r, a['exact'], a['n'], b['exact'], b['n']))

    print()
    print("=== invariant sweep, step='tick', K=8, micro-seek on ===")
    worst_err = worst_cell = None
    worst_dev_ticks = 0.0
    sat = 0
    for cap in CAPS:
        for rend in RENDS:
            for r in RATES:
                s = sim(cap, rend, r)
                if worst_err is None or abs(s['err']) > abs(worst_err):
                    worst_err, worst_cell = s['err'], (cap, rend, r)
                worst_dev_ticks = max(worst_dev_ticks, s['maxdev'] / s['maxtick'])
                if s['micro']:
                    sat += 1
    print("cells                      : %d" % (len(CAPS) * len(RENDS) * len(RATES)))
    print("worst |rate error|         : %.4f%%  at cap%d rend%d r=%g"
          % (abs(worst_err), worst_cell[0], worst_cell[1], worst_cell[2]))
    print("worst |playhead - r*wall|  : %.2f recorded ticks   (F-9: state the real bound)"
          % worst_dev_ticks)
    print("cells that saturated K     : %d" % sat)

    print()
    print("=== F-3/BL-3: a 10 s stall must not burst or drift ===")
    s = sim(90, 90, 1.0, secs=30.0, stall_at=10.0, stall_us=10e6)
    print("residual acc after a 10 s stall: %.1f us   micro-seeks: %d   rate err %.4f%%"
          % (s['acc'], s['micro'], s['err']))
