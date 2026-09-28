"""Transcribe every PriMock57 input with each Whisper export; save hypotheses, timing and memory.

Hypotheses go to hyp/<ir>/<cid>_<form>.json under build/eval/asr, so scoring is a separate,
repeatable pass. Forms: mixed (the single-microphone track), doctor, patient (the clean
channels). Greedy, English forced for multilingual models. Resumable; a failed input or export
is logged and skipped. Exports ending -npu-ov run on the NPU, the rest on the iGPU.

    python eval/asr/transcribe.py [--models whisper-large-v3-turbo-int8-ov] [--limit 2] [--forms mixed]
"""
import argparse
import json
import os
import sys
import time
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config, primock  # noqa: E402

RATE = 16000


def load_audio(path):
    with wave.open(str(path), "rb") as w:
        assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (RATE, 1, 2), f"{path} bad format"
        return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0


def source(consult, form):
    if form == "mixed":
        return config.path("mixed_audio") / f"{consult}_mixed.wav"
    return config.path("primock57") / "audio" / f"{consult}_{form}.wav"


def model_dir(name):
    # An [asr] model is a folder under asr_models, or an app model folder
    for root in (config.path("asr_models"), config.path("app_models")):
        if (root / name).is_dir():
            return root / name
    raise SystemExit(f"no Whisper export named {name} under asr_models or app_models")


def run_ir(name, consults, forms, device):
    import openvino_genai as ov
    import psutil
    out = config.out("asr", "hyp", name)
    proc = psutil.Process()
    base_mb = proc.memory_info().rss / 1e6
    try:
        t0 = time.perf_counter()
        pipe = ov.WhisperPipeline(str(model_dir(name)), device=device)
        load_s = time.perf_counter() - t0
    except Exception as e:
        summary = {"tag": name, "device": device, "error": f"load failed: {type(e).__name__} {e}"}
        (out / "_summary.json").write_text(json.dumps(summary, indent=2))
        print(f"[{name}] LOAD FAILED on {device}: {e}", flush=True)
        return
    load_mb = proc.memory_info().rss / 1e6 - base_mb
    cfg = pipe.get_generation_config()
    if cfg.is_multilingual:  # .en models reject language and task tokens
        cfg.language = "<|en|>"
        cfg.task = "transcribe"
    cfg.return_timestamps = False
    peak_mb, rates, done, fails = load_mb, [], 0, 0
    for consult in consults:
        for form in forms:
            dst = out / f"{consult}_{form}.json"
            if dst.exists():
                done += 1
                continue
            try:
                audio = load_audio(source(consult, form))
                t = time.perf_counter()
                text = str(pipe.generate(audio, cfg))
                proc_s = time.perf_counter() - t
                rtfx = len(audio) / RATE / proc_s if proc_s else 0.0
                dst.write_text(json.dumps({"text": text, "audio_s": round(len(audio) / RATE, 2),
                                           "proc_s": round(proc_s, 3), "rtfx": round(rtfx, 2)},
                                          ensure_ascii=False), encoding="utf-8")
                rates.append(rtfx)
                done += 1
                peak_mb = max(peak_mb, proc.memory_info().rss / 1e6 - base_mb)
            except Exception as e:
                fails += 1
                print(f"  FAIL {name}/{consult}_{form}: {type(e).__name__} {e}", flush=True)
    median = sorted(rates)[len(rates) // 2] if rates else 0.0
    summary = {"tag": name, "device": device, "load_s": round(load_s, 1), "load_mb": round(load_mb),
               "peak_mb": round(peak_mb), "median_rtfx": round(median, 2), "n_done": done, "n_fail": fails}
    (out / "_summary.json").write_text(json.dumps(summary, indent=2))
    print(f"[{name}] {device} load {load_s:.1f} s, peak {peak_mb:.0f} MB, median RTFx {median:.1f}, "
          f"done {done}, failed {fails}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default="", help="comma list; default every [asr] model in eval/config.toml")
    ap.add_argument("--limit", type=int, default=0, help="first N consultations only")
    ap.add_argument("--forms", default="mixed,doctor,patient")
    ap.add_argument("--device", default="", help="one device for every export")
    args = ap.parse_args()
    models = args.models.split(",") if args.models else config.section("asr")["models"]
    consults = primock.consults()[: args.limit or None]
    for name in models:
        run_ir(name, consults, args.forms.split(","), args.device or ("NPU" if name.endswith("-npu-ov") else "GPU.0"))


if __name__ == "__main__":
    main()
