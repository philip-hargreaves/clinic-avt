"""Clinical-error lens: medical-concept recall, negation preservation, dose accuracy.

Concepts from scispaCy bc5cdr NER (drugs CHEMICAL, conditions DISEASE), matched on lemmas plus the
UK-to-US map so inflection and spelling are not misses. A concept is negated when an explicit cue
precedes it within a short window before any clause boundary; this avoids ConText's inferred-scope
false positives. Doses by unit-anchored regex. Needs en_ner_bc5cdr_md (the [transcription] extra).
"""
import re
from functools import cache, lru_cache

from transcription.normalise import UK_US_MEDICAL


@cache
def _nlp():
    import spacy
    return spacy.load("en_ner_bc5cdr_md", exclude=["parser"])  # NER and lemmas need no parser


_DOSE = re.compile(r"\b\d+\.?\d*\s?(?:mg|mcg|g|ml|units?|puffs?|tablets?|caps?|drops?)\b", re.I)
_CUES = {"no", "not", "never", "without", "nil", "none", "nor", "neither",
         "denies", "deny", "denied", "negative", "n't"}
_STOP = {".", "?", "!", ";", "but", "however", "although", "though", "yet"}
_STOPW = {"of", "the", "a", "an", "in", "on", "and", "or", "to", "with"}

def _lem(tok):
    l = tok.lemma_.lower()
    return UK_US_MEDICAL.get(l, l)

def _lemmas(doc):
    return [_lem(t) for t in doc if not (t.is_punct or t.is_space)]

def _negated(ent, window=5):
    doc = ent.doc
    for j in range(ent.start - 1, max(-1, ent.start - 1 - window), -1):
        t = doc[j].text.lower()
        if t in _STOP:
            break
        if t in _CUES:
            return True
    return False

_NOISE = {"um", "uh", "uhh", "hi", "hello", "k", "ok", "okay", "mm", "mhm", "mmhmm", "hmm",
          "bye", "yeah", "yep", "yes", "no", "oh", "er", "erm", "na", "ah", "huh", "mhmm", "so"}

def _is_noise(lemmas):  # NER tags fillers, greetings and one- or two-letter tokens as concepts
    return all(w in _NOISE or len(w) <= 2 for w in lemmas)

def _run(text):
    doc = _nlp()(text)
    out = {}
    for e in doc.ents:
        lem = [_lem(t) for t in e if not (t.is_punct or t.is_space)]
        key = " ".join(lem)
        if not key or _is_noise(lem):
            continue
        c = out.setdefault(key, {"key": key, "lemmas": lem, "text": e.text.lower(),
                                 "label": e.label_, "neg_n": 0, "pos_n": 0})
        if _negated(e):
            c["neg_n"] += 1
        else:
            c["pos_n"] += 1
    for c in out.values():
        c["neg"] = c["neg_n"] > 0  # any negated mention
        c["denied"] = c["neg_n"] > 0 and c["pos_n"] == 0  # every mention negated
    return out, _lemmas(doc)

@lru_cache(maxsize=256)
def _run_ref(text):  # each reference is scored against every export
    return _run(text)

def _present(concept_lemmas, hyp_lemmas):
    n = len(concept_lemmas)
    if any(hyp_lemmas[i:i + n] == concept_lemmas for i in range(len(hyp_lemmas) - n + 1)):
        return True
    if n > 1:  # NER-merged span: count present if every content word appears individually
        content = [w for w in concept_lemmas if w not in _STOPW]
        if content and all(w in hyp_lemmas for w in content):
            return True
    return False

def _hyp_negated(ref_lemmas, hyp_c):
    ref_content = {w for w in ref_lemmas if w not in _STOPW}
    for hc in hyp_c.values():  # any negated hypothesis concept sharing the content words
        hyp_content = {w for w in hc["lemmas"] if w not in _STOPW}
        if hc["neg"] and ref_content and (ref_content <= hyp_content or hyp_content <= ref_content):
            return True
    return False

def analyze(ref_text, hyp_text):
    ref_c, _ = _run_ref(ref_text)
    hyp_c, hyp_lemmas = _run(hyp_text)
    rows = []
    for key, c in ref_c.items():
        present = _present(c["lemmas"], hyp_lemmas)
        neg_ok = None
        if present and c["denied"]:  # only consistently denied findings
            neg_ok = _hyp_negated(c["lemmas"], hyp_c)
        rows.append({**c, "present": present, "neg_ok": neg_ok})

    ref_doses = {re.sub(r"\s", "", m.group().lower()) for m in _DOSE.finditer(ref_text)}
    hyp_doses = {re.sub(r"\s", "", m.group().lower()) for m in _DOSE.finditer(hyp_text)}

    drugs = [r for r in rows if r["label"] == "CHEMICAL"]
    negs  = [r for r in rows if r["denied"] and r["present"]]
    def recall(rs): return sum(r["present"] for r in rs) / len(rs) if rs else None
    return {
        "rows": rows,
        "concept_recall": recall(rows),
        "drug_recall": recall(drugs),
        "neg_preserved": (sum(r["neg_ok"] for r in negs) / len(negs)) if negs else None,
        "dose_recall": (len(ref_doses & hyp_doses) / len(ref_doses)) if ref_doses else None,
        "n_concepts": len(rows), "n_drugs": len(drugs), "n_neg": len([r for r in rows if r["denied"]]),
        "ref_doses": ref_doses, "hyp_doses": hyp_doses,
    }
