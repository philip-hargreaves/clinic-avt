# End-to-end: one replayed consult through the real engine, then the title
# must be in session/list. One GPU job.
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perf_loop as pl  # noqa: E402


def main():
    os.makedirs(pl.LOGS, exist_ok=True)
    engine = pl.Engine(990)
    try:
        engine.wait_up()
        result, rtt = pl.timed(engine, "session/start", {
            "replay": {"path": os.path.join(pl.HERE, "audio", "c02m_elbow.wav"),
                       "speed": 16.0, "monitor": False}}, 30)
        print("started:", result, round(rtt, 2))
        # Drain the replay (120 s at 16x), then stop like the shell would
        end = time.time() + 120 / 16.0 + 2
        while time.time() < end:
            engine.next_notification(1)
        result, rtt = pl.timed(engine, "session/stop", None, 180)
        print("stopped:", result, round(rtt, 2))
        deadline = time.time() + 300
        got_patient = False
        seen = {}
        while time.time() < deadline and not got_patient:
            n = engine.next_notification(5)
            if n is None:
                continue
            method = n[1].get("method")
            seen[method] = seen.get(method, 0) + 1
            if method == "patient/ready":
                got_patient = True
            if method in ("note/failed", "patient/failed", "session/interrupted"):
                print("FAILED:", n[1])
                print("seen:", seen)
                return 1
        print("seen:", seen)
        if not got_patient:
            print("FAILED: no patient/ready in 300 s")
            return 1
        # The label follows the sheet; give the short generation a moment
        for _ in range(60):
            row = engine.request("session/list")["sessions"][0]
            if row.get("label"):
                print("TITLE:", repr(row["label"]))
                print("EDITED:", row.get("editedAt"))
                return 0
            time.sleep(1)
        print("FAILED: no label appeared;", row)
        return 1
    finally:
        for line in engine.new_log_lines()[-12:]:
            print("  engine:", line)
        engine.close()


if __name__ == "__main__":
    sys.exit(main())
