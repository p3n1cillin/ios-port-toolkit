"""magfit.py RECORDING [--matrix a,b,c,d,e,f,g,h,i]: the magnetometer's hard-iron offset from a figure-8 recording.

RECORDING is `magrec` output (ports/r5q/wifi/magrec.c): lines "n mx my mz ax ay az", the magnetometer in uT x1000
and the accelerometer in m/s^2 x1000, as the kernel has them from the sensors DSP. Turn the phone through every
orientation while it records (a minute of slow figure-8s, face up, face down, every edge).

Prints:
  - the sphere fit: centre (the offset to subtract), radius (should be the Earth's field, ~25-65 uT -- if not, the
    units are wrong) and residual;
  - the field's tilt against gravity while the phone is still (constant if the axes are right; ~-60..-70 deg in
    Europe, steeper indoors near steel) and the best of all 48 axis permutations/sign flips (identity = no swap);
  - with --matrix, the same after a 3x3 correction (e.g. the factory corr_mat from the persist registry), to see
    whether it helps;
  - an ellipsoid fit (soft iron) for comparison.
Needs numpy.
"""
import itertools
import sys

import numpy as np


def load(path):
    rows = []
    for line in open(path):
        p = line.split()
        if len(p) == 7 and p[0].isdigit():
            rows.append([float(v) for v in p[1:]])
    a = np.array(rows)
    return a[:, :3] / 1000.0, a[:, 3:] / 1000.0


def sphere(m):
    A = np.c_[2 * m, np.ones(len(m))]
    x, *_ = np.linalg.lstsq(A, (m ** 2).sum(1), rcond=None)
    c = x[:3]
    return c, np.sqrt(x[3] + (c ** 2).sum())


def report(name, m, gn, still):
    c, r = sphere(m)
    v = m - c
    n = np.linalg.norm(v, axis=1)
    f = v / n[:, None]
    tilt = np.degrees(np.arcsin((f * gn).sum(1)))[still]
    print('%-10s offset %s uT, radius %.1f, residual %.2f | tilt %.1f +- %.1f deg' %
          (name, c.round(1), r, (n - r).std(), tilt.mean(), tilt.std()))
    return f


def main():
    m, g = load(sys.argv[1])
    gnorm = np.linalg.norm(g, axis=1)
    gn = g / gnorm[:, None]
    still = np.abs(gnorm - 9.81) < 0.4
    spread = gn.min(0).round(2), gn.max(0).round(2)
    print('%d samples (%d still); gravity direction from %s to %s (want -1..1 on every axis)' %
          (len(m), still.sum(), spread[0], spread[1]))
    f = report('raw', m, gn, still)
    best = []
    for perm in itertools.permutations(range(3)):
        for s in itertools.product([1, -1], repeat=3):
            t = np.degrees(np.arcsin(((f[:, perm] * np.array(s)) * gn).sum(1)))[still]
            best.append((t.std(), t.mean(), perm, s))
    best.sort()
    print('axes: best %s signs %s (tilt %.1f +- %.1f); next %s signs %s (+- %.1f)' %
          (best[0][2], best[0][3], best[0][1], best[0][0], best[2][2], best[2][3], best[2][0]))
    if '--matrix' in sys.argv:
        M = np.array([float(v) for v in sys.argv[sys.argv.index('--matrix') + 1].split(',')]).reshape(3, 3)
        report('M*raw', m @ M.T, gn, still)
        report('inv(M)*raw', m @ np.linalg.inv(M).T, gn, still)
    D = np.c_[m ** 2, 2 * m[:, 0] * m[:, 1], 2 * m[:, 0] * m[:, 2], 2 * m[:, 1] * m[:, 2], 2 * m, np.ones(len(m))]
    p = np.linalg.svd(D, full_matrices=False)[2][-1]
    Q = np.array([[p[0], p[3], p[4]], [p[3], p[1], p[5]], [p[4], p[5], p[2]]])
    c = -np.linalg.solve(Q, p[6:9])
    w = np.linalg.eigvalsh(Q / (c @ Q @ c - p[9]))
    print('ellipsoid  centre %s, axes %s uT (equal axes = no soft iron)' % (c.round(1), (1 / np.sqrt(np.abs(w))).round(1)))


if __name__ == '__main__':
    main()
