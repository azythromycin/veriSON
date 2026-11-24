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

### Live camera

The camera sender needs Linux, a V4L2 webcam, and a graphical desktop for the OpenCV preview. It opens camera index 0 by default. Install the Python dependencies and download the same YOLOv8n checkpoint used to develop the sender.

```bash
python3 -m venv camera/.venv
camera/.venv/bin/python -m pip install -r camera/requirements.txt
curl -fL https://github.com/ultralytics/assets/releases/download/v8.1.0/yolov8n.pt -o camera/yolov8n.pt
printf '%s\n' '31e20dde3def09e2cf938c7be6fe23d9150bbbe503982af13345706515f2ef95  camera/yolov8n.pt' | sha256sum -c
```

Start the live simulator from the checkout root. It listens for camera metadata on UDP port 5005 and runs for five minutes by default.

```bash
./ns3 run verison-live
```

For the live dashboard, use the launcher in place of the command above. It starts the simulator and serves the dashboard on port 8501.

```bash
python3 -m venv veriSON-dashboard/.venv
veriSON-dashboard/.venv/bin/python -m pip install -r veriSON-dashboard/requirement.txt
veriSON-dashboard/.venv/bin/python veriSON-dashboard/run_live.py
```

Once the simulator reports that it is listening, open a second terminal on the same machine and run this from the checkout root.

```bash
camera/.venv/bin/python camera/yolo_camera.py
```

The sender sends frame metadata to `127.0.0.1:5005` and writes `camera/camera_trace.csv`. Press Escape in the preview window to stop. Each run overwrites the previous capture. To replay a new capture, run:

```bash
./ns3 run 'verison-camera --traceFile=camera/camera_trace.csv'
```

To view the bundled sample, run Streamlit from `veriSON-dashboard` and upload `data/verison_sample_telemetry.jsonl` in the sidebar.

```bash
cd veriSON-dashboard
.venv/bin/python -m streamlit run app.py
```
