# veriSON – 5G Network Telemetry Dashboard

A locally hosted dashboard built with **Python and Streamlit** to visualize telemetry data from a simulated 5G network.

## Prerequisites

- Python 3.9 or later
- Windows PowerShell
- Git (optional)

## Setup Instructions

Run the following commands from the root `veriSON` directory.

### 1. Create a Virtual Environment

```powershell
python -m venv venv
```

### 2. Activate the Virtual Environment

```powershell
.\venv\Scripts\Activate.ps1
```

**If PowerShell blocks script execution:**

Run the following command to temporarily allow script execution for the current PowerShell session:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
```

Then activate the environment again:

```powershell
.\venv\Scripts\Activate.ps1
```

### 3. Install Dependencies

```powershell
pip install streamlit pandas plotly
```

### 4. Launch the Dashboard

```powershell
streamlit run app.py
```

The dashboard will be available locally at:

**http://localhost:8501**

## Technology Stack

- **Python** – Backend and data processing
- **Streamlit** – Dashboard interface
- **Pandas** – Telemetry data processing
- **Plotly** – Interactive data visualization
- **JSONL** – Telemetry input format

## Notes

- The dashboard currently uses sample telemetry data in JSONL format.
- A JSONL file can be uploaded through the dashboard.
- Live telemetry integration with the 5G simulation will be added in a future version.

## Deactivating the Virtual Environment

When finished, run:

```powershell
deactivate
```