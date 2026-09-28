# Enrol a voiceprint through the real engine from a wav standing in for the microphone,
# and stage the resulting anchor for a sweep. Validates the enrolment path end to end and
# gives the 57-consultation gate an enrolled print to compare with the accrued one.
#   EVAL_ENGINE=<engine exe> python evaluation/diarisation/enrol_anchor.py <doctor wav> <out anchor.bin> [seconds]
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from performance import perf_loop  # noqa: E402

wav, out = sys.argv[1], sys.argv[2]
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 50.0

os.makedirs(perf_loop.LOGS, exist_ok=True)
# The replay wav rides in as the engine's fourth positional argument
engine = perf_loop.Engine(9000, extra_args=[wav])
try:
    engine.wait_up()  # up, without waiting on the note model's compile cache
    engine.request("anchor/clear")
    before = engine.request("anchor/status")
    print("before:", before)
    engine.request("anchor/enrol", {"seconds": seconds})
    outcome = None
    t0 = time.time()
    progress = 0
    while outcome is None and time.time() - t0 < seconds + 120:
        item = engine.next_notification(1)
        if item is None:
            continue
        method, params = item[1].get("method"), item[1].get("params", {})
        if method == "anchor/progress":
            progress += 1
        elif method == "anchor/enrolled":
            outcome = params
    print(f"progress notifications: {progress}")
    print("outcome:", outcome)
    after = engine.request("anchor/status")
    print("after:", after)
    if not outcome or not outcome.get("ok"):
        sys.exit(1)
finally:
    engine.close()

shutil.copyfile(os.path.join(perf_loop.STORE, "anchor.bin"), out)
print("staged", out)
