"""Normalisation, applied identically to reference and hypothesis.

normalize()      Whisper's EnglishTextNormalizer plus the clinical UK-to-US spellings it misses
normalize_raw()  lowercase and strip punctuation only, to show the cosmetic gap
"""
import re

from transcription.normalizers import EnglishTextNormalizer

_EN = EnglishTextNormalizer()

# Clinical spellings english.json misses, keyed by the normaliser's output word forms
UK_US_MEDICAL = {
    "haematoma": "hematoma", "haemoglobin": "hemoglobin",
    "haemorrhage": "hemorrhage", "haemorrhoids": "hemorrhoids", "haematuria": "hematuria",
    "haemolysis": "hemolysis", "haematology": "hematology", "haemophilia": "hemophilia",
    "ischaemia": "ischemia", "ischaemic": "ischemic", "leukaemia": "leukemia",
    "anaemia": "anemia", "anaemic": "anemic", "bacteraemia": "bacteremia",
    "septicaemia": "septicemia", "hypercalcaemia": "hypercalcemia", "hypoglycaemia": "hypoglycemia",
    "hyperglycaemia": "hyperglycemia", "hypokalaemia": "hypokalemia", "hyperkalaemia": "hyperkalemia",
    "hyponatraemia": "hyponatremia", "uraemia": "uremia",
    "oedema": "edema", "oesophagus": "esophagus", "oesophageal": "esophageal",
    "coeliac": "celiac", "foetal": "fetal", "foetus": "fetus", "oestrogen": "estrogen",
    "diarrhoea": "diarrhea", "gonorrhoea": "gonorrhea",
    "paediatric": "pediatric", "paediatrics": "pediatrics", "gynaecology": "gynecology",
    "gynaecological": "gynecological", "orthopaedic": "orthopedic", "anaesthetic": "anesthetic",
    "anaesthesia": "anesthesia", "dyspnoea": "dyspnea", "apnoea": "apnea", "tachypnoea": "tachypnea",
    "orthopnoea": "orthopnea", "oedematous": "edematous", "haemodynamic": "hemodynamic",
}

_PUNCT = re.compile(r"[^\w\s']")

def normalize(text: str) -> str:
    s = _EN(text or "")
    if not s:
        return ""
    toks = [UK_US_MEDICAL.get(t, t) for t in s.split()]
    return " ".join(toks)

def normalize_raw(text: str) -> str:
    s = (text or "").lower()
    s = _PUNCT.sub(" ", s)
    return re.sub(r"\s+", " ", s).strip()
