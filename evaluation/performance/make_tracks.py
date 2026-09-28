"""Cut the 1x replay tracks from the demo consultations: 2/5/10-minute slices, 15/20-minute joins
and whole consultations, into build/perf-loop/audio.

    python evaluation/performance/make_tracks.py
"""
import os
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402

SRC = os.path.join(config.REPO, "demo")
OUT = os.path.join(config.path("perf_loop"), "audio")
SEC = 32000  # 16 kHz mono int16


def read(name):
    w = wave.open(os.path.join(SRC, name))
    assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (16000, 1, 2), name
    data = w.readframes(w.getnframes())
    w.close()
    return data


def write(name, data):
    w = wave.open(os.path.join(OUT, name), "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(16000)
    w.writeframes(data)
    w.close()
    print(f"{name}: {len(data) / SEC / 60:.2f} min")


os.makedirs(OUT, exist_ok=True)
elbow = read("day2_consultation02_mixed.wav")     # 9.03 min
chest = read("day2_consultation07_mixed.wav")     # 7.62 min
weight = read("day3_consultation03_mixed.wav")    # 11.6 min
allergy = read("day1_consultation09_mixed.wav")   # 8.95 min
write("c02m_elbow.wav", elbow[:2 * 60 * SEC])
write("c05m_elbow.wav", elbow[:5 * 60 * SEC])
write("c10m_weight.wav", weight[:10 * 60 * SEC])
write("c15m_elbow_chest.wav", elbow + chest[:6 * 60 * SEC])
write("c20m_weight_allergy.wav", weight + allergy[:int(8.4 * 60 * SEC)])
# Whole consultations, for reading transcripts and notes before and after a change
write("cfull_elbow.wav", elbow)
write("cfull_day1_consultation01.wav", read("day1_consultation01_mixed.wav"))
write("cfull_day1_consultation09.wav", allergy)
write("cfull_day2_consultation07.wav", chest)
write("cfull_day3_consultation03.wav", weight)
