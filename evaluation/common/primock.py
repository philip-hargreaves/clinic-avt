"""PriMock57: consultation ids and the Praat TextGrid transcripts (one tier per speaker file).

    consults()                    sorted ids, e.g. day1_consultation01
    intervals(path)               [(start, end, raw text)] for every interval, empty ones included
    clean(text)                   markup removed: uncertain words kept, unintelligible dropped
    turns(consult)                [(start, end, speaker, clean text)], doctor then patient, unsorted
"""

import re
from pathlib import Path

from common import config

SPEAKERS = ("doctor", "patient")

# The lookahead to the next interval, tier or end keeps escaped "" inside a text
_INTERVAL = re.compile(
    r'intervals \[\d+\]:\s*xmin = ([\d.eE+-]+)\s*xmax = ([\d.eE+-]+)\s*'
    r'text = "(.*?)"\s*(?=intervals \[|item \[|\Z)',
    re.DOTALL,
)
_UNSURE = re.compile(r"</?UNSURE>")
_TAG = re.compile(r"<[^>]+>")
_SPACE = re.compile(r"\s+")


def transcripts_dir() -> Path:
    return config.path("primock57") / "transcripts"


def consults() -> list[str]:
    return sorted(p.name[:-len("_doctor.TextGrid")] for p in transcripts_dir().glob("*_doctor.TextGrid"))


def textgrid(consult: str, speaker: str) -> Path:
    return transcripts_dir() / f"{consult}_{speaker}.TextGrid"


def intervals(path: Path) -> list[tuple[float, float, str]]:
    raw = Path(path).read_text(encoding="utf-8", errors="replace")
    return [(float(m.group(1)), float(m.group(2)), m.group(3).replace('""', '"'))
            for m in _INTERVAL.finditer(raw)]


def clean(text: str) -> str:
    # <UNSURE> keeps its words; <UNIN/>, <INAUDIBLE_SPEECH/> and any other tag become a gap
    text = _UNSURE.sub("", text)
    text = _TAG.sub(" ", text)
    return _SPACE.sub(" ", text).strip()


def turns(consult: str) -> list[tuple[float, float, str, str]]:
    out = []
    for speaker in SPEAKERS:
        for start, end, text in intervals(textgrid(consult, speaker)):
            text = clean(text)
            if text:
                out.append((start, end, speaker, text))
    return out
