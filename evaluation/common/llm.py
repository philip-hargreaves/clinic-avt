"""A note model from the registry in evaluation/config.toml, loaded with OpenVINO GenAI and prompted
the way its entry says.

    model = NoteModel("qwen3.5-9b-int4")
    text, metrics = model.generate(prompt, max_new_tokens=1024)

Templates. "chatml" is the engine's own wrap: one user turn and an empty think block, so the
model answers without reasoning. "model" is the model's chat template from its tokenizer with
enable_thinking off; with thinking off Qwen3.8 adds no reasoning-effort block, so both Qwen
generations see the same prompt. Decoding is greedy unless a temperature is given. No CPU
fallback: a load failure raises.
"""

import re
import time

from common import config

CHATML = "<|im_start|>user\n{}<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"
CHATML_SYSTEM = "<|im_start|>system\n{}<|im_end|>\n"
_REASONING = re.compile(r"<think>.*?</think>", re.S)


class NoteModel:
    def __init__(self, name: str, device: str = "GPU"):
        import openvino_genai as ov
        if device.upper() == "CPU":
            raise SystemExit("note models run on the GPU only")
        self.ov = ov
        self.spec = config.model(name)
        self.name = name
        path = self.spec["path"]
        if not path.is_dir():
            raise SystemExit(f"{name}: no export at {path}")
        props = {k: v if isinstance(v, bool) else str(v) for k, v in self.spec["properties"].items()}
        if self.spec.get("cache"):
            props["CACHE_DIR"] = str(path / ".cache")
        pipeline = ov.VLMPipeline if self.spec["pipeline"] == "vlm" else ov.LLMPipeline
        t0 = time.perf_counter()
        self.pipe = pipeline(str(path), device, **props)
        self.load_s = time.perf_counter() - t0
        self.tokenizer = self.pipe.get_tokenizer()

    def render(self, prompt: str, system: str | None = None) -> str:
        if self.spec["template"] == "chatml":
            return (CHATML_SYSTEM.format(system) if system else "") + CHATML.format(prompt)
        messages = [{"role": "system", "content": system}] if system else []
        messages.append({"role": "user", "content": prompt})
        return self.tokenizer.apply_chat_template(messages, True, extra_context={"enable_thinking": False})

    def generate(self, prompt: str, max_new_tokens: int, system: str | None = None,
                 ignore_eos: bool = False, temperature: float | None = None) -> tuple[str, dict]:
        cfg = self.ov.GenerationConfig()
        cfg.max_new_tokens = max_new_tokens
        cfg.do_sample = temperature is not None
        if temperature is not None:
            cfg.temperature = temperature
        cfg.apply_chat_template = False
        cfg.ignore_eos = ignore_eos
        text = self.render(prompt, system)
        t0 = time.perf_counter()
        if self.spec["pipeline"] == "vlm":
            res = self.pipe.generate(text, images=[], generation_config=cfg)
        else:
            res = self.pipe.generate([text], cfg)
        wall = time.perf_counter() - t0
        pm = res.perf_metrics
        out = res.texts[0]
        if self.spec["template"] == "model":
            out = _REASONING.sub("", out)
        return out.strip(), {
            "wall_s": round(wall, 2),
            "ttft_s": round(pm.get_ttft().mean / 1000, 3),
            "tpot_ms": round(pm.get_tpot().mean, 1),
            "input_tokens": int(pm.get_num_input_tokens()),
            "output_tokens": int(pm.get_num_generated_tokens()),
            "decode_tok_s": round(pm.get_throughput().mean, 2),
            "generate_s": round(pm.get_generate_duration().mean / 1000, 3),
        }
