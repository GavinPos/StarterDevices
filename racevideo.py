import cv2
import gi
import numpy as np
import datetime
import time
import os
import subprocess
import socket
import queue
import threading

gi.require_version('Gst', '1.0')
from gi.repository import Gst

# Initialize GStreamer
Gst.init(None)

# ── External‐command listener ─────────────────────────────────────────────────

# Queue to hold externally-sent single-char commands
cmd_queue = queue.Queue()

def external_command_listener(host='127.0.0.1', port=6000):
    """Listen on a TCP socket and enqueue each received character."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    while True:
        conn, _ = srv.accept()
        with conn:
            data = conn.recv(1024).decode('utf-8', errors='ignore')
            for ch in data:
                cmd_queue.put(ch)

# Start the listener in the background
threading.Thread(target=external_command_listener, daemon=True).start()
os.system('cls' if os.name == 'nt' else 'clear')
print("🔌 Listening for external commands on tcp://127.0.0.1:6000 …")

# GStreamer pipeline using CPU H.264 decoder
PIPELINE_DESC = (
    "udpsrc port=5000 caps=\"application/x-rtp, media=video, encoding-name=H264, payload=96\" ! "
    "rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! video/x-raw,format=BGR ! "
    "appsink name=sink emit-signals=true sync=false max-buffers=1 drop=true"
)
pipeline = Gst.parse_launch(PIPELINE_DESC)
appsink = pipeline.get_by_name("sink")
pipeline.set_state(Gst.State.PLAYING)

cv2.namedWindow("Live Stream", cv2.WINDOW_NORMAL)

# Set initial size to 1280×720
cv2.resizeWindow("Live Stream", 1024, 576)

# Move the window’s top-left corner to (100,100) on your screen
cv2.moveWindow("Live Stream", 0, 10)

cv2.setWindowProperty("Live Stream", cv2.WND_PROP_TOPMOST, 1)

recording = False
writer = None
fourcc = cv2.VideoWriter_fourcc(*'mp4v')
fps = 100
recorded_files = []

def focus_window(win_name):
    try:
        # find the first window whose title contains win_name
        wids = subprocess.check_output(
            ["xdotool", "search", "--name", win_name]
        ).split()
        if wids:
            subprocess.run(
                ["xdotool", "windowactivate", "--sync", wids[0]]
            )
    except subprocess.CalledProcessError:
        print(f"⚠️ Window “{win_name}” not found to focus.")
    except Exception as e:
        print("⚠️ Error focusing window:", e)


def get_session_filename():
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"recording_{timestamp}.mp4"
    recorded_files.append(filename)
    return filename

race_start_time = None
race_end_time = None

print("📺 Live preview started.")
print("Press 'S' to start timer, 'F' to freeze timer, SPACE to record, ESC to exit.")

try:
    while True:
        sample = appsink.emit("try-pull-sample", Gst.SECOND // 10)
        if not sample:
            print("⚠️ No sample received")
            continue

        buf = sample.get_buffer()
        caps = sample.get_caps()
        structure = caps.get_structure(0)
        width = structure.get_value('width')
        height = structure.get_value('height')

        success, map_info = buf.map(Gst.MapFlags.READ)
        if not success:
            continue

        frame_data = map_info.data
        frame_array = np.frombuffer(frame_data, dtype=np.uint8)
        frame = frame_array.reshape((height, width, 3))  # BGR format guaranteed
        buf.unmap(map_info)

        # 🏁 Draw semi-transparent finish line
        finish_line_x = int(width * 0.5)
        overlay = frame.copy()
        cv2.line(overlay, (finish_line_x, 0), (finish_line_x, height), (255, 255, 255), 2)
        frame = cv2.addWeighted(overlay, 0.35, frame, 0.65, 0)

        # ⏱ Add timer overlay
        if race_start_time is not None:
            elapsed = race_end_time - race_start_time if race_end_time else time.time() - race_start_time
            timestamp_text = f"{elapsed:.2f} sec"
        else:
            timestamp_text = "Waiting for start..."

        cv2.putText(frame, timestamp_text, (10, height - 10),
                    cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2, cv2.LINE_AA)

        cv2.imshow("Live Stream", frame)

        if recording and writer:
            writer.write(frame)

        key = cv2.waitKey(1) & 0xFF

        # Drain any external commands and treat them like keypresses
        while not cmd_queue.empty():
            ext = cmd_queue.get().lower()
            if ext == 's':
                key = ord('s')
                focus_window("Live Stream")     # bring it forward & focus
            elif ext == 'f':
                key = ord('f')
            elif ext == ' ':
                key = 32
            elif ext == '\x1b':  # ESC
                key = 27

        if key == 27:  # ESC
            print("👋 Exiting.")
            break

        elif key == 32:  # SPACE to toggle recording
            if not recording:
                filename = get_session_filename()
                writer = cv2.VideoWriter(filename, fourcc, fps, (width, height))
                if not writer.isOpened():
                    print(f"❌ Failed to open VideoWriter for {filename}")
                    writer = None
                else:
                    print(f"🟢 Started recording: {filename}")
                    recording = True
            else:
                print("🛑 Stopped recording.")
                writer.release()
                writer = None
                recording = False
            cv2.waitKey(200)

        elif key in [ord('s'), ord('S')]:
            race_start_time = time.time()
            race_end_time = None
            print("🏁 Race start time marked!")

        elif key in [ord('f'), ord('F')]:
            if race_start_time is not None and race_end_time is None:
                race_end_time = time.time()
                print("⏱️ Timer finished!")

finally:
    if writer:
        writer.release()
    pipeline.set_state(Gst.State.NULL)
    cv2.destroyAllWindows()

    # 🔗 Concatenate and play all recorded videos (if more than one)
    if recorded_files:
        print(f"Concatenating videos...")
        concat_list = "concat_list.txt"
        with open(concat_list, "w") as f:
            for filename in recorded_files:
                f.write(f"file '{os.path.abspath(filename)}'\n")

        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        concat_output = f"session_{timestamp}.mp4"
        ffmpeg_cmd = [
            "ffmpeg", "-y", "-f", "concat", "-safe", "0", "-i", concat_list,
            "-c:v", "libx264", "-preset", "fast", "-crf", "23", concat_output
        ]
        print(f"🛠️ Concatenating recorded videos into {concat_output}...")
        subprocess.run(ffmpeg_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        print(f"🎬 Playing concatenated video: {concat_output}")
        subprocess.Popen(["celluloid", concat_output])
