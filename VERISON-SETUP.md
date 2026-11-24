# veriSON 5G simulation setup

Clone the `5G-Lena` branch on Linux. Install Git, Python 3, CMake, a C++ compiler and a build tool such as Make or Ninja before configuring ns-3.

```bash
git clone --branch 5G-Lena --single-branch https://github.com/azythromycin/veriSON.git verison-5g
cd verison-5g
git clone https://gitlab.com/cttc-lena/nr.git contrib/nr
git -C contrib/nr checkout cedceadda17392c90587fb9400eb9b1f8c236713
./ns3 configure --enable-examples --enable-tests
./ns3 build
```

Replay the included camera trace from the checkout root.

```bash
./ns3 run verison-camera
```

For live operation, run your camera metadata sender on the same machine and direct it to UDP `127.0.0.1:5005`. Run the simulator from the checkout root.

```bash
./ns3 run verison-live
```

To run the live simulation with the dashboard, use the dashboard launcher from the checkout root. It starts the simulation for you and serves the dashboard on port 8501.

```bash
python3 -m venv veriSON-dashboard/.venv
veriSON-dashboard/.venv/bin/python -m pip install -r veriSON-dashboard/requirement.txt
veriSON-dashboard/.venv/bin/python veriSON-dashboard/run_live.py
```

To view the bundled sample, run Streamlit from `veriSON-dashboard` and upload `data/verison_sample_telemetry.jsonl` in the sidebar.

```bash
cd veriSON-dashboard
.venv/bin/python -m streamlit run app.py
```
