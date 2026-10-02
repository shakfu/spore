"""Run the Autobahn fuzzing client (Docker) against spored's /ws/echo.

Usage: run.py SPORED REPORT_DIR. Exits nonzero if any case FAILED.

Excluded in fuzzingclient.json: messages over spore_ws_config.max_message
(1 MiB; 9.1.4-6, 9.2.4-6, 9.3, 9.4) and permessage-deflate (12, 13),
which spore does not offer."""

import collections
import json
import os
import subprocess
import sys

spored, reports = sys.argv[1], os.path.abspath(sys.argv[2])
config = os.path.dirname(os.path.abspath(__file__))
os.makedirs(reports, exist_ok=True)

srv = subprocess.Popen([spored, "--port", "9001"], stdout=subprocess.PIPE, text=True)
try:
    line = srv.stdout.readline()
    if "listening" not in line:
        sys.exit(f"spored did not start: {line!r}")
    subprocess.run(
        ["docker", "run", "--rm", "--network", "host",
         "-v", f"{config}:/config:ro", "-v", f"{reports}:/reports",
         os.environ.get("AUTOBAHN_IMAGE", "crossbario/autobahn-testsuite"),
         "wstest", "-m", "fuzzingclient", "-s", "/config/fuzzingclient.json"],
        check=True,
    )
finally:
    srv.terminate()
    srv.wait()

counts = collections.Counter()
failed = set()
with open(os.path.join(reports, "index.json")) as f:
    for agent, cases in json.load(f).items():
        for case, r in cases.items():
            for key in ("behavior", "behaviorClose"):
                counts[r[key]] += 1
                if r[key] == "FAILED":
                    failed.add(f"{case} {key}")
print(", ".join(f"{k}: {v}" for k, v in sorted(counts.items())))
for f in sorted(failed):
    print("FAILED", f)
sys.exit(1 if failed or not counts else 0)
