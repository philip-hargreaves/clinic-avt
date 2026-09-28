"""PriMock57 TextGrids -> ground-truth references, structural cleaning only (common.primock.clean).

references/<cid>.json per consultation: per-speaker segments, doctor_ref, patient_ref and
merged_ref (both speakers by start time). Canonicalisation happens later, in normalise.py,
identically for reference and hypothesis.

    python evaluation/transcription/references.py
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config, primock  # noqa: E402


def build(consult):
    out = {"stem": consult}
    turns = primock.turns(consult)
    for speaker in primock.SPEAKERS:
        segs = [{"start": round(a, 3), "end": round(b, 3), "text": t} for a, b, s, t in turns if s == speaker]
        out[speaker] = segs
        out[f"{speaker}_ref"] = " ".join(s["text"] for s in segs)
    merged = sorted(out["doctor"] + out["patient"], key=lambda s: s["start"])
    out["merged_ref"] = " ".join(s["text"] for s in merged)
    return out


def folder():
    return config.out("transcription", "references")


def load(consult):
    path = folder() / f"{consult}.json"
    if not path.exists():
        path.write_text(json.dumps(build(consult), indent=2, ensure_ascii=False), encoding="utf-8")
    return json.loads(path.read_text(encoding="utf-8"))


def main():
    for consult in primock.consults():
        (folder() / f"{consult}.json").write_text(json.dumps(build(consult), indent=2, ensure_ascii=False),
                                                  encoding="utf-8")
    leaks = sum(1 for c in primock.consults() for k in ("doctor_ref", "patient_ref", "merged_ref")
                if "<" in load(c)[k])
    print(f"{len(primock.consults())} references -> {folder()}; residual markup in {leaks} (must be 0)")
    sys.exit(1 if leaks else 0)


if __name__ == "__main__":
    main()
