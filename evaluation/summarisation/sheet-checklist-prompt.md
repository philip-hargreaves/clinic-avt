# Sheet checklist extraction prompt (frozen 2026-09-29)

One isolated extraction per clinical note. The checklist lists what a patient information sheet written
from this note must carry across; the sheet judge then marks each item present or absent.

---

You read ONE GP clinical note and list the items a patient information sheet written from it must carry
across, so the patient knows what they have and what to do. You do not give clinical advice.
Return STRICT JSON only, no prose, no markdown fences:

{ "items": [ { "id": "s1", "text": "...", "category": "...", "criticality": "critical" | "minor" } ] }

List, in the order the note gives them:
- "diagnosis": the stated diagnosis or working diagnosis, once. If the note states none, list nothing here.
- "medicine": each medicine to take, stop or change, as ONE item with its name, dose and how to take it
  as far as the note states them (for example "Amoxicillin 500 mg three times a day for seven days").
- "test": each test or examination arranged.
- "referral": each referral or appointment with another service.
- "follow-up": each follow-up with the practice, with its timing if stated.
- "warning-sign": each warning sign or reason to seek help that the note gives, as ONE item each (for
  example "Seek urgent help if short of breath").
- "advice": each self-care instruction the note gives (rest, fluids, avoid an activity).

Criticality: "critical" for diagnosis, medicine and warning-sign items; "minor" for the rest.

Rules:
- Take items ONLY from the note's assessment and plan and its explicit instructions. Never list history,
  symptoms, examination findings or background.
- One item per instruction; do not merge two medicines or two warning signs.
- Keep the note's own words for each item; do not add detail the note does not give.
- If the note gives nothing to carry across, return { "items": [] }.
