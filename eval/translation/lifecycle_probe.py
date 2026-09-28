"""Measures the engine translator's cold start, warm speed and memory under CPU plugin settings.

    python lifecycle_probe.py <label> <cache dir> [options]        one fresh process, one JSON line
    python lifecycle_probe.py matrix <out.jsonl> <work dir> [a,b]  variants, compile then import twice

Options: --cache-mode size|speed, --no-mmap, --threads N, --pcore, --dq 0, --no-partials,
--warm ready|sheet, --unload, --tokenizer <dir>. A --tokenizer runs as the engine runs the
SentencePiece one, with punctuation made plain and the source language and end tokens added.

Mirrors NllbTranslator: tokenizer and detokenizer on their own Core without a cache, encoder
and stateful decoder compiled with CACHE_DIR, greedy decoding one sentence at a time, the
runner-up breaking a triple repeat and, by default, a detokenize per token as the partials do.
"""

import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from study import APP_MODELS, ROOT, SHEETS  # noqa: E402

MODEL = APP_MODELS / "nllb-200-600m-int8"
LANGUAGES = ["Urdu", "Polish", "Punjabi", "Arabic", "Bengali", "Romanian"]
MAX_TOKENS = 512


def sentences(line: str) -> list[str]:
    out, start = [], 0
    while start < len(line):
        end = len(line)
        for mark in (". ", "? ", "! "):
            at = line.find(mark, start)
            if at != -1 and at + 1 < end:
                end = at + 1
        if line[start:end].strip():
            out.append(line[start:end])
        start = end + (1 if end < len(line) else 0)
    return out


def memory() -> dict:
    import psutil
    info = psutil.Process().memory_info()
    return {"wset_mb": round(info.rss / 2**20), "private_mb": round(info.private / 2**20),
            "peak_wset_mb": round(info.peak_wset / 2**20)}


def probe(label: str, cache: Path, args: list[str]) -> dict:
    def option(flag, default):
        return args[args.index(flag) + 1] if flag in args else default

    marks, out = {}, {"label": label, "args": args}
    t = time.perf_counter()

    import numpy as np
    import openvino as ov
    marks["import_s"] = time.perf_counter() - t
    out["mem_import"] = memory()

    config = {"CACHE_DIR": str(cache)}
    if "--cache-mode" in args:
        config["CACHE_MODE"] = "OPTIMIZE_SIZE" if option("--cache-mode", "") == "size" else "OPTIMIZE_SPEED"
    if "--threads" in args:
        config["INFERENCE_NUM_THREADS"] = int(option("--threads", "0"))
    if "--pcore" in args:
        config["SCHEDULING_CORE_TYPE"] = "PCORE_ONLY"
    if "--dq" in args:
        config["DYNAMIC_QUANTIZATION_GROUP_SIZE"] = int(option("--dq", "32"))
    core = ov.Core()
    if "--no-mmap" in args:
        core.set_property({"ENABLE_MMAP": False})

    def compile(name):
        return core.compile_model(str(MODEL / name), "CPU", config).create_infer_request()

    t = time.perf_counter()
    encoder = compile("openvino_encoder_model.xml")
    marks["encoder_s"] = time.perf_counter() - t
    t = time.perf_counter()
    decoder = compile("openvino_decoder_model.xml")
    marks["decoder_s"] = time.perf_counter() - t
    out["mem_models"] = memory()

    t = time.perf_counter()
    import openvino_tokenizers  # noqa: F401  registers the extension, as add_extension does
    tok_core = ov.Core()
    tok_dir = Path(option("--tokenizer", str(MODEL)))
    tokenizer = tok_core.compile_model(str(tok_dir / "openvino_tokenizer.xml"), "CPU")
    tokenizer = tokenizer.create_infer_request()
    detokenizer = tok_core.compile_model(str(MODEL / "openvino_detokenizer.xml"), "CPU")
    detokenizer = detokenizer.create_infer_request()
    marks["tokenizers_s"] = time.perf_counter() - t
    out["mem_loaded"] = memory()

    spec = json.load(open(MODEL / "languages.json", encoding="utf-8"))
    eos, start_id = spec["special"]["eos"], spec["special"]["decoderStart"]
    partials = "--no-partials" not in args
    beam = ov.Tensor(np.array([0], dtype=np.int32))
    split = {"tokenizer": 0.0, "encoder": 0.0, "decoder": 0.0, "detok": 0.0}

    def detok(ids):
        s = time.perf_counter()
        detokenizer.infer([ov.Tensor(np.array([ids], dtype=np.int64))])
        text = detokenizer.get_output_tensor().str_data[0].replace("\u2581", " ").lstrip()
        split["detok"] += time.perf_counter() - s
        return text

    rebuild = "--tokenizer" in args
    from tokenizer_parity import PLAIN

    def sentence(text, target, steps, marks_mem=None):
        s = time.perf_counter()
        if rebuild:
            tokenizer.infer([ov.Tensor(np.array([text.translate(PLAIN)]))])
            body = tokenizer.get_tensor("input_ids").data[0].tolist()
            # The first conversion also emits <s>
            if body and body[0] == 0:
                body = body[1:]
            full = [spec["special"]["sourceLang"]] + body + [eos]
            ids = ov.Tensor(np.array([full], dtype=np.int64))
            mask = ov.Tensor(np.ones((1, len(full)), dtype=np.int64))
        else:
            tokenizer.infer([ov.Tensor(np.array([text]))])
            ids, mask = tokenizer.get_tensor("input_ids"), tokenizer.get_tensor("attention_mask")
        split["tokenizer"] += time.perf_counter() - s
        if marks_mem is not None:
            marks_mem["after_tokenizer"] = memory()
        s = time.perf_counter()
        encoder.set_tensor("input_ids", ids)
        encoder.set_tensor("attention_mask", mask)
        encoder.infer()
        hidden = encoder.get_output_tensor(0)
        split["encoder"] += time.perf_counter() - s
        if marks_mem is not None:
            marks_mem["after_encoder"] = memory()
        s = time.perf_counter()
        decoder.reset_state()
        step, generated = [start_id, target], []
        while len(generated) < MAX_TOKENS:
            decoder.set_tensor("input_ids", ov.Tensor(np.array([step], dtype=np.int64)))
            decoder.set_tensor("encoder_hidden_states", hidden)
            decoder.set_tensor("encoder_attention_mask", mask)
            decoder.set_tensor("beam_idx", beam)
            decoder.infer()
            last = decoder.get_tensor("logits").data[0, -1]
            token = int(last.argmax())
            if len(generated) >= 2 and token == generated[-1] == generated[-2]:
                copy = last.copy()
                copy[token] = -1e9
                token = int(copy.argmax())
            if token == eos:
                break
            generated.append(token)
            step = [token]
            steps.append(1)
            if partials:
                split["decoder"] += time.perf_counter() - s
                detok(generated)
                s = time.perf_counter()
        split["decoder"] += time.perf_counter() - s
        return detok(generated)

    def sheet(text, language):
        target, steps = spec["languages"][language]["id"], []
        lines = [" ".join(sentence(x, target, steps) for x in sentences(line)) for line in text.split("\n")]
        return "\n".join(lines), len(steps)

    # The engine's warm-up, or a whole sheet
    t = time.perf_counter()
    warm_mem = {}
    if option("--warm", "ready") == "ready":
        sentence("Ready.", spec["languages"]["Arabic"]["id"], [], warm_mem)
    else:
        sheet(load_sheets()[-1], "Arabic")
    marks["warm_s"] = time.perf_counter() - t
    first = dict(split)
    out["mem_warm"] = memory()
    out["mem_warm_steps"] = warm_mem

    texts = load_sheets()
    laps, outputs = [], []
    for n, language in enumerate(LANGUAGES):
        for key in split:
            split[key] = 0.0
        t = time.perf_counter()
        translated, tokens = sheet(texts[n], language)
        laps.append({"language": language, "s": round(time.perf_counter() - t, 3), "tokens": tokens,
                     **{k: round(v, 3) for k, v in split.items()}})
        outputs.append(translated)
    out["mem_after"] = memory()
    out["marks"] = {k: round(v, 2) for k, v in marks.items()}
    out["warm_split"] = {k: round(v, 3) for k, v in first.items()}
    out["laps"] = laps
    out["first_sheet_s"] = laps[0]["s"]
    rest = sorted(lap["s"] for lap in laps[1:])
    out["warm_median_s"] = rest[len(rest) // 2]
    out["cold_to_first_sheet_s"] = round(marks["encoder_s"] + marks["decoder_s"] + marks["tokenizers_s"]
                                         + laps[0]["s"], 2)
    out["outputs_hash"] = hash_texts(outputs)
    if "--unload" in args:
        # Release everything but the tokenizers, then load again from the cache
        del encoder, decoder
        import gc
        gc.collect()
        out["mem_unloaded"] = memory()
        t = time.perf_counter()
        encoder = compile("openvino_encoder_model.xml")
        decoder = compile("openvino_decoder_model.xml")
        reload_s = time.perf_counter() - t
        t = time.perf_counter()
        sheet(texts[0], LANGUAGES[0])
        out["reload"] = {"load_s": round(reload_s, 2), "first_sheet_s": round(time.perf_counter() - t, 2),
                         "mem": memory()}
    out["cache_mb"] = round(sum(p.stat().st_size for p in cache.glob("*")) / 2**20)
    return out


def load_sheets() -> list[str]:
    return [p.read_text(encoding="utf-8").strip() for p in sorted(SHEETS.glob("*.md"))[:8]]


def hash_texts(texts) -> str:
    import hashlib
    return hashlib.sha256("\x00".join(texts).encode("utf-8")).hexdigest()[:16]


SPM = str(ROOT / "probe" / "tokenizers" / "spm")
VARIANTS = {
    "engine": [],
    "spm": ["--tokenizer", SPM],
    "spm-no-partials": ["--tokenizer", SPM, "--no-partials"],
    "spm-mmap-off": ["--tokenizer", SPM, "--no-mmap"],
    "spm-cache-size": ["--tokenizer", SPM, "--cache-mode", "size"],
    "spm-threads4": ["--tokenizer", SPM, "--threads", "4"],
    "spm-threads6-pcore": ["--tokenizer", SPM, "--threads", "6", "--pcore"],
    "spm-pcore": ["--tokenizer", SPM, "--pcore"],
    "spm-dq-off": ["--tokenizer", SPM, "--dq", "0"],
    "spm-size-pcore6": ["--tokenizer", SPM, "--cache-mode", "size", "--threads", "6", "--pcore"],
}


def matrix(out_path: Path, work: Path, only: list[str]):
    work.mkdir(parents=True, exist_ok=True)
    with open(out_path, "a", encoding="utf-8") as f:
        for name, extra in VARIANTS.items():
            if only and name not in only:
                continue
            cache = work / f"cache-{name}"
            for phase in ("compile", "import", "import"):
                if phase == "compile" and cache.exists():
                    for p in cache.glob("*"):
                        p.chmod(0o666)  # OpenVINO writes blobs read-only
                        p.unlink()
                run = subprocess.run([sys.executable, __file__, f"{name}:{phase}", str(cache), *extra],
                                     capture_output=True, text=True, encoding="utf-8")
                line = next((text for text in run.stdout.splitlines() if text.startswith("{")), None)
                row = json.loads(line) if line else {"label": f"{name}:{phase}", "error": run.stderr[-1500:]}
                f.write(json.dumps(row) + "\n")
                f.flush()
                print(name, phase, row.get("marks"), row.get("first_sheet_s"), row.get("warm_median_s"),
                      row.get("mem_after"), row.get("error", "")[:200], flush=True)


if __name__ == "__main__":
    if sys.argv[1] == "matrix":
        matrix(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4].split(",") if len(sys.argv) > 4 else [])
    else:
        print(json.dumps(probe(sys.argv[1], Path(sys.argv[2]), sys.argv[3:])))
