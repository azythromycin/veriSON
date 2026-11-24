"""Serve the dashboard and forward progress reports from verison-live."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory


PROGRESS = re.compile(
    r"\[sim (\d+\.\d+) s\] receiver: (\d+\.\d+) Mbps \(last 1 s\), "
    r"frames fed: (\d+), rejected UDP sends: (\d+)"
)


def forward_telemetry(stdout, telemetry):
    previous_rejected = 0
    pending = telemetry.with_suffix(".tmp")
    for line in stdout:
        print(line, end="", flush=True)
        match = PROGRESS.fullmatch(line.rstrip("\n"))
        if match is None:
            continue

        time, mbps, frames, rejected = match.groups()
        rejected = int(rejected)
        record = {
            "simulation_time_s": float(time),
            "received_mbps": float(mbps),
            "frames_fed": int(frames),
            "rejected_udp_sends": rejected - previous_rejected,
        }
        previous_rejected = rejected
        # Publish a complete snapshot so the dashboard cannot read a partial row.
        if telemetry.exists():
            shutil.copyfile(telemetry, pending)
        with pending.open("a", encoding="utf-8") as output:
            output.write(json.dumps(record) + "\n")
        pending.replace(telemetry)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8501, help="dashboard HTTP port")
    parser.add_argument(
        "simulation", nargs="?", default="verison-live",
        help='ns-3 run specification, e.g. "verison-live --runSeconds=30"',
    )
    args = parser.parse_args()
    dashboard = Path(__file__).resolve().parent

    with TemporaryDirectory(prefix="verison-dashboard-") as runtime:
        telemetry = Path(runtime) / "data" / "sample_telemetry.jsonl"
        telemetry.parent.mkdir()
        with subprocess.Popen(
            [
                sys.executable, "-B", "-m", "streamlit", "run",
                str(dashboard / "live_app.py"),
                "--server.address=127.0.0.1",
                f"--server.port={args.port}",
                "--server.headless=true",
                "--browser.gatherUsageStats=false",
            ],
            cwd=runtime,
        ) as server:
            try:
                with subprocess.Popen(
                    ["stdbuf", "-oL", "./ns3", "run", args.simulation],
                    cwd=dashboard.parent,
                    stdout=subprocess.PIPE,
                    text=True,
                ) as simulation:
                    try:
                        forward_telemetry(simulation.stdout, telemetry)
                        status = simulation.wait()
                    finally:
                        if simulation.poll() is None:
                            simulation.terminate()
                if status or server.poll() is not None:
                    return status or server.returncode
                print("Simulation finished. Press Ctrl+C to stop the dashboard.", flush=True)
                return server.wait()
            except KeyboardInterrupt:
                return 0
            finally:
                if server.poll() is None:
                    server.terminate()


if __name__ == "__main__":
    sys.exit(main())
