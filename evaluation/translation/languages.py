"""The 24 languages the app offers (models/nllb-200-600m-int8/languages.json), with each one's
FLORES-200 code, TICO-19 code where TICO has the language, and the Unicode script its letters
should be in. The first eight are the ones the selection study measured, in its order."""

LANGUAGES = {
    "Urdu": {"flores": "urd_Arab", "tico": "ur", "script": "ARABIC"},
    "Punjabi": {"flores": "pan_Guru", "tico": None, "script": "GURMUKHI"},
    "Bengali": {"flores": "ben_Beng", "tico": "bn", "script": "BENGALI"},
    "Gujarati": {"flores": "guj_Gujr", "tico": None, "script": "GUJARATI"},
    "Polish": {"flores": "pol_Latn", "tico": None, "script": "LATIN"},
    "Romanian": {"flores": "ron_Latn", "tico": None, "script": "LATIN"},
    "Arabic": {"flores": "arb_Arab", "tico": "ar", "script": "ARABIC"},
    "Somali": {"flores": "som_Latn", "tico": "so", "script": "LATIN"},
    "French": {"flores": "fra_Latn", "tico": "fr", "script": "LATIN"},
    "German": {"flores": "deu_Latn", "tico": None, "script": "LATIN"},
    "Italian": {"flores": "ita_Latn", "tico": None, "script": "LATIN"},
    "Portuguese": {"flores": "por_Latn", "tico": "pt-BR", "script": "LATIN"},
    "Spanish": {"flores": "spa_Latn", "tico": "es-LA", "script": "LATIN"},
    "Turkish": {"flores": "tur_Latn", "tico": None, "script": "LATIN"},
    "Ukrainian": {"flores": "ukr_Cyrl", "tico": None, "script": "CYRILLIC"},
    "Greek": {"flores": "ell_Grek", "tico": None, "script": "GREEK"},
    "Chinese (Simplified)": {"flores": "zho_Hans", "tico": "zh", "script": "CJK"},
    "Chinese (Traditional)": {"flores": "zho_Hant", "tico": None, "script": "CJK"},
    "Farsi": {"flores": "pes_Arab", "tico": "fa", "script": "ARABIC"},
    "Hindi": {"flores": "hin_Deva", "tico": "hi", "script": "DEVANAGARI"},
    "Kurdish (Kurmanji)": {"flores": "kmr_Latn", "tico": "ku", "script": "LATIN"},
    "Kurdish (Sorani)": {"flores": "ckb_Arab", "tico": "ckb", "script": "ARABIC"},
    "Lithuanian": {"flores": "lit_Latn", "tico": None, "script": "LATIN"},
    "Tamil": {"flores": "tam_Taml", "tico": "ta", "script": "TAMIL"},
}
ORIGINAL = list(LANGUAGES)[:8]

FLORES = {name: entry["flores"] for name, entry in LANGUAGES.items()}
TICO = {name: entry["tico"] for name, entry in LANGUAGES.items() if entry["tico"]}
SCRIPT = {name: entry["script"] for name, entry in LANGUAGES.items()}
