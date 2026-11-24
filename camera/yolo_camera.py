
import cv2
import csv
import time
import math
import socket
from collections import deque
from pathlib import Path
from ultralytics import YOLO

# Configuration
CAMERA_INDEX = 0
CONFIDENCE = 0.60
JPEG_QUALITY = 80
PACKET_PAYLOAD_BYTES = 1200

MODEL_PATH = Path(__file__).parent / "yolov8n.pt"
CSV_PATH = Path(__file__).parent / "camera_trace.csv"

# Live telemetry configuration
TELEMETRY_HOST = "127.0.0.1"
TELEMETRY_PORT = 5005

# UDP is used only to send metadata to our local
# ns-3 bridge. This is NOT the simulated 5G link.
telemetry_socket = socket.socket(
    socket.AF_INET,
    socket.SOCK_DGRAM
)

telemetry_destination = (
    TELEMETRY_HOST,
    TELEMETRY_PORT
)

model = YOLO(str(MODEL_PATH))

cap = cv2.VideoCapture(
    CAMERA_INDEX,
    cv2.CAP_V4L2
)

if not cap.isOpened():
    telemetry_socket.close()
    raise RuntimeError("Cannot open color webcam")

cap.set(
    cv2.CAP_PROP_FOURCC,
    cv2.VideoWriter_fourcc(*"MJPG")
)
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
cap.set(cv2.CAP_PROP_FPS, 30)

print("veriSON | Live Camera Telemetry")
print(f"Publishing to {TELEMETRY_HOST}:{TELEMETRY_PORT}")
print("CSV logging enabled")
print("Press ESC to stop")

start = time.monotonic()
previous_capture = None

frame_id = 0
total_bytes = 0
total_packets = 0
published_messages = 0

# Rolling one-second traffic window
window = deque()
window_bytes = 0

try:
    with open(CSV_PATH, "w", newline="") as f:
        writer = csv.writer(f)

        writer.writerow([
            "frame_id",
            "timestamp_ms",
            "frame_interval_ms",
            "encoded_bytes",
            "estimated_packets",
            "total_encoded_bytes",
            "total_estimated_packets",
            "rolling_bitrate_mbps",
            "average_bitrate_mbps",
            "effective_fps",
            "detections"
        ])

        while True:
            ret, frame = cap.read()

            if not ret:
                print("Frame capture failed")
                break

            now = time.monotonic()

            timestamp_ms = (
                now - start
            ) * 1000

            interval_ms = (
                (now - previous_capture) * 1000
                if previous_capture is not None
                else 0.0
            )

            previous_capture = now

            # Measure actual JPEG frame size.
            success, encoded = cv2.imencode(
                ".jpg",
                frame,
                [
                    cv2.IMWRITE_JPEG_QUALITY,
                    JPEG_QUALITY
                ]
            )

            if not success:
                continue

            frame_bytes = len(encoded)

            # Estimated application packet count.
            packets = math.ceil(
                frame_bytes / PACKET_PAYLOAD_BYTES
            )

            total_bytes += frame_bytes
            total_packets += packets

            # ---------------------------------
            # LIVE TELEMETRY PUBLISHER
            # ---------------------------------
            # Message format:
            # frame_id,timestamp_ms,encoded_bytes
            #
            # The C++ bridge will parse this
            # message and generate simulated
            # camera traffic inside ns-3.

            message = (
                f"{frame_id},"
                f"{timestamp_ms:.2f},"
                f"{frame_bytes}"
            )

            telemetry_socket.sendto(
                message.encode("utf-8"),
                telemetry_destination
            )

            published_messages += 1

            # ---------------------------------
            # TRAFFIC MEASUREMENTS
            # ---------------------------------

            window.append((now, frame_bytes))
            window_bytes += frame_bytes

            while (
                window
                and now - window[0][0] > 1.0
            ):
                _, old_bytes = window.popleft()
                window_bytes -= old_bytes

            if len(window) >= 2:
                window_duration = (
                    now - window[0][0]
                )

                rolling_mbps = (
                    window_bytes * 8
                    / max(window_duration, 0.001)
                    / 1_000_000
                )
            else:
                rolling_mbps = 0.0

            total_duration = now - start

            avg_mbps = (
                total_bytes * 8
                / total_duration
                / 1_000_000
                if total_duration > 0
                else 0.0
            )

            fps = (
                1000 / interval_ms
                if interval_ms > 0
                else 0.0
            )

            # ---------------------------------
            # YOLO OBJECT DETECTION
            # ---------------------------------

            results = model.predict(
                source=frame,
                conf=CONFIDENCE,
                verbose=False
            )

            detections = len(
                results[0].boxes
            )

            annotated = results[0].plot()

            # ---------------------------------
            # CSV LOGGING
            # ---------------------------------

            writer.writerow([
                frame_id,
                round(timestamp_ms, 2),
                round(interval_ms, 2),
                frame_bytes,
                packets,
                total_bytes,
                total_packets,
                round(rolling_mbps, 3),
                round(avg_mbps, 3),
                round(fps, 2),
                detections
            ])

            if frame_id % 30 == 0:
                f.flush()

            # ---------------------------------
            # CAMERA DISPLAY
            # ---------------------------------

            cv2.putText(
                annotated,
                f"Demand: {rolling_mbps:.2f} Mbps",
                (20, 40),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.8,
                (0, 255, 0),
                2
            )

            cv2.putText(
                annotated,
                f"FPS: {fps:.1f} | Packets: {packets}",
                (20, 75),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.7,
                (0, 255, 0),
                2
            )

            cv2.putText(
                annotated,
                f"LIVE TELEMETRY: UDP {TELEMETRY_PORT}",
                (20, 110),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.6,
                (0, 255, 255),
                2
            )

            cv2.imshow(
                "veriSON | Live Camera Telemetry",
                cv2.flip(annotated, 1)
            )

            frame_id += 1

            if cv2.waitKey(1) & 0xFF == 27:
                break

finally:
    cap.release()
    telemetry_socket.close()
    cv2.destroyAllWindows()

    print("\nveriSON Camera Session")
    print(f"Frames: {frame_id}")
    print(f"Encoded bytes: {total_bytes}")
    print(f"Estimated packets: {total_packets}")
    print(f"UDP telemetry messages sent: {published_messages}")
    print(f"Telemetry saved to: {CSV_PATH}")
