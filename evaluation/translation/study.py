"""Paths and constants of the translation study. Candidates, test sets and results live under
mt_root in evaluation/config.toml (EVAL_MT_ROOT)."""

import sys

from common import config
from languages import LANGUAGES as ALL_LANGUAGES

ROOT = config.path("mt_root")
APP_MODELS = config.path("app_models")
# Judged patient sheets from the accuracy tier (summarisation/generate.py app)
SHEETS = config.path("summarisation") / "notes" / "tier-accuracy-sheet"
LANGUAGES = list(ALL_LANGUAGES)
LOW_RESOURCE = ("Urdu", "Punjabi", "Bengali", "Gujarati", "Somali")
REFERENCE = "nllb-600m-int8"


def option(flag: str, default: str) -> str:
    return sys.argv[sys.argv.index(flag) + 1] if flag in sys.argv else default
