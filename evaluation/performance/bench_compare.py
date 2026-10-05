"""Compares bench_all.py runs from different machines, with one table and grouped bar charts
per measure.

    python evaluation/performance/bench_compare.py <run dir> <run dir> [...] [--out <dir>]

Each run needs its summary.json from bench_report.py. Writes compare.md and figures/ to --out
(default build/perf-loop/bench/compare).
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common import config  # noqa: E402

ARMS = ["4b-gpu", "9b-gpu", "35b-gpu", "4b-npu", "9b-npu", "35b-npu"]
MEASURES = [
    ("stop_to_first_words_s", "Stop to first note words (s)", lambda a: a.get("stop_to_first_words_s")),
    ("sheet_done_s", "Stop to sheet done (s)", lambda a: a.get("sheet_done_s")),
    ("finalise_s", "Finalise (s)", lambda a: a.get("finalise_s")),
    ("note_tokens_per_s", "Note decode (tokens/s)", lambda a: a.get("note_tokens_per_s")),
    ("asr_rtf", "Speech recognition (x real time)", lambda a: a.get("asr_rtf")),
    ("capture_above_w", "Power while recording, above idle (W)",
     lambda a: (a.get("energy") or {}).get("capture_above_w")),
    ("above_wh", "Energy per consultation, above idle (Wh)", lambda a: (a.get("energy") or {}).get("above_wh")),
    ("day_wh", "A day of 30 consultations (Wh)", lambda a: (a.get("energy") or {}).get("day_wh")),
    ("memory_gb", "Engine and note host memory (GB)",
     lambda a: ((a.get("memory") or {}).get("engine_mb") or 0) / 1024
     + ((a.get("memory") or {}).get("note_host_mb") or 0) / 1024 or None),
]


def label(summary):
    m = summary.get("machine", {})
    cpu = (m.get("cpu") or "").replace("Intel(R) Core(TM) ", "").replace("(TM)", "")
    return f"{m.get('host')} ({cpu}, {m.get('ram_gb')} GB)"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runs", nargs="+")
    parser.add_argument("--out", default=str(config.path("perf_loop") / "bench" / "compare"))
    args = parser.parse_args()
    out = Path(args.out)
    (out / "figures").mkdir(parents=True, exist_ok=True)
    runs = []
    for d in args.runs:
        summary = json.loads((Path(d) / "summary.json").read_text(encoding="utf-8"))
        arms = {k.split("#")[0]: v for k, v in summary["arms"].items() if k.endswith("#1")}
        runs.append((label(summary), arms, summary))

    lines = ["# Machine comparison\n"]
    for name, _, s in runs:
        m = s["machine"]
        lines.append(f"- **{name}:** {m.get('model')}; GPUs {m.get('gpus')}; NPU {m.get('npu')}; "
                     f"idle package {s.get('idle_pkg_w') and round(s['idle_pkg_w'], 1)} W")
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        plt = None
    for key, title, get in MEASURES:
        lines.append(f"\n## {title}\n")
        lines.append("| Arm | " + " | ".join(n for n, _, _ in runs) + " |")
        lines.append("|---|" + "---|" * len(runs))
        present = [a for a in ARMS if any(a in arms for _, arms, _ in runs)]
        for arm in present:
            cells = []
            for _, arms, _ in runs:
                v = get(arms[arm]) if arm in arms else None
                cells.append("–" if v is None else f"{v:.2f}")
            lines.append(f"| {arm} | " + " | ".join(cells) + " |")
        if plt and present:
            fig, ax = plt.subplots(figsize=(8, 3))
            width = 0.8 / len(runs)
            for i, (name, arms, _) in enumerate(runs):
                xs = [j + i * width for j in range(len(present))]
                ax.bar(xs, [(get(arms[a]) if a in arms else None) or 0 for a in present], width, label=name)
            ax.set_xticks([j + width * (len(runs) - 1) / 2 for j in range(len(present))], present)
            ax.set_ylabel(title, fontsize=8)
            ax.legend(fontsize=7)
            fig.tight_layout()
            fig.savefig(out / "figures" / f"{key}.png", dpi=200)
            plt.close(fig)
            lines.append(f"\n![{title}](figures/{key}.png)")
    (out / "compare.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(out / "compare.md")


if __name__ == "__main__":
    main()
