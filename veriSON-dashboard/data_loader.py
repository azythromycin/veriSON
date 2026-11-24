
import json
import pandas as pd


REQUIRED_COLUMNS = [
    "simulation_time_s",
    "received_mbps",
    "frames_fed",
    "rejected_udp_sends",
]


def load_jsonl(file):
    records = []

    for line_number, line in enumerate(file, start=1):
        if not line.strip():
            continue

        try:
            records.append(json.loads(line))
        except json.JSONDecodeError as error:
            raise ValueError(
                f"Invalid JSON on line {line_number}: {error}"
            )

    if not records:
        raise ValueError("JSONL file is empty.")

    df = pd.DataFrame(records)

    missing = [
        col for col in REQUIRED_COLUMNS
        if col not in df.columns
    ]

    if missing:
        raise ValueError(f"Missing columns: {missing}")

    for col in REQUIRED_COLUMNS:
        df[col] = pd.to_numeric(df[col], errors="coerce")

    if df[REQUIRED_COLUMNS].isna().any().any():
        raise ValueError("Invalid or missing telemetry values.")

    if (df[REQUIRED_COLUMNS] < 0).any().any():
        raise ValueError("Telemetry values cannot be negative.")

    df = df.sort_values("simulation_time_s").reset_index(drop=True)

    return df