"""Word and character error rate from one rapidfuzz Levenshtein alignment, and content WER.

Content WER discounts discourse-filler backchannels but keeps those answering a question
(Doctor: "any pain?" Patient: "yeah" counts). A filler token is droppable when it is in
DISCOURSE and not in the leading run of an utterance that follows the other speaker's
question. The reference is tokenised per segment, so speaker and question structure maps onto
tokens, and that tokenisation is shared by both metrics, so they always compare.
"""
from rapidfuzz.distance import Levenshtein

from asr.normalise import normalize

DISCOURSE = {"yeah", "yep", "yea", "okay", "ok", "mm", "mhm", "mmhmm", "uhhuh", "right", "oh"}


def align(ref, hyp):
    ops = []
    for op in Levenshtein.opcodes(ref, hyp):
        if op.tag == "equal":
            for k in range(op.src_end - op.src_start):
                ops.append(("ok", ref[op.src_start + k], hyp[op.dest_start + k]))
        elif op.tag == "replace":
            sl, dl = op.src_end - op.src_start, op.dest_end - op.dest_start
            m = min(sl, dl)
            for k in range(m):
                ops.append(("sub", ref[op.src_start + k], hyp[op.dest_start + k]))
            for k in range(m, sl):
                ops.append(("del", ref[op.src_start + k], None))
            for k in range(m, dl):
                ops.append(("ins", None, hyp[op.dest_start + k]))
        elif op.tag == "delete":
            for k in range(op.src_start, op.src_end):
                ops.append(("del", ref[k], None))
        elif op.tag == "insert":
            for k in range(op.dest_start, op.dest_end):
                ops.append(("ins", None, hyp[k]))
    return ops


def error_rate(ref_text, hyp_text, char=False):
    ref = list(ref_text) if char else ref_text.split()
    hyp = list(hyp_text) if char else hyp_text.split()
    ops = align(ref, hyp)
    s = sum(o[0] == "sub" for o in ops)
    d = sum(o[0] == "del" for o in ops)
    i = sum(o[0] == "ins" for o in ops)
    return {"err": (s + d + i) / max(1, len(ref)), "S": s, "D": d, "I": i, "N": len(ref)}


def flagged_ref_tokens(ref):
    segs = sorted(({**s, "spk": spk} for spk in ("doctor", "patient") for s in ref[spk]), key=lambda s: s["start"])
    toks, flags = [], []
    for i, s in enumerate(segs):
        prev = segs[i - 1] if i else None
        follows_q = bool(prev and prev["spk"] != s["spk"] and "?" in prev["text"])
        leading = True
        for w in normalize(s["text"]).split():
            filler = w in DISCOURSE
            flags.append(filler and not (leading and follows_q))
            if not filler:
                leading = False
            toks.append(w)
    return toks, flags


def standard_and_content(ref, hyp_text):
    toks, flags = flagged_ref_tokens(ref)
    ops = align(toks, normalize(hyp_text).split())
    ss = sd = si = cs = cd = ci = kept = ri = 0
    for kind, _, h in ops:
        if kind == "ins":
            si += 1
            if h not in DISCOURSE:
                ci += 1
            continue
        drop = flags[ri]
        ri += 1
        if kind == "sub":
            ss += 1
        elif kind == "del":
            sd += 1
        if drop:
            continue
        kept += 1
        if kind == "sub":
            cs += 1
        elif kind == "del":
            cd += 1
    n = len(toks)
    return ({"err": (ss + sd + si) / max(1, n), "S": ss, "D": sd, "I": si, "N": n},
            {"err": (cs + cd + ci) / max(1, kept), "S": cs, "D": cd, "I": ci, "N": kept, "dropped": sum(flags)})
