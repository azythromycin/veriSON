"""Refresh the original dashboard when a new telemetry snapshot arrives."""

from pathlib import Path
import runpy

import streamlit as st


telemetry = Path("data/sample_telemetry.jsonl")
snapshot_size = telemetry.stat().st_size if telemetry.exists() else 0


@st.fragment(run_every=1)
def refresh_on_telemetry():
    if telemetry.exists() and telemetry.stat().st_size != snapshot_size:
        st.rerun()


refresh_on_telemetry()
runpy.run_path(str(Path(__file__).with_name("app.py")), run_name="__main__")
