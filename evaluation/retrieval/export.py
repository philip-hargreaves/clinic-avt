"""Export shortlist candidates to OpenVINO IR with provenance.

  python export.py bge-base                 fp16 and int8
  python export.py bge-base --precision int8
  python export.py --role embedder          every enabled embedder
  python export.py --all
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common.io import log, write_json  # noqa: E402
from harness import CANDIDATES, candidate, candidate_dir, load_shortlist  # noqa: E402


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(hf_id: str, extra: list[str]) -> tuple[Path, str]:
    # In-process, so the IPv4 fix applies. local_dir needs no symlinks on exFAT
    from huggingface_hub import HfApi, snapshot_download
    local = CANDIDATES / ".src" / hf_id.replace("/", "--")
    snapshot_download(hf_id, local_dir=str(local),
                      allow_patterns=["*.json", "*.txt", "*.model", "*.safetensors", "*.bin", "*.py", *extra])
    return local, HfApi().model_info(hf_id).sha


def export_from_onnx(entry: dict, precision: str, snapshot: Path, out: Path) -> None:
    # Architectures optimum cannot export yet (ModernBERT) ship an ONNX file to convert instead
    import openvino as ov
    model = ov.Core().read_model(str(snapshot / entry["onnx"]))
    if precision == "int8":
        import nncf
        model = nncf.compress_weights(model, mode=nncf.CompressWeightsMode.INT8_ASYM)
    out.mkdir(parents=True, exist_ok=True)
    ov.save_model(model, str(out / "openvino_model.xml"), compress_to_fp16=precision == "fp16")
    for name in ("config.json", "tokenizer.json", "tokenizer_config.json", "special_tokens_map.json"):
        if (snapshot / name).exists():
            shutil.copy(snapshot / name, out / name)


def export_one(entry: dict, precision: str) -> Path:
    out = candidate_dir(entry["id"], precision)
    if (out / "openvino_model.xml").exists():
        log(f"{out.name}: exists, skipped")
        return out
    snapshot, sha = download(entry["hf"], [entry["onnx"]] if entry.get("onnx") else [])
    if entry.get("onnx"):
        cmd = ["ov.Core().read_model", str(snapshot / entry["onnx"]), "nncf INT8_ASYM" if precision == "int8" else "fp16", str(out)]
        log(" ".join(cmd))
        export_from_onnx(entry, precision, snapshot, out)
    else:
        optimum_cli = Path(sys.executable).with_name("optimum-cli.exe")
        cmd = [str(optimum_cli), "export", "openvino", "--model", str(snapshot), "--task", entry["export_task"],
               "--weight-format", precision, str(out)]
        log(" ".join(cmd))
        # Quantisation writes an fp32 copy to TEMP first, which has to stay off C:
        tmp = CANDIDATES / ".tmp"
        tmp.mkdir(exist_ok=True)
        subprocess.run(cmd, check=True, env={**os.environ, "TEMP": str(tmp), "TMP": str(tmp)})
    files = sorted(p for p in out.rglob("*") if p.is_file())
    write_json(out / "provenance.json", {
        "id": entry["id"],
        "hf": entry["hf"],
        "revision": sha,
        "precision": precision,
        "task": entry["export_task"],
        "command": " ".join(cmd),
        "exported_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "files": {p.relative_to(out).as_posix(): {"sha256": sha256(p), "bytes": p.stat().st_size} for p in files},
    })
    config = json.loads((out / "config.json").read_text(encoding="utf-8"))
    log(f"{out.name}: architectures={config.get('architectures')} model_type={config.get('model_type')}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ids", nargs="*")
    ap.add_argument("--role", choices=["embedder", "reranker", "baseline"])
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--precision", choices=["fp16", "int8", "both"], default="both")
    ap.add_argument("--include-disabled", action="store_true")
    args = ap.parse_args()

    entries = load_shortlist()
    if args.ids:
        entries = [candidate(i) for i in args.ids]
    elif args.role:
        entries = [e for e in entries if e["role"] == args.role]
    elif not args.all:
        ap.error("give ids, --role or --all")
    entries = [e for e in entries if e.get("enabled", True) or args.include_disabled or args.ids]
    precisions = ["fp16", "int8"] if args.precision == "both" else [args.precision]
    failed = []
    for entry in entries:
        for precision in precisions:
            try:
                export_one(entry, precision)
            except subprocess.CalledProcessError as e:
                # A failed export leaves a partial directory. Remove it so a retry starts clean
                shutil.rmtree(candidate_dir(entry["id"], precision), ignore_errors=True)
                failed.append(f"{entry['id']}-{precision}")
                log(f"{entry['id']}-{precision}: export failed (exit {e.returncode})")
    if failed:
        raise SystemExit("failed: " + ", ".join(failed))


if __name__ == "__main__":
    main()
