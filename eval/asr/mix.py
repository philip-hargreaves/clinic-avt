"""Mix each PriMock57 consultation's two speaker channels into one single-microphone track.

Summed as recorded (no per-channel rebalancing), DC removed, one uniform gain so the peak sits
at 0.95 of full scale: deterministic and clip-free. Writes <cid>_mixed.wav (16 kHz mono 16-bit)
and mixed_manifest.csv to EVAL_MIXED_AUDIO (default: the research bench's mixed/ folder).

    python eval/asr/mix.py
"""
import csv
import os
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config, primock  # noqa: E402

CEILING = 0.95
RATE = 16000


def read_wav(path):
    with wave.open(str(path), "rb") as w:
        assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (RATE, 1, 2), path.name
        return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0


def write_wav(path, x):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(np.clip(np.round(x * 32767.0), -32768, 32767).astype(np.int16).tobytes())


def mix_one(consult, audio, out):
    d = read_wav(audio / f"{consult}_doctor.wav")
    p = read_wav(audio / f"{consult}_patient.wav")
    n = min(len(d), len(p))
    mix = (d[:n] - d[:n].mean()) + (p[:n] - p[:n].mean())
    raw_peak = float(np.max(np.abs(mix))) if n else 0.0
    gain = CEILING / raw_peak if raw_peak > 0 else 1.0
    mix = mix * gain
    write_wav(out / f"{consult}_mixed.wav", mix)
    return {"stem": consult, "dur_s": round(n / RATE, 1), "raw_peak": round(raw_peak, 3), "gain": round(gain, 3),
            "final_peak": round(float(np.max(np.abs(mix))) if n else 0.0, 3),
            "rms": round(float(np.sqrt(np.mean(mix ** 2))) if n else 0.0, 4),
            "clipped": int(np.sum(np.abs(mix) >= 1.0))}


def main():
    audio = config.path("primock57") / "audio"
    out = config.path("mixed_audio")
    out.mkdir(parents=True, exist_ok=True)
    rows = [mix_one(c, audio, out) for c in primock.consults()]
    with open(out / "mixed_manifest.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    clipped = sum(r["clipped"] for r in rows)
    print(f"{len(rows)} mixed -> {out}; clipped samples {clipped} (must be 0)")
    sys.exit(1 if clipped else 0)


if __name__ == "__main__":
    main()
