"""Diarisation with more than two speakers. Synthetic 3- and 4-speaker consultations go through the
engine's diarisation and role naming (diar_eval_runner --roles), and `space` exports the voice space
of one clip.

A clip is the first 90 s of a real PriMock consultation (doctor and patient) with one or two
companions added: speech from other consultations' speakers, kept only if the engine's embedder
scores them as a different person (cosine to every voice already in the clip below the doctor-patient
similarity plus 0.05). Companions take clean turns in the gaps (overlap 0), half their time over
the base speech (50) or all of it (100), for 10 or 20% of the clip. Seeds are fixed, so the clips
are identical on every build.

    python evaluation/diarisation/multispeaker.py build             clips and manifest.json
    python evaluation/diarisation/multispeaker.py run [--real]      the engine over every clip, and
                                                                    with --real the 57 consultations
    python evaluation/diarisation/multispeaker.py run --enrol       the clips again with the clinician
                                                                    enrolled (runs-enrolled/)
    python evaluation/diarisation/multispeaker.py score [--enrol]   tables per speaker count and overlap
    python evaluation/diarisation/multispeaker.py space <clip>      voice-space and silhouette CSVs

Metrics, duration-weighted over reference turns or 80 ms frames:
  attribution     doctor and patient turns whose majority label is their own role
  speakers found  the engine's cluster count against the true count
  doctor named    the label "doctor" holds most of the true doctor's speech
  to unknown      companion speech labelled unknown (higher is better)
  leak            companion speech labelled doctor or patient (lower is better)

Enrolled runs enrol the clinician as the app would, with 30 s of the base consultation's doctor
speech from after the clip ends, so none of it is scored.

--full uses whole consultations instead of 90 s clips. Each keeps all but its last 90 s, which holds
the enrolment speech, and companions take the same share of the longer clip.
Its clips, runs and scores go to multispeaker-full beside the 90 s set.
"""
import functools
import json
import os
import subprocess
import sys
import tempfile
import wave
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config, io, primock  # noqa: E402
from transcription import references  # noqa: E402

SR = 16000
DUR = 90.0                      # seconds per clip
FULL = "--full" in sys.argv     # whole consultations less a held-out tail
TAIL = 90.0                     # held out at the end of a full clip, for the enrolment
DUCK = 0.05                     # base level under a companion's clean turn
BASES = ["day1_consultation01", "day2_consultation01", "day3_consultation01",
         "day4_consultation01", "day5_consultation01"]
OVERLAPS = [0.0, 0.5, 1.0]
TALK = [0.10, 0.20]
FRAME = 0.08
ENROL_S = 30.0                  # enrolment speech, above the app's 20 s minimum
FIT_MIN_S = 2.0                 # kFitMinFrames, shortest slice used to fit the count
MAX_SPEAKERS = 4                # kMaxSpeakers


def root():
    return config.out("diarisation", "multispeaker-full" if FULL else "multispeaker")


def runner(wav, *flags):
    return subprocess.run([str(config.path("diar_runner")), str(config.path("app_models")), str(wav),
                           *flags], capture_output=True, text=True, check=True).stdout


# ------------------------------------------------------------------ build

def load_wav(path):
    with wave.open(str(path), "rb") as w:
        return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768


def save_wav(path, audio):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.clip(audio, -1, 1) * 32767).astype(np.int16).tobytes())


@functools.lru_cache(maxsize=8)
def mixed(consult):
    return load_wav(config.path("mixed_audio") / f"{consult}_mixed.wav")


def clip_len(base):
    """Seconds of the base consultation a clip keeps"""
    return len(mixed(base)) / SR - TAIL if FULL else DUR


def speech(consult, role, cap=None):
    audio = mixed(consult)
    clips = [audio[int(s["start"] * SR):int(s["end"] * SR)]
             for s in references.load(consult)[role] if s["end"] - s["start"] > 0.4]
    out = np.concatenate(clips) if clips else np.zeros(0, np.float32)
    return out[:int(cap * SR)] if cap else out


class Voices:
    """Engine voiceprints of 8 s of each speaker's speech, cached across builds."""

    def __init__(self):
        self.path = root() / "voiceprints.json"
        self.cache = io.read_json(self.path) if self.path.exists() else {}

    def __call__(self, consult, role):
        key = f"{consult}:{role}"
        if key not in self.cache:
            clip = speech(consult, role, 8)
            if len(clip) < int(0.3 * SR):
                self.cache[key] = None
            else:
                with tempfile.TemporaryDirectory() as tmp:
                    wav = Path(tmp) / "voice.wav"
                    save_wav(wav, clip)
                    line = runner(wav, "--embed").split()
                self.cache[key] = [float(x) for x in line[1:]]
            io.write_json(self.path, self.cache, indent=None)
        v = self.cache[key]
        return None if v is None else np.asarray(v, np.float32)


def base_turns(consult):
    ref = references.load(consult)
    end = clip_len(consult)
    return [[(s["start"], min(s["end"], end)) for s in ref[role]
             if s["end"] - s["start"] > 0.2 and s["start"] < end] for role in ("doctor", "patient")]


def pick_companion(base, avoid, used, rng, voices):
    threshold = max(float(np.dot(avoid[0], avoid[1])), 0.35) + 0.05
    candidates = [(c, r) for c in BASES + primock.consults() for r in ("doctor", "patient") if c != base]
    for i in rng.permutation(len(candidates)):
        consult, role = candidates[i]
        if (consult, role) in used:
            continue
        pool = speech(consult, role)
        if len(pool) < 6 * SR:
            continue
        v = voices(consult, role)
        if v is not None and all(np.dot(v, a) < threshold for a in avoid):
            used.add((consult, role))
            return pool, v, (consult, role)
    raise RuntimeError(f"no distinct companion for {base}")


def place(pool, budget_s, regions, rng, occupied, pos=0):
    """Companion utterances of 1.2-3.5 s inside regions, never on another companion"""
    segs, inserts, placed = [], [], 0.0
    regions = [list(r) for r in regions]
    rng.shuffle(regions)
    for r0, r1 in regions:
        t = r0
        while t < r1 - 0.6 and placed < budget_s and pos < len(pool):
            dur = min(float(rng.uniform(1.2, 3.5)), r1 - t)
            if dur < 0.6:
                break
            if any(t < o1 and t + dur > o0 for o0, o1 in occupied):
                t += 0.3
                continue
            clip = pool[pos:pos + int(dur * SR)]
            pos += int(dur * SR)
            if len(clip) < int(0.6 * SR):
                break
            end = t + len(clip) / SR
            inserts.append((int(t * SR), clip))
            segs.append((round(t, 3), round(end, 3)))
            occupied.append((t, end))
            placed += len(clip) / SR
            t = end + float(rng.uniform(0.4, 1.5))
        if placed >= budget_s or pos >= len(pool):
            break
    return segs, inserts, pos


def subtract(segs, holes):
    out = []
    for s, e in segs:
        parts = [(s, e)]
        for h0, h1 in holes:
            parts = [p for a, b in parts for p in
                     ([(a, b)] if h1 <= a or h0 >= b else [(a, min(b, h0)), (max(a, h1), b)])]
        out += [(a, b) for a, b in parts if b - a > 0.2]
    return out


def build_clip(base, extra, overlap_frac, talk, seed, voices):
    rng = np.random.RandomState(seed)
    end = clip_len(base)
    audio = mixed(base)[:int(end * SR)].copy()
    audio = np.pad(audio, (0, max(0, int(end * SR) - len(audio))))
    doctor, patient = base_turns(base)
    avoid = [voices(base, "doctor"), voices(base, "patient")]
    ref, sources, used, occupied, clean = {}, {}, set(), [], []
    for c in range(extra):
        pool, v, source = pick_companion(base, avoid, used, rng, voices)
        avoid.append(v)
        budget, segs, pos = talk * end, [], 0
        if (1 - overlap_frac) * budget > 0.1:   # clean turns, base speech ducked
            s, inserts, pos = place(pool, (1 - overlap_frac) * budget, [(0.0, end)], rng, occupied, pos)
            for at, clip in inserts:
                n = min(len(clip), len(audio) - at)
                audio[at:at + n] = audio[at:at + n] * DUCK + 0.95 * clip[:n]
            clean += s
            segs += s
        if overlap_frac * budget > 0.1:         # overlapping turns, mixed over the base speech
            s, inserts, pos = place(pool, overlap_frac * budget, sorted(doctor + patient), rng,
                                    occupied, pos)
            for at, clip in inserts:
                n = min(len(clip), len(audio) - at)
                audio[at:at + n] += 0.9 * clip[:n]
            segs += s
        name = "companion" if c == 0 else "companion2"
        ref[name] = [{"start": a, "end": b} for a, b in sorted(segs)]
        sources[name] = ":".join(source)
    ref = {"doctor": [{"start": a, "end": b} for a, b in subtract(doctor, clean)],
           "patient": [{"start": a, "end": b} for a, b in subtract(patient, clean)], **ref}
    peak = float(np.abs(audio).max())
    return (audio / peak if peak > 1 else audio), ref, sources


def build():
    voices = Voices()
    clips = root() / "clips"
    clips.mkdir(exist_ok=True)
    grid = [(b, extra, ov, talk, si) for si, b in enumerate(BASES) for extra in (1, 2)
            for ov in OVERLAPS for talk in TALK]
    manifest = []
    for i, (base, extra, ov, talk, si) in enumerate(grid):
        audio, ref, sources = build_clip(base, extra, ov, talk, 1000 + i, voices)
        name = f"{base}_k{2 + extra}_ov{int(ov * 100):03d}_tf{int(talk * 100):02d}"
        save_wav(clips / f"{name}.wav", audio)
        manifest.append({"name": name, "base": base, "speakers": 2 + extra, "overlap": ov,
                         "talk": talk, "sources": sources, "ref": ref})
        io.log(f"{name}  companions {', '.join(sources.values())}")
    io.write_json(root() / "manifest.json", manifest)
    io.log(f"{len(manifest)} clips -> {clips}")


# ------------------------------------------------------------------ run

def enrolment(base):
    """Enrolment wav from the doctor's speech after the clip ends"""
    path = root() / "enrol" / f"{base}.wav"
    if not path.exists():
        audio = mixed(base)
        clips = [audio[int(s["start"] * SR):int(s["end"] * SR)]
                 for s in references.load(base)["doctor"]
                 if s["start"] >= clip_len(base) and s["end"] - s["start"] > 0.4]
        speech = np.concatenate(clips)[:int(ENROL_S * SR)]
        if len(speech) < int(20 * SR):
            raise RuntimeError(f"{base}: under 20 s of doctor speech after the clip")
        path.parent.mkdir(exist_ok=True)
        save_wav(path, speech)
    return path


def run(real, enrol=False):
    out = root() / ("runs-enrolled" if enrol else "runs")
    out.mkdir(exist_ok=True)
    clips = io.read_json(root() / "manifest.json")
    jobs = [(c["name"], root() / "clips" / f"{c['name']}.wav", c["base"]) for c in clips]
    jobs += [(f"{b}_k2", None, b) for b in BASES]
    if real and not enrol:
        jobs += [(c, config.path("mixed_audio") / f"{c}_mixed.wav", c) for c in primock.consults()]
    for name, wav, base in jobs:
        dest = out / f"{name}.txt"
        if dest.exists():
            continue
        flags = ["--enrol", str(enrolment(base))] if enrol else ["--roles"]
        if wav is None:                         # base clip with no companions
            with tempfile.TemporaryDirectory() as tmp:
                wav = Path(tmp) / "base.wav"
                save_wav(wav, mixed(base)[:int(clip_len(base) * SR)])
                dest.write_text(runner(wav, *flags), encoding="utf-8")
        else:
            dest.write_text(runner(wav, *flags), encoding="utf-8")
        io.log(name)


# ------------------------------------------------------------------ score

def parse(text):
    roles, slices = {}, []
    for line in text.splitlines():
        part = line.split()
        if part[:1] == ["ROLE"]:
            roles[int(part[1])] = " ".join(part[2:])
        elif part[:1] == ["SLICE"]:
            slices.append((float(part[1]), float(part[2]), int(part[3])))
    return [(s, e, roles.get(c, "unknown")) for s, e, c in slices], len(roles)


def overlap(a0, a1, b0, b1):
    return max(0.0, min(a1, b1) - max(a0, b0))


def attribution(tracks, ref):
    num = den = 0.0
    for role in ("doctor", "patient"):
        for x in ref.get(role, []):
            d = x["end"] - x["start"]
            if d <= 0.2:
                continue
            time = defaultdict(float)
            for s, e, label in tracks:
                time[label] += overlap(s, e, x["start"], x["end"])
            num += d * (max(time, key=time.get) == role if any(time.values()) else 0)
            den += d
    return num, den


def doctor_named(tracks, ref):
    time = defaultdict(float)
    for x in ref["doctor"]:
        for s, e, label in tracks:
            time[label] += overlap(s, e, x["start"], x["end"])
    return bool(time) and max(time, key=time.get) == "doctor"


def frames(spans, n):
    m = np.zeros(n, bool)
    for s, e in spans:
        m[int(s / FRAME):int(np.ceil(e / FRAME))] = True
    return m


def companions(tracks, ref):
    ends = [e for _, e, _ in tracks] + [x["end"] for r in ref.values() for x in r]
    n = int(np.ceil(max(ends) / FRAME)) + 2
    unknown = frames([(s, e) for s, e, label in tracks if label == "unknown"], n)
    main = frames([(s, e) for s, e, label in tracks if label in ("doctor", "patient")], n)
    extra = frames([(x["start"], x["end"]) for r in ("companion", "companion2") for x in ref.get(r, [])], n)
    total = extra.sum() or 1
    return 100 * (extra & unknown).sum() / total, 100 * (extra & main & ~unknown).sum() / total


def score(enrol=False):
    runs = root() / ("runs-enrolled" if enrol else "runs")
    rows = []
    for clip in io.read_json(root() / "manifest.json"):
        rows.append({**clip, "text": (runs / f"{clip['name']}.txt").read_text(encoding="utf-8")})
    for base in BASES:
        ref = references.load(base)
        end = clip_len(base)
        cut = {r: [{"start": s["start"], "end": min(s["end"], end)} for s in ref[r] if s["start"] < end]
               for r in ("doctor", "patient")}
        rows.append({"name": f"{base}_k2", "speakers": 2, "overlap": 0.0, "ref": cut,
                     "text": (runs / f"{base}_k2.txt").read_text(encoding="utf-8")})
    real = [] if enrol else [c for c in primock.consults() if (runs / f"{c}.txt").exists()]
    for c in real:
        ref = references.load(c)
        rows.append({"name": c, "speakers": 2, "overlap": "real",
                     "ref": {r: ref[r] for r in ("doctor", "patient")},
                     "text": (runs / f"{c}.txt").read_text(encoding="utf-8")})

    groups = defaultdict(list)
    for row in rows:
        tracks, count = parse(row["text"])
        num, den = attribution(tracks, row["ref"])
        to_unknown, leak = companions(tracks, row["ref"]) if row["speakers"] > 2 else (None, None)
        # Naming either abstained, picked the doctor, or picked the wrong speaker
        abstained = not any(label == "doctor" for _, _, label in tracks)
        right = doctor_named(tracks, row["ref"])
        anchored = "ANCHORED 1" in row["text"]
        groups[(row["speakers"], row["overlap"])].append(
            (num, den, count == row["speakers"], right, to_unknown, leak, abstained,
             not abstained and not right, anchored))

    out = []
    print(f"{'speakers':>8} {'overlap':>7} {'n':>3} {'attribution':>11} {'found':>7} {'doctor':>7} "
          f"{'not named':>9} {'swapped':>7} {'to unknown':>10} {'leak':>6}")
    for (k, ov), g in sorted(groups.items(), key=lambda kv: (kv[0][0], str(kv[0][1]))):
        attr = 100 * sum(x[0] for x in g) / sum(x[1] for x in g)
        found = sum(x[2] for x in g)
        named = sum(x[3] for x in g)
        abstained = sum(x[6] for x in g)
        swapped = sum(x[7] for x in g)
        unk = [x[4] for x in g if x[4] is not None]
        leak = [x[5] for x in g if x[5] is not None]
        row = {"speakers": k, "overlap": ov if ov == "real" else int(100 * ov), "n": len(g),
               "attribution": round(attr, 1), "found": found, "doctor": named,
               "not_named": abstained, "swapped": swapped,
               "to_unknown": round(float(np.mean(unk)), 1) if unk else None,
               "leak": round(float(np.mean(leak)), 1) if leak else None,
               "by_voice": sum(x[8] for x in g)}
        out.append(row)
        print(f"{k:>8} {str(row['overlap']):>7} {len(g):>3} {attr:>10.1f}% {found:>3}/{len(g):<3} "
              f"{named:>3}/{len(g):<3} {abstained:>9} {swapped:>7} "
              f"{'' if not unk else f'{np.mean(unk):.1f}%':>10} "
              f"{'' if not leak else f'{np.mean(leak):.1f}%':>6}")
    io.write_json(root() / ("scores-enrolled.json" if enrol else "scores.json"), out)


# ------------------------------------------------------------------ space

def silhouette(dist, labels):
    """Mean silhouette over precomputed distances. Singletons score 0, as in the engine."""
    total = 0.0
    for i in range(len(labels)):
        same = (labels == labels[i])
        same[i] = False
        if not same.any():
            continue
        a = dist[i, same].mean()
        b = min(dist[i, labels == c].mean() for c in set(labels) if c != labels[i])
        total += (b - a) / max(a, b)
    return total / len(labels)


def space(name):
    from scipy.cluster.hierarchy import fcluster, linkage
    from scipy.spatial.distance import squareform

    clip = next(c for c in io.read_json(root() / "manifest.json") if c["name"] == name)
    lines = runner(root() / "clips" / f"{name}.wav", "--space").splitlines()
    count = int(lines[0].split()[1])
    rows = [line.split() for line in lines[1:]]
    start = np.array([float(r[1]) for r in rows])
    end = np.array([float(r[2]) for r in rows])
    cluster = np.array([int(r[3]) for r in rows])
    emb = np.array([[float(x) for x in r[4:]] for r in rows])
    emb /= np.linalg.norm(emb, axis=1, keepdims=True)

    role_of = {}
    for line in (root() / "runs" / f"{name}.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("ROLE "):
            role_of[int(line.split()[1])] = " ".join(line.split()[2:])

    def truth(s, e):
        time = {r: sum(overlap(s, e, x["start"], x["end"]) for x in clip["ref"].get(r, []))
                for r in ("doctor", "patient", "companion", "companion2")}
        best = max(time, key=time.get)
        return best if time[best] > 0 else "none"

    centred = emb - emb.mean(axis=0)
    _, _, vt = np.linalg.svd(centred, full_matrices=False)
    pc = centred @ vt[:2].T
    centroids = np.array([emb[cluster == c].mean(axis=0) for c in range(count)])
    centroids /= np.linalg.norm(centroids, axis=1, keepdims=True)
    cpc = (centroids - emb.mean(axis=0)) @ vt[:2].T

    # Companion slices are right when labelled unknown. An unnamed cluster ("speaker N") takes its
    # majority speaker, and a slice is wrong when its speaker differs from its cluster's
    label = [role_of.get(int(c), "unknown") for c in cluster]
    true = [{"companion": "unknown", "companion2": "unknown"}.get(t, t)
            for t in (truth(s, e) for s, e in zip(start, end))]
    expected = {}
    for c in range(count):
        named = role_of.get(c, "unknown")
        members = [t for t, k in zip(true, cluster) if k == c]
        expected[c] = (max(set(members), key=members.count) if members else named) \
            if named.startswith("speaker") else named
    wrong = [expected[int(c)] != t for c, t in zip(cluster, true)]
    shown = ["unnamed" if lab.startswith("speaker") else lab for lab in label]
    raw = [truth(s, e) for s, e in zip(start, end)]
    matched = {}
    for c in range(count):
        members = [t for t, k in zip(raw, cluster) if k == c]
        matched[c] = max(set(members), key=members.count) if members else "none"

    out = root() / "figures"
    out.mkdir(exist_ok=True)
    with open(out / f"space-{name}.csv", "w", encoding="utf-8", newline="\n") as f:
        f.write("pc1,pc2,seconds,cluster,role,truth,wrong,matched\n")
        for i in range(len(rows)):
            f.write(f"{pc[i, 0]:.4f},{pc[i, 1]:.4f},{end[i] - start[i]:.2f},{cluster[i]},"
                    f"{shown[i]},{true[i]},{int(wrong[i])},{matched[int(cluster[i])]}\n")
    with open(out / f"centroids-{name}.csv", "w", encoding="utf-8", newline="\n") as f:
        f.write("pc1,pc2,cluster,role,matched\n")
        for c in range(count):
            named = role_of.get(c, "unknown")
            f.write(f"{cpc[c, 0]:.4f},{cpc[c, 1]:.4f},{c},"
                    f"{'unnamed' if named.startswith('speaker') else named},{matched[c]}\n")

    fit = (end - start) >= FIT_MIN_S
    dist = np.clip(1.0 - emb[fit] @ emb[fit].T, 0.0, None)
    np.fill_diagonal(dist, 0.0)
    tree = linkage(squareform(dist, checks=False), method="average")
    scores = {k: silhouette(dist, fcluster(tree, k, criterion="maxclust"))
              for k in range(2, min(MAX_SPEAKERS, int(fit.sum()) - 1) + 1)}
    chosen = max(scores, key=scores.get)
    with open(out / f"silhouette-{name}.csv", "w", encoding="utf-8", newline="\n") as f:
        f.write("k,score,chosen\n")
        for k, s in scores.items():
            f.write(f"{k},{s:.4f},{int(k == chosen)}\n")
    io.log(f"{name}: engine k={count}, sweep k={chosen}{'' if chosen == count else '  MISMATCH'}, "
           f"{len(rows)} slices, {sum(wrong)} labelled differently from the truth -> {out}")


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else ""
    if command == "build":
        build()
    elif command == "run":
        run("--real" in sys.argv, "--enrol" in sys.argv)
    elif command == "score":
        score("--enrol" in sys.argv)
    elif command == "space" and len(sys.argv) > 2:
        space(sys.argv[2])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
