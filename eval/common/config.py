"""The suite's one configuration: eval/config.toml, with EVAL_<NAME> environment overrides.

    from common import config
    config.path("primock57")        # Path, override EVAL_PRIMOCK57
    config.out("asr")               # build/eval/asr, created
    config.model("qwen3.5-9b-int4") # dict with path, pipeline, template, properties, label
"""

import json
import os
import re
import tomllib
from functools import cache
from pathlib import Path

EVAL = Path(__file__).resolve().parents[1]
REPO = EVAL.parent


@cache
def settings() -> dict:
    with open(EVAL / "config.toml", "rb") as f:
        return tomllib.load(f)


def _expand(value: str) -> Path:
    value = re.sub(r"\{(\w+)\}", lambda m: str(path(m.group(1))), value)
    p = Path(value).expanduser()
    return p if p.is_absolute() else REPO / p


def path(name: str) -> Path:
    override = os.environ.get(f"EVAL_{name.upper()}")
    if override:
        return Path(override)
    if name not in settings()["paths"]:
        raise SystemExit(f"no path '{name}' in eval/config.toml")
    return _expand(settings()["paths"][name])


def out(*parts: str) -> Path:
    p = path("out").joinpath(*parts)
    p.mkdir(parents=True, exist_ok=True)
    return p


def section(name: str) -> dict:
    return settings().get(name, {})


def model(name: str) -> dict:
    entries = settings()["models"]
    if name not in entries:
        raise SystemExit(f"unknown model '{name}'; eval/config.toml lists: {', '.join(entries)}")
    entry = dict(entries[name])
    entry["name"] = name
    entry["path"] = _expand(entry["path"])
    entry.setdefault("label", name)
    entry.setdefault("template", "model")
    entry.setdefault("bos", False)
    props = dict(entry.get("properties", {}))
    manifest = entry["path"] / "manifest.json"
    if manifest.exists():
        runtime = json.loads(manifest.read_text(encoding="utf-8")).get("runtime", {})
        entry.setdefault("pipeline", runtime.get("pipeline"))
        props = {**runtime.get("properties", {}), **props}
    entry["properties"] = props
    if not entry.get("pipeline"):
        entry["pipeline"] = "vlm" if (entry["path"] / "openvino_language_model.xml").exists() else "llm"
    return entry


def label(name: str) -> str:
    return settings()["models"].get(name, {}).get("label", name)
