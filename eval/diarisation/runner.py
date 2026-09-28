"""The engine's diarisation run offline over the mixed PriMock tracks (diar_eval_runner), and the
time-weighted attribution rule both runner scripts share.

Gold is the PriMock reference segments longer than 0.2 s, labelled doctor (1) or patient (0).
"""
import subprocess
from collections import defaultdict

from asr import references
from common import config, primock


def gold(consult):
    ref = references.load(consult)
    return [{**s, "label": 1 if spk == "doctor" else 0}
            for spk in primock.SPEAKERS for s in ref[spk] if s["end"] - s["start"] > 0.2]


def overlap(a0, a1, b0, b1):
    return max(0.0, min(a1, b1) - max(a0, b0))


def run(consult, *flags):
    wav = config.path("mixed_audio") / f"{consult}_mixed.wav"
    return subprocess.run([str(config.path("diar_runner")), str(config.path("app_models")), str(wav), *flags],
                          capture_output=True, text=True, check=True).stdout


def consults():
    return [c for c in primock.consults() if (config.path("mixed_audio") / f"{c}_mixed.wav").exists()]


def cluster_votes(slices, g):
    # cluster -> [patient seconds, doctor seconds]
    votes = defaultdict(lambda: [0.0, 0.0])
    for s, e, c in slices:
        for gs in g:
            votes[c][gs["label"]] += overlap(gs["start"], gs["end"], s, e)
    return votes
