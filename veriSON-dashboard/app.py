
import streamlit as st
import pandas as pd
import plotly.express as px

from pathlib import Path
from data_loader import load_jsonl


st.set_page_config(
    page_title="5G Network Telemetry",
    page_icon="📡",
    layout="wide"
)

st.title("📡 5G Network Telemetry Dashboard")
st.caption("Simulated 5G Network Performance Monitoring")


# Sidebar: Upload telemetry
st.sidebar.header("Telemetry Input")

uploaded_file = st.sidebar.file_uploader(
    "Upload JSONL file",
    type=["jsonl", "ndjson", "txt"]
)

default_file = Path("data/sample_telemetry.jsonl")

if uploaded_file is not None:
    data_source = uploaded_file
elif default_file.exists():
    data_source = default_file.open("rb")
else:
    data_source = None

if data_source is None:
    st.warning("Please upload a telemetry JSONL file.")
    st.stop()

try:
    df = load_jsonl(data_source)
except Exception as error:
    st.error(f"Error loading telemetry: {error}")
    st.stop()
finally:
    if uploaded_file is None and data_source is not None:
        data_source.close()


# Calculate metrics
latest_throughput = df["received_mbps"].iloc[-1]

total_frames = df["frames_fed"].iloc[-1]

total_rejected = df["rejected_udp_sends"].sum()


# Metric cards
st.subheader("Network Overview")

col1, col2, col3 = st.columns(3)

col1.metric(
    "Received Throughput",
    f"{latest_throughput:.2f} Mbps"
)

col2.metric(
    "Frames Fed",
    f"{total_frames:,.0f}"
)

col3.metric(
    "Rejected UDP Sends",
    f"{total_rejected:,.0f}"
)

st.divider()


# Graph 1: Received throughput
st.subheader("Received Throughput Over Simulation Time")

fig1 = px.line(
    df,
    x="simulation_time_s",
    y="received_mbps",
    markers=True,
    labels={
        "simulation_time_s": "Simulation Time (seconds)",
        "received_mbps": "Received Throughput (Mbps)"
    }
)

st.plotly_chart(fig1, use_container_width=True)


# Graph 2: Frames fed
st.subheader("Frames Fed Over Simulation Time")

fig2 = px.line(
    df,
    x="simulation_time_s",
    y="frames_fed",
    markers=True,
    labels={
        "simulation_time_s": "Simulation Time (seconds)",
        "frames_fed": "Frames Fed"
    }
)

st.plotly_chart(fig2, use_container_width=True)


# Graph 3: Rejected UDP sends
st.subheader("Rejected UDP Sends Over Simulation Time")

fig3 = px.bar(
    df,
    x="simulation_time_s",
    y="rejected_udp_sends",
    labels={
        "simulation_time_s": "Simulation Time (seconds)",
        "rejected_udp_sends": "Rejected UDP Sends"
    }
)

st.plotly_chart(fig3, use_container_width=True)


# Raw data
with st.expander("View Raw Telemetry"):
    st.dataframe(df, use_container_width=True)