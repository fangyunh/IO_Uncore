#!/usr/bin/env python3
"""Shared statistics for the IAU headroom investigation (spec 2026-10-13 §4 H and §4 K)."""
import random
import statistics


def med(v):
    return statistics.median(v)


def spread(v):
    return (max(v) - min(v)) if v else 0.0


def ns_per_io(iops, app_cores=1):
    return app_cores * 1e9 / iops


def effect(base, test, floor_ns=1.0, k=2.0):
    """base/test: ns/IO per repeat. Real iff |delta| > max(floor_ns, k * larger within-arm spread)."""
    d = med(test) - med(base)
    s = max(spread(base), spread(test))
    return {"delta": d, "spread": s, "real": abs(d) > max(floor_ns, k * s)}


def bootstrap_ci(base, test, n_boot=10000, alpha=0.05, seed=12345):
    """Percentile bootstrap CI of med(test) - med(base), resampling each arm independently (fixed seed).

    With ~5 repeats per arm the median bootstrap is discrete and tends to under-cover (coarse, optimistic); the 2x-spread rule remains the decision criterion.
    """
    if not base or not test:
        raise ValueError("bootstrap_ci needs non-empty base and test")
    rng = random.Random(seed)
    diffs = []
    for _ in range(n_boot):
        b = [base[rng.randrange(len(base))] for _ in base]
        t = [test[rng.randrange(len(test))] for _ in test]
        diffs.append(med(t) - med(b))
    diffs.sort()
    lo = diffs[int((alpha / 2) * n_boot)]
    hi = diffs[min(n_boot - 1, int((1 - alpha / 2) * n_boot))]
    return lo, hi


def headroom(iops_lo, iops_hi):
    """Relative IOPS gain when device capacity is raised (lo config -> hi config)."""
    return med(iops_hi) / med(iops_lo) - 1.0


def slope(ns_lo, ns_hi, injected_cycles, ghz):
    """Measured ns/IO rise between two spin levels over the injected time (1.0 = fully CPU-bound)."""
    return (med(ns_hi) - med(ns_lo)) / (injected_cycles / ghz)


def slope_fit(ks, ns_medians):
    """Least-squares slope of median ns/IO vs injected ns (unitless; 1.0 = fully CPU-bound)."""
    n = len(ks)
    mk, mn = sum(ks) / n, sum(ns_medians) / n
    return sum((k - mk) * (y - mn) for k, y in zip(ks, ns_medians)) / sum((k - mk) ** 2 for k in ks)


HEADROOM_TOL = 0.01
SLOPE_TOL = 0.15


def verdict(headroom_val, slope_val, headroom_tol=HEADROOM_TOL, slope_tol=SLOPE_TOL, headroom_real=True):
    """device-limited only if the headroom clears the tolerance AND is distinguishable from noise."""
    if headroom_val >= headroom_tol and headroom_real:
        return "device-limited"
    if abs(slope_val - 1.0) <= slope_tol:
        return "cpu-bound"
    return "inconclusive"


def per_sq_util(iops, app_cores, qpairs, channels=32, tmax=250000.0):
    return iops / (app_cores * qpairs) / (channels * tmax)
