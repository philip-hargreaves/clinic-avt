"""The medical knowledge suite (MedQA, MedMCQA, PubMedQA and six MMLU medical subjects), zero-shot
multiple choice scored by loglikelihood through lm-evaluation-harness, on the iGPU.

Two backends, one scoring rule:
  optimum  an LLM export (openvino_model.xml) through lm-eval's OpenVINO model; the backend of
           every banked result
  split    a VLM split (openvino_language_model.xml with its text embeddings) driven directly on
           ov.Core: embed the tokens, run the language model once over context + continuation,
           sum the continuation's log-probabilities. The vision parts are never loaded.
The backend is chosen from the folder. Models whose tokenizer omits BOS but were trained with
one (Gemma, Mistral; "bos" in evaluation/config.toml) get it prepended, or scores collapse to chance.

    python evaluation/knowledge/run.py qwen3.5-9b-int4-ov
    python evaluation/knowledge/run.py gemma-4-31b-it-int4-ov --limit 5          # smoke: 5 per task
    python evaluation/knowledge/run.py --table [folder]                           # average, MMLU medical, per task
"""
import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.io import write_json  # noqa: E402

ALLOC_CAP = 4_294_959_104   # the iGPU's single-allocation cap; the automatic batch stays under it
MAX_SEQ = 1100              # longest context in the suite, for the batch estimate

os.environ.setdefault("HF_DATASETS_TRUST_REMOTE_CODE", "1")
os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")


def optimum_lm(spec, device, batch):
    from lm_eval.models.optimum_lm import OptimumLM
    cfg = json.loads((spec["path"] / "config.json").read_text())
    if batch == "auto":
        batch = max(1, ALLOC_CAP // (MAX_SEQ * cfg["vocab_size"] * 4))  # logits bytes per sample
    lm = OptimumLM(pretrained=str(spec["path"]), device=device, batch_size=int(batch))
    if spec["bos"]:
        # lm-eval's add_bos_token only toggles add_special_tokens, which this tokenizer ignores
        lm.tokenizer.add_bos_token = True
    return lm


def split_lm(spec, device):
    import numpy as np
    import openvino as ov
    from lm_eval.api.model import TemplateLM
    from tokenizers import Tokenizer

    path = spec["path"]
    cfg = json.loads((path / "config.json").read_text())
    text_cfg = cfg.get("text_config", cfg)

    def token_id(key):
        value = cfg.get(key, text_cfg.get(key))
        return value[0] if isinstance(value, list) else value

    class SplitLM(TemplateLM):
        def __init__(self):
            super().__init__()
            core = ov.Core()
            self.embed = core.compile_model(path / "openvino_text_embeddings_model.xml", device)
            per_layer = path / "openvino_text_embeddings_per_layer_model.xml"
            self.per_layer = core.compile_model(per_layer, device) if per_layer.exists() else None
            model = core.read_model(path / "openvino_language_model.xml")
            self.inputs = {i.get_any_name(): i.get_partial_shape() for i in model.inputs}
            props = {k: str(v) for k, v in spec["properties"].items()}
            self.request = core.compile_model(model, device, props).create_infer_request()
            self.tokenizer_json = Tokenizer.from_file(str(path / "tokenizer.json"))
            self.bos = token_id("bos_token_id") if spec["bos"] else None
            self.eos = token_id("eos_token_id")

        @property
        def eot_token_id(self):
            return self.eos

        @property
        def prefix_token_id(self):
            return self.bos if self.bos is not None else self.eos

        def tok_encode(self, string, add_special_tokens=None, **kwargs):
            ids = self.tokenizer_json.encode(string, add_special_tokens=False).ids
            return [self.bos] + ids if self.bos is not None and add_special_tokens is not False else ids

        def logits(self, ids):
            ids = np.array([ids], dtype=np.int64)
            n = ids.shape[1]
            feed = {"inputs_embeds": self.embed(ids)[0], "attention_mask": np.ones((1, n), dtype=np.int64),
                    "beam_idx": np.zeros(1, dtype=np.int32)}
            positions = np.arange(n, dtype=np.int64)[None, :]
            rank = self.inputs["position_ids"].rank.get_length()
            # Qwen VL models take one row per rope section; text alone puts the same positions in each
            feed["position_ids"] = np.tile(positions, (4, 1, 1)) if rank == 3 else positions
            if "per_layer_inputs" in self.inputs:
                feed["per_layer_inputs"] = self.per_layer(ids)[0]
            if "token_type_ids" in self.inputs:
                feed["token_type_ids"] = np.zeros((1, n), dtype=np.int64)
            self.request.reset_state()
            self.request.infer(feed)
            return self.request.get_tensor("logits").data[0]

        def _loglikelihood_tokens(self, requests, disable_tqdm=False, **kwargs):
            out = []
            for _, context, continuation in requests:
                whole = (context + continuation)[-(MAX_SEQ + 1):]
                logits = self.logits(whole[:-1])[-len(continuation):].astype(np.float64)
                logits -= logits.max(axis=-1, keepdims=True)
                logprobs = logits - np.log(np.exp(logits).sum(axis=-1, keepdims=True))
                picked = logprobs[np.arange(len(continuation)), continuation]
                out.append((float(picked.sum()), bool((logits.argmax(axis=-1) == continuation).all())))
            return out

        def loglikelihood_rolling(self, requests, disable_tqdm=False):
            raise NotImplementedError("the knowledge suite is multiple choice only")

        def generate_until(self, requests, disable_tqdm=False):
            raise NotImplementedError("the knowledge suite is multiple choice only")

    return SplitLM()


def run(name, device, batch, limit, num_fewshot):
    import lm_eval
    spec = config.model(name)
    split = spec["pipeline"] == "vlm"
    lm = split_lm(spec, device) if split else optimum_lm(spec, device, batch)
    tasks = config.section("knowledge")["tasks"]
    print(f"{name} | {'split' if split else 'optimum'} | {device} | {num_fewshot}-shot | {len(tasks)} tasks"
          + (f" | {limit} per task" if limit else ""), flush=True)
    results = lm_eval.simple_evaluate(model=lm, tasks=tasks, num_fewshot=num_fewshot, limit=limit or None)["results"]
    folder = config.out("knowledge", "results" if not limit else "smoke")
    write_json(folder / f"{name}.json", results)
    print_results(results)
    print(f"-> {folder / f'{name}.json'}")


def print_results(results):
    accs = []
    for task, m in results.items():
        if m.get("acc,none") is not None:
            accs.append(m["acc,none"])
            print(f"  {task:30s} {m['acc,none'] * 100:5.1f}")
    if accs:
        print(f"  {'average':30s} {sum(accs) / len(accs) * 100:5.1f}")


def table(folder):
    tasks = config.section("knowledge")["tasks"]
    rows = []
    for path in sorted(folder.glob("*.json")):
        res = json.loads(path.read_text(encoding="utf-8"))
        accs = [res[t]["acc,none"] * 100 for t in tasks if t in res and res[t].get("acc,none") is not None]
        if accs:
            rows.append((path.stem, len(accs), sum(accs) / len(accs), {t: res[t]["acc,none"] * 100 for t in tasks if t in res}))
    short = [t.replace("mmlu_", "").replace("_4options", "") for t in tasks]
    mmlu = [t for t in tasks if t.startswith("mmlu_")]
    print("| model | tasks | average | MMLU medical | " + " | ".join(short) + " |")
    print("|---|---|---|---|" + "---|" * len(tasks))
    for name, n, avg, per in sorted(rows, key=lambda r: -r[2]):
        sub = [per[t] for t in mmlu if t in per]
        print(f"| {name} | {n} | {avg:.1f} | {sum(sub) / len(sub):.1f} | "
              + " | ".join(f"{per[t]:.1f}" if t in per else "-" for t in tasks) + " |")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model", nargs="?", help="a model name from evaluation/config.toml")
    ap.add_argument("--device", default="GPU.0")
    ap.add_argument("--batch", default="auto")
    ap.add_argument("--limit", type=int, default=0, help="questions per task, for a smoke run")
    ap.add_argument("--num-fewshot", type=int, default=0)
    ap.add_argument("--table", nargs="?", const="", help="print the results table (optionally of another folder)")
    args = ap.parse_args()
    if args.table is not None:
        table(Path(args.table) if args.table else config.out("knowledge", "results"))
    elif args.model:
        run(args.model, args.device, args.batch, args.limit, args.num_fewshot)
    else:
        ap.error("give a model or --table")


if __name__ == "__main__":
    main()
