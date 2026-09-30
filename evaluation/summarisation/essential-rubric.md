# Checklist item classification rubric (fixed before any scoring)

Each line of a consultation checklist is one fact a clinician recorded as belonging in the note for
that consultation. Items are terse and in consultation order, so read each item in the context of the
items around it (for example "left" after "one side" means the pain is on the left).

Put every item in exactly one category:

| Code | Category | Includes |
|---|---|---|
| PC | Presenting complaint and its key features | the main symptom(s), onset, duration, site, character, severity, frequency, progression, associated symptoms that define the problem |
| RF | Red flags and pertinent negatives | danger symptoms asked about, whether present or denied (e.g. no blood, no fever, no chest pain, not radiating, no weight loss), and negatives that rule a serious cause in or out |
| AP | Assessment, plan and safety-netting | working diagnosis, differential, investigations, treatment, advice, referral, follow-up, when to seek help |
| MA | Medication and allergies | current medicines and doses, medicines tried for this problem, allergies |
| PH | Past and family history | past illnesses, how well controlled, previous episodes, family history |
| SH | Social history | smoking, alcohol, drugs, occupation, who they live with, functional impact on daily life |
| DT | Minor detail | exact numbers or timings that refine a fact already covered, measurement notes ("not measured"), repetitions |

Essential categories: PC, RF, AP, MA. Secondary: PH, SH, DT.

Rules:
- Classify by clinical role in this consultation, not by the word alone. "Felt hot" is RF when fever
  is being assessed; an exact stool count refining "going often" is DT.
- Symptoms reported as present that are part of the problem are PC; symptoms asked about to exclude a
  serious cause are RF, whether present or absent.
- Functional impact ("has to stay close to the toilet") is SH.
- When genuinely torn between an essential and a secondary category, choose the essential one.

Output: one line per item, `<number><TAB><code>`, nothing else, in the same order as the input.
