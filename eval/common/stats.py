"""Paired bootstrap interval of a mean, shared by the stages that report one."""

import numpy as np


def mean_interval(rng, delta, draws=2000) -> tuple[float, float]:
    """95% percentile interval of the mean of `delta` over `draws` resamples, from a numpy Generator."""
    delta = np.asarray(delta)
    picks = rng.integers(0, len(delta), size=(draws, len(delta)))
    low, high = np.percentile(delta[picks].mean(axis=1), [2.5, 97.5])
    return low, high
