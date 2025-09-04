import csv
import os
import time
import serial
import serial.tools.list_ports
import socket

CSV_PATH = "data/athletes.csv"
CSV_PATH2 = "data/athletes2.csv"   # kept for compatibility; not required
SESSION_FILE = "data/sessions.csv"
SERIAL_PORT = None
BAUD_RATE = 115200
SERIAL_TIMEOUT = 1
WRITE_TIMEOUT  = 1       # max seconds to block on write

# Global state
devices = []                 # ['00','01',...]
start_points = {}
athletes = {}
ser = None

# Volume management
default_volume = None        # None or int 0..30
device_volumes = {}          # {'03': 18, '07': 25, ...}

# ───────────────────────── Serial helpers ─────────────────────────

def detect_serial_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if "Arduino" in p.description or "ttyUSB" in p.device or "ttyACM" in p.device:
            return p.device
    raise IOError("❌ Could not auto-detect Arduino serial port. Is it connected?")

def clear_screen():
    os.system('cls' if os.name == 'nt' else 'clear')

def init_serial():
    global ser, SERIAL_PORT
    if ser is None or not ser.is_open:
        if SERIAL_PORT is None:
            SERIAL_PORT = detect_serial_port()
        ser = serial.Serial(
            SERIAL_PORT,
            BAUD_RATE,
            timeout=SERIAL_TIMEOUT,
            write_timeout=WRITE_TIMEOUT
        )
        if ser and ser.is_open:
            ser.reset_input_buffer()
            ser.reset_output_buffer()
        else:
            init_serial()
        time.sleep(2)   # give Arduino time after reset

def _send_line(line: str):
    """Small helper: write a single command line and flush buffers."""
    init_serial()
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    ser.write((line.strip() + "\n").encode("utf-8"))

# ───────────────────────── Device actions ─────────────────────────

def discover_devices(timeout=2):
    """
    Ask transmitter to DISCOVER and collect 'CHECK DEVxx ACKed' lines.
    Returns sorted list of device IDs ['00','03',...].
    """
    _send_line("DISCOVER")
    discovered = set()
    end_time = time.time() + timeout

    while time.time() < end_time:
        if ser.in_waiting > 0:
            raw = ser.readline()
            try:
                line = raw.decode('utf-8', errors='ignore').strip()
            except:
                continue
            if line.startswith("CHECK "):
                parts = line.split()
                if len(parts) >= 3 and parts[2].upper() == "ACKED":
                    addr = parts[1]
                    dev_id = addr[-2:]
                    discovered.add(dev_id)

    if not discovered:
        print("⚠️  No devices responded.")
    else:
        for dev_id in sorted(discovered, key=lambda x: int(x)):
            print(f"✅ Found device: {dev_id}")
    return sorted(discovered, key=lambda x: int(x)) if discovered else []

def flash_all_devices(timeout=2):
    """
    Send FLASH and parse 'FLASH DEVxx OK/FAIL' lines for a short window.
    """
    _send_line("FLASH")
    ok_devices = []
    end_time = time.time() + timeout

    while time.time() < end_time:
        if ser.in_waiting > 0:
            raw = ser.readline()
            try:
                line = raw.decode('utf-8', errors='ignore').strip()
            except:
                continue
            if line.startswith("FLASH "):
                parts = line.split()
                if len(parts) >= 3:
                    addr, status = parts[1], parts[2].upper()
                    dev_id = addr[-2:]
                    if status == "OK":
                        ok_devices.append(dev_id)

    if not ok_devices:
        print("⚠️ No devices flashed successfully.")
    else:
        for d in sorted(ok_devices, key=lambda x: int(x)):
            print(f"✅ FLASH OK on device: {d}")
    return ok_devices

# Backwards-compat aliases used elsewhere in your code
def send_all_reset_and_listen():
    found = discover_devices(timeout=2)
    input("Press ENTER to continue…")
    return found

def test_all_devices(timeout=2):
    ok = flash_all_devices(timeout=timeout)
    input("Press ENTER to continue…")
    return ok

# ───────────────────────── Volume helpers ─────────────────────────

def _clamp_volume(v):
    try:
        v = int(v)
    except:
        return None
    if v < 0: v = 0
    if v > 30: v = 30
    return v

def set_default_volume_interactive():
    global default_volume
    while True:
        v = input("Enter default volume (0..30), blank to cancel: ").strip()
        if v == "":
            return
        nv = _clamp_volume(v)
        if nv is None:
            print("❌ Invalid number.")
            continue
        default_volume = nv
        print(f"✅ Default volume set to {default_volume} (will be sent in START blocks).")
        choice = input("Send 'VOLUME' to transmitter now too? (y/n): ").strip().lower()
        if choice == 'y':
            _send_line(f"VOLUME:{default_volume}")
            t0 = time.time()
            while time.time() - t0 < 0.5 and ser.in_waiting > 0:
                msg = ser.readline().decode("utf-8", errors="ignore").strip()
                if msg:
                    print(f"📡 {msg}")
        return

def set_per_device_volumes():
    global device_volumes
    if not devices:
        print("⚠️ No devices in list. Discover first.")
        input("Press ENTER to continue…")
        return
    print("Enter per-device volumes (0..30). Leave blank to keep existing. Type 'x' to clear a device override.")
    for dev in sorted(devices, key=lambda x: int(x)):
        curr = device_volumes.get(dev)
        prompt = f"  DEV{dev} volume [{'' if curr is None else curr}]: "
        v = input(prompt).strip().lower()
        if v == "":
            continue
        if v == "x":
            if dev in device_volumes:
                del device_volumes[dev]
                print(f"  ↺ Cleared override for DEV{dev}")
            continue
        nv = _clamp_volume(v)
        if nv is None:
            print("   ❌ Invalid number, ignored.")
        else:
            device_volumes[dev] = nv
            print(f"   ✅ DEV{dev} → {nv}")
    input("Press ENTER to continue…")

def list_volumes():
    print("\n🔊 Volume settings")
    print(f"  Default: {default_volume if default_volume is not None else '(none)'}")
    print("  Per-device overrides:")
    if device_volumes:
        for dev in sorted(device_volumes, key=lambda x: int(x)):
            print(f"   • DEV{dev}: {device_volumes[dev]}")
    else:
        print("   (none)")
    input("\nPress ENTER to continue…")

# ───────────────────────── Athletes load/save/search ─────────────────────────

def load_athletes(file_path=CSV_PATH):
    """
    Load athletes from CSV. Expects columns:
      ID, Name, <distance1>, <distance2>, ...
    Returns a dict:
      { athlete_id: { "name": str, "pbs": { distance: float, ... } } }
    """
    athletes = {}
    if not os.path.exists(file_path):
        return athletes
    with open(file_path, newline='') as csvfile:
        reader = csv.DictReader(csvfile)
        for row in reader:
            aid = row.get('ID', '').strip().upper()
            if not aid:
                continue
            name = row.get('Name', '').strip()
            pbs = {}
            for col, val in row.items():
                if col in ('ID', 'Name'):
                    continue
                if val is None:
                    continue
                sval = str(val).strip()
                if sval:
                    try:
                        pbs[col] = float(sval)
                    except ValueError:
                        pass
            athletes[aid] = {"name": name or aid, "pbs": pbs}
    return athletes

def write_athletes_csv(athletes_dict, file_path=CSV_PATH):
    """Rewrite athletes.csv with a unified set of distance columns."""
    distances = sorted(
        {d for a in athletes_dict.values() for d in a["pbs"].keys()},
        key=lambda x: float(x) if x.replace('.', '', 1).isdigit() else x
    )
    os.makedirs(os.path.dirname(file_path), exist_ok=True)
    with open(file_path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['ID', 'Name'] + distances)
        for aid in sorted(athletes_dict.keys()):
            ath = athletes_dict[aid]
            row = [aid, ath['name']]
            for dist in distances:
                val = ath['pbs'].get(dist)
                row.append(f"{val:.2f}" if isinstance(val, (int, float)) else '')
            w.writerow(row)

def search_athletes_by_name(query, limit=20):
    """Return [(aid, name)] whose name contains query (case-insensitive)."""
    q = query.strip().lower()
    out = []
    for aid, info in athletes.items():
        if q in info["name"].lower():
            out.append((aid, info["name"]))
    out.sort(key=lambda t: t[1].lower())
    return out[:limit]

def add_new_athlete_interactive():
    """
    Add a new athlete. PB input:
      • number like 12.34 → PB in seconds
      • '?'  → set THIS distance to 999
      • '??' → set THIS and ALL REMAINING distances to 999
      • ''   → leave blank
    """
    global athletes

    clear_screen()
    print("=== Add New Athlete ===\n")

    # ID
    while True:
        aid = input("New Athlete ID: ").strip().upper()
        if not aid:
            print("❌ ID cannot be empty.")
            continue
        if aid in athletes:
            print("❌ ID already exists.")
            continue
        break

    # Name
    name = input("Name: ").strip()
    if not name:
        name = aid

    # Distance columns from existing CSV (or fallback)
    distances = sorted(
        {d for a in athletes.values() for d in a["pbs"].keys()},
        key=lambda x: float(x) if x.replace('.', '', 1).isdigit() else x
    )
    if not distances:
        distances = ["60", "100", "200", "300", "400", "800", "1500"]

    print("\nEnter PB (seconds) for each distance.")
    print("  • Number like 12.34")
    print("  • '?'  → set THIS distance to 999")
    print("  • '??' → set THIS and ALL REMAINING to 999")
    print("  • ENTER to skip\n")

    pbs = {}
    fill_rest_999 = False
    for dist in distances:
        if fill_rest_999:
            pbs[dist] = 999.0
            continue

        while True:
            val = input(f"  PB for {dist}m: ").strip()
            if val == "":
                break
            if val == "?":
                pbs[dist] = 999.0
                break
            if val == "??":
                pbs[dist] = 999.0
                fill_rest_999 = True
                break
            try:
                pbs[dist] = float(val)
                break
            except ValueError:
                print("   ❌ Enter a number, '?', '??', or blank to skip.")

    athletes[aid] = {"name": name, "pbs": pbs}
    write_athletes_csv(athletes, CSV_PATH)

    print(f"\n✅ Added athlete {aid} — {name}")
    input("Press ENTER to continue…")

# ───────────────────────── Race building & helpers ─────────────────────────

def resolve_athlete_input(text, distance_hint=None):
    """
    Accept an input that can be:
      • exact athlete ID
      • partial name fragment (auto-search)
      • '?' then prompt query
      • '+' to add a new athlete
    Returns athlete ID or '' if no selection.
    """
    t = text.strip()
    if t == "":
        return ""
    if t == "+":
        add_new_athlete_interactive()
        return ""
    if t == "?":
        q = input("Enter part of the name to search: ").strip()
        matches = search_athletes_by_name(q)
        return pick_athlete_from_list(matches, distance_hint)
    # exact ID
    up = t.upper()
    if up in athletes:
        return up
    # treat as name fragment
    matches = search_athletes_by_name(t)
    return pick_athlete_from_list(matches, distance_hint)

def pick_athlete_from_list(matches, distance_hint=None):
    if not matches:
        print("   ⚠️  No matches.")
        time.sleep(0.7)
        return ""
    if len(matches) == 1:
        print(f"   → {matches[0][0]} — {matches[0][1]}")
        time.sleep(0.4)
        return matches[0][0]
    print("\nMatches:")
    print(f"{'#':<3}{'ID':<10}{'Name':<24}{'PB@dist':<10}")
    print("-" * 50)
    for i, (aid, name) in enumerate(matches, 1):
        pb = "-"
        if distance_hint and distance_hint in athletes[aid]["pbs"]:
            pb = f"{athletes[aid]['pbs'][distance_hint]:.2f}"
        print(f"{i:<3}{aid:<10}{name:<24}{pb:<10}")
    sel = input("Pick number (ENTER to cancel): ").strip()
    if sel.isdigit():
        idx = int(sel)
        if 1 <= idx <= len(matches):
            return matches[idx-1][0]
    return ""

def get_race_participants(athletes_dict, start_points_dict):
    """
    Enhanced: supports entering '?', '+', or name fragments when assigning lanes.
    """
    racers = {}
    for dist, sp in start_points_dict.items():
        print(f"\nSetting up race for {dist}m:")
        if sp["has_lanes"]:
            for lane in range(1, sp["num_lanes"] + 1):
                while True:
                    aid_in = input(f"  Lane {lane} Athlete (ID, name, '?' to search, '+' to add, ENTER to skip): ").strip()
                    if aid_in == "":
                        break
                    aid = resolve_athlete_input(aid_in, distance_hint=dist)
                    if aid == "":
                        continue
                    if aid not in athletes_dict:
                        print("❌ ID not found.")
                        continue
                    if aid in racers:
                        print("⚠️ Already added.")
                        continue
                    racers[aid] = {
                        "name": athletes_dict[aid]["name"],
                        "pbs": athletes_dict[aid]["pbs"].copy(),
                        "start_point": dist
                    }
                    sp["assignments"][f"Lane {lane}"] = aid
                    break
        else:
            print("  Enter athletes for this start point (press ENTER to finish).")
            while True:
                aid_in = input("  Athlete (ID, name, '?' to search, '+' to add): ").strip()
                if aid_in == "":
                    break
                aid = resolve_athlete_input(aid_in, distance_hint=dist)
                if aid == "":
                    continue
                if aid not in athletes_dict:
                    print("❌ ID not found.")
                    continue
                if aid in racers:
                    print("⚠️ Already added.")
                    continue
                racers[aid] = {
                    "name": athletes_dict[aid]["name"],
                    "pbs": athletes_dict[aid]["pbs"].copy(),
                    "start_point": dist
                }
                sp["assignments"][aid] = aid
    return racers

def calculate_staggered_starts(racers, distance):
    """
    Compute 'start' offset for each racer based on their PB at given distance.
    Also sets racer['pb'] = event PB for easy lookup later.
    """
    valid_pbs = [r['pbs'].get(distance) for r in racers.values() if r['pbs'].get(distance) is not None]
    slowest_pb = max(valid_pbs) if valid_pbs else 0.0

    for aid, r in racers.items():
        pb_val = r['pbs'].get(distance)
        r['pb'] = pb_val if pb_val is not None else 0.0
        r['start'] = round(slowest_pb - pb_val, 3) if pb_val is not None else 0.0
    return racers

def define_start_points():
    valid = ["60", "100", "200", "300", "400", "800", "1500"]
    opts = ", ".join(valid)

    while True:
        clear_screen()
        print("=== Define Start Points ===\n")
        print(f"Valid distances: {opts}")

        if start_points:
            print("Already defined:")
            for d, sp in sorted(start_points.items(), key=lambda x: int(x[0])):
                if sp["has_lanes"]:
                    print(f"  • {d} m  → {sp['num_lanes']} lane(s)")
                else:
                    print(f"  • {d} m  → no lanes")
            print()

        dist = input("Enter distance, 'c' to clear, or press ENTER to finish: ").strip().lower()
        if dist == 'c':
            confirm = input("❗ This will remove all start points. Are you sure? (y/n): ").strip().lower()
            if confirm == 'y':
                start_points.clear()
            continue
        if dist == "":
            break
        if dist not in valid:
            print(f"\n❌ Invalid distance. Choose from: {opts}")
            input("\nPress ENTER to try again…")
            continue
        if dist in start_points:
            print(f"\n❌ You’ve already defined {dist} m.")
            input("\nPress ENTER to try again…")
            continue

        resp = input("Defined lanes? (y/n): ").strip().lower()
        has_lanes = (resp == "y")
        num_lanes = 0
        if has_lanes:
            while True:
                nl = input("Number of lanes: ").strip()
                if nl.isdigit() and int(nl) > 0:
                    num_lanes = int(nl)
                    break
                print("❌ Enter a positive integer for lanes.")

        start_points[dist] = {
            "has_lanes": has_lanes,
            "num_lanes": num_lanes,
            "devices": [],
            "assignments": {},
            "device_assignments": {}
        }

def add_virtual_devices():
    """
    Prompt for single IDs or ranges (NN or NN-NN), add to devices.
    """
    global devices
    clear_screen()
    print("=== Add Virtual Devices ===")
    print("Enter device IDs or ranges separated by spaces or commas (e.g. 03, 05-08). Press ENTER to finish.")
    while True:
        inp = input("Device IDs: ").strip()
        if not inp:
            break
        tokens = inp.replace(',', ' ').split()
        for tok in tokens:
            if '-' in tok:
                parts = tok.split('-', 1)
                if len(parts) == 2 and parts[0].isdigit() and parts[1].isdigit():
                    start, end = int(parts[0]), int(parts[1])
                    if start > end:
                        start, end = end, start
                    for i in range(start, end + 1):
                        dev = str(i).zfill(2)
                        if dev not in devices:
                            devices.append(dev)
                            print(f"✅ Added virtual device: {dev}")
                        else:
                            print(f"⚠️  Skipping existing device: {dev}")
                else:
                    print(f"❌ Invalid range '{tok}'. Use NN-NN format.")
            elif tok.isdigit():
                dev = tok.zfill(2)
                if dev not in devices:
                    devices.append(dev)
                    print(f"✅ Added virtual device: {dev}")
                else:
                    print(f"⚠️  Skipping existing device: {dev}")
            else:
                print(f"❌ Invalid token '{tok}'.")
        print()
    return devices

def collect_start_point_timings(start_points_dict, athletes_dict):
    data = {}
    for dist, sp in start_points_dict.items():
        rows = []
        if sp["has_lanes"]:
            entries = [(lane, aid) for lane, aid in sp["assignments"].items() if aid in athletes_dict]
            pbs = [athletes_dict[aid]["pbs"].get(dist) for _, aid in entries if dist in athletes_dict[aid]["pbs"]]
            slowest = max(pbs) if pbs else 0.0
        else:
            entries = [(None, aid) for aid in sp["assignments"].values() if aid in athletes_dict]
            slowest = None
        for lane, aid in entries:
            pb_val = athletes_dict[aid]["pbs"].get(dist)
            if pb_val is None:
                continue
            headstart = round(slowest - pb_val, 2) if sp["has_lanes"] else 0.0
            rows.append({
                "lane": lane if sp["has_lanes"] else "-",
                "id": aid,
                "name": athletes_dict[aid]["name"],
                "pb": pb_val,
                "start": headstart
            })
        data[dist] = rows
    return data

def calculate_timings(racers):
    """
    Loop through all defined start points, display per-distance PB & headstart tables,
    and inject each racer's start, device, and pb into racers.
    """
    clear_screen()
    print("=== Start Point Timings ===")

    timing_data = collect_start_point_timings(start_points, athletes)

    for dist, rows in timing_data.items():
        print(f"--- {dist}m ---")
        if not rows:
            print("No PBs found for this distance.")
        else:
            print(f"{'Lane':<7}{'Name':<20}{'ID':<8}{'PB(s)':<8}{'Start(s)':<10}")
            print("-" * 53)
            for row in rows:
                lane = row['lane'] or '-'
                print(f"{lane:<7}{row['name']:<20}{row['id']:<8}"
                      f"{row['pb']:<8.2f}{row['start']:<10.2f}")
    input("Press ENTER to return...")

    # inject into racers
    for dist, rows in timing_data.items():
        sp = start_points[dist]
        if sp['has_lanes']:
            for row in rows:
                aid = row['id']
                if aid in racers:
                    racers[aid]['start'] = row['start']
                    racers[aid]['device'] = sp['device_assignments'].get(row['lane'], '-')
                    racers[aid]['pb'] = row['pb']
        else:
            default_dev = sp['devices'][0] if sp['devices'] else '-'
            for row in rows:
                aid = row['id']
                if aid in racers:
                    racers[aid]['start'] = 0.0
                    racers[aid]['device'] = default_dev
                    racers[aid]['pb'] = row['pb']

    return timing_data

def compute_and_apply_timings(racers):
    """
    Compute timing rows (PB + headstarts) and inject into `racers`
    without any UI. Mirrors the injection logic from calculate_timings().
    """
    timing_data = collect_start_point_timings(start_points, athletes)

    for dist, rows in timing_data.items():
        sp = start_points[dist]
        if sp.get('has_lanes'):
            for row in rows:
                aid = row['id']
                if aid in racers:
                    racers[aid]['start']  = row['start']
                    racers[aid]['pb']     = row['pb']
                    racers[aid]['device'] = sp['device_assignments'].get(row['lane'], '-')
        else:
            # SCRATCH: racers have a shared start; devices handled in schedule builder
            for row in rows:
                aid = row['id']
                if aid in racers:
                    racers[aid]['start']  = 0.0
                    racers[aid]['pb']     = row['pb']
                    racers[aid]['device'] = '-'  # not used for scratch; schedule adds all devices

    return timing_data

# ───────────────────────── Overrides & lane patterns ─────────────────────────

def override_start_delays(racers):
    """
    Allow the user to override 'start' values for *laned* events only.
    For scratch events (no lanes), editing is disabled here by design.
    """
    from collections import defaultdict
    if not racers:
        print("ℹ️  No racers loaded. Setup race first.")
        input("Press ENTER to continue…")
        return

    while True:
        clear_screen()

        grouped_laned = defaultdict(list)    # dist -> list of (lane_key, aid)
        grouped_scratch = defaultdict(list)  # dist -> list of (dash, aid) for display only

        for dist, sp in start_points.items():
            if not sp.get("assignments"):
                continue
            if sp.get("has_lanes"):
                for ln in range(1, sp["num_lanes"] + 1):
                    lane_key = f"Lane {ln}"
                    aid = sp["assignments"].get(lane_key)
                    if aid and aid in racers and racers[aid].get("start_point") == dist:
                        grouped_laned[dist].append((lane_key, aid))
            else:
                for aid in sp["assignments"].values():
                    if aid and aid in racers and racers[aid].get("start_point") == dist:
                        grouped_scratch[dist].append(("-", aid))

        index = []
        print("✏️  Override Start Delays (laned events only)\n")
        print("  • Pick athlete by NUMBER or ID to set an absolute start (seconds)")
        print("  • Or type:  all <±delta>        (e.g. all +0.20)")
        print("  • Or type:  dist <D> <±delta>   (e.g. dist 100 +0.10)")
        print("  • ENTER to finish\n")

        # Show laned groups (editable)
        row_no = 1
        for dist in sorted(grouped_laned, key=lambda x: float(x) if x.replace('.','',1).isdigit() else x):
            print(f"=== {dist}m (laned) ===")
            print(f"{'#':<3}{'Lane':<8}{'Athlete ID':<12}{'Name':<20}{'PB(s)':<8}{'Start(s)':<10}")
            print("-" * 61)
            for lane_key, aid in grouped_laned[dist]:
                r = racers[aid]
                pb = r.get('pb', 0.0)
                st = r.get('start', 0.0)
                print(f"{row_no:<3}{lane_key:<8}{aid:<12}{r['name']:<20}{pb:<8.2f}{st:<10.2f}")
                index.append((row_no, dist, lane_key, aid))
                row_no += 1
            print()

        # Show scratch groups (read-only)
        for dist in sorted(grouped_scratch, key=lambda x: float(x) if x.replace('.','',1).isdigit() else x):
            print(f"=== {dist}m (scratch — editing disabled in Option 4) ===")
            print(f"{'':<3}{'Lane':<8}{'Athlete ID':<12}{'Name':<20}{'PB(s)':<8}{'Start(s)':<10}")
            print("-" * 61)
            for lane_key, aid in grouped_scratch[dist]:
                r = racers[aid]
                pb = r.get('pb', 0.0)
                st = r.get('start', 0.0)
                print(f"{'':<3}{lane_key:<8}{aid:<12}{r['name']:<20}{pb:<8.2f}{st:<10.2f}")
            print("  Tip: Set a global scratch offset during '3. Setup Race' flow.")
            print()

        sel = input("Select #/ID (or 'all +/-x.xx' / 'dist D +/-x.xx', ENTER to finish): ").strip()
        if sel == "":
            break

        parts = sel.split()
        if len(parts) == 2 and parts[0].lower() == "all":
            try:
                delta = float(parts[1])
            except ValueError:
                print("❌ Could not parse delta.")
                time.sleep(0.8)
                continue
            for _, _, _, aid in index:
                racers[aid]['start'] = float(racers[aid].get('start', 0.0)) + delta
            print(f"✅ Applied {delta:+.2f}s to ALL laned athletes.")
            time.sleep(0.8)
            continue

        if len(parts) == 3 and parts[0].lower() == "dist":
            dist_key = parts[1]
            try:
                delta = float(parts[2])
            except ValueError:
                print("❌ Could not parse delta.")
                time.sleep(0.8)
                continue
            if dist_key in grouped_scratch:
                print("⚠️  That is a scratch event; editing is disabled here.")
                time.sleep(0.9)
                continue
            applied = 0
            for _, d, _, aid in index:
                if d == dist_key:
                    racers[aid]['start'] = float(racers[aid].get('start', 0.0)) + delta
                    applied += 1
            if applied:
                print(f"✅ Applied {delta:+.2f}s to {applied} athlete(s) in {dist_key}m (laned).")
            else:
                print(f"⚠️  No laned athletes found for distance '{dist_key}'.")
            time.sleep(0.8)
            continue

        # individual selection (laned only)
        aid_to_edit = None
        if sel.isdigit():
            num = int(sel)
            match = next((t for t in index if t[0] == num), None)
            if match:
                aid_to_edit = match[3]
        else:
            cand = sel.upper()
            # Only allow if the athlete is part of a laned group
            if any(aid == cand for _, _, _, aid in index):
                aid_to_edit = cand

        if not aid_to_edit:
            print("❌ Not a valid (laned) selection.")
            time.sleep(0.8)
            continue

        current = racers[aid_to_edit].get('start', 0.0)
        val = input(f"New absolute start for {aid_to_edit} ({racers[aid_to_edit]['name']}) [current {current:.2f}s]: ").strip()
        if val == "":
            continue
        try:
            new_start = float(val)
        except ValueError:
            print("❌ Please enter a number (e.g. 1.25 or -0.30).")
            time.sleep(0.8)
            continue
        racers[aid_to_edit]['start'] = new_start
        print(f"✅ Updated {aid_to_edit} → {new_start:.2f}s")
        time.sleep(0.8)

def override_scratch_offset(distance, racers):
    """Set a single absolute start time (seconds) for a scratch distance."""
    sp = start_points.get(distance)
    if not sp or sp.get('has_lanes'):
        return  # only for scratch

    aids = [aid for aid, r in racers.items() if r.get('start_point') == distance]
    if not aids:
        return

    current_vals = sorted({ float(racers[a].get('start', 0.0)) for a in aids })
    cur_str = ", ".join(f"{v:.2f}" for v in current_vals)
    print(f"\nScratch {distance}m current start(s): {cur_str}")
    val = input(f"Set a GLOBAL absolute start for {distance}m (seconds, ENTER to keep): ").strip()
    if val == "":
        return
    try:
        new_start = float(val)
    except ValueError:
        print("❌ Please enter a number.")
        time.sleep(0.8)
        return
    for a in aids:
        racers[a]['start'] = new_start
    print(f"✅ Set all {distance}m scratch starts to {new_start:.2f}s")
    time.sleep(0.8)

def _lane_sequence_outside_in(num_lanes):
    # [1, N, 2, N-1, 3, N-2, ...]
    seq = []
    left, right = 1, num_lanes
    while left <= right:
        seq.append(left)
        if right != left:
            seq.append(right)
        left += 1
        right -= 1
    return seq

def _lane_sequence_left_to_right(num_lanes):
    # [1,2,3,...,N]
    return list(range(1, num_lanes + 1))

def apply_handicap_lane_pattern(distance, racers, pattern="outside_in"):
    """
    Reorders lane assignments for a laned start point based on computed 'start' values.
    pattern:
      • "outside_in"     → slowest to fastest mapped to [1, N, 2, N-1, ...]
      • "left_to_right"  → slowest..fastest mapped to [1, 2, 3, ..., N]
    Displays the new mapping.
    """
    if distance not in start_points:
        print(f"⚠️  Distance {distance} not defined in start points.")
        input("Press ENTER…")
        return
    sp = start_points[distance]
    if not sp.get("has_lanes"):
        print("⚠️  Patterning only applies to laned start points.")
        input("Press ENTER…")
        return

    # extract current participants tied to this distance
    current = []
    for ln in range(1, sp["num_lanes"] + 1):
        aid = sp["assignments"].get(f"Lane {ln}")
        if aid and aid in racers and racers[aid].get("start_point") == distance:
            current.append(aid)

    if not current:
        print("⚠️  No athletes assigned to lanes yet.")
        input("Press ENTER…")
        return

    # sort by start descending (largest headstart = slowest), tiebreak by PB desc
    sorted_aids = sorted(
        current,
        key=lambda a: (racers[a].get("start", 0.0), racers[a].get("pb", 0.0)),
        reverse=True
    )

    if pattern == "outside_in":
        lane_order = _lane_sequence_outside_in(sp["num_lanes"])
    else:
        lane_order = _lane_sequence_left_to_right(sp["num_lanes"])

    # map athletes to lane order; ignore extra lanes, or if more athletes than lanes, truncate
    sp["assignments"].clear()
    pairs = []
    for idx, aid in enumerate(sorted_aids):
        if idx >= len(lane_order):
            print("⚠️  More athletes than lanes. Extra athletes not assigned.")
            break
        lane = lane_order[idx]
        sp["assignments"][f"Lane {lane}"] = aid
        pairs.append((lane, aid))

    # Show the new mapping
    clear_screen()
    print(f"🏟️  Lane pattern for {distance}m → {pattern.replace('_',' ').title()}\n")
    print(f"{'Lane':<6}{'Athlete ID':<12}{'Name':<22}{'PB(s)':<10}{'Start(s)':<8}")
    print("-" * 60)
    for lane, aid in sorted(pairs, key=lambda x: x[0]):
        r = racers[aid]
        pb = r.get('pb', 0.0)
        st = r.get('start', 0.0)
        print(f"{lane:<6}{aid:<12}{r['name']:<22}{pb:<10.2f}{st:<8.2f}")
    input("\nPress ENTER to continue…")

# ───────────────────────── Command sequence & RF start ─────────────────────────

def show_command_sequence(racers):
    if not racers:
        print("❌ No race has been set up yet.")
        input("\nPress ENTER to continue...")
        return

    sched = build_device_schedule(racers)
    events = []
    for dev, t in sched.items():
        events += [
            (t['red_on'],    dev, '1'),
            (t['orange_on'], dev, '2'),
            (t['green_on'],  dev, '3'),
            (t['green_off'], dev, '0'),
        ]
    events.sort(key=lambda e: e[0])

    code_map = {'0': "-", '1': "Marks", '2': "Set", '3': "Go"}

    print("\n⏱️  Command Sequence:")
    print(f"{'Time(s)':<8}{'Device':<12}{'Action'}")
    print("-" * 28)
    for t, dev, cmd in events:
        action = code_map.get(cmd, cmd)
        print(f"{t:>7.2f}  {dev:<12} {action}")

def build_device_schedule(racers):
    """
    Build a per-device schedule. For laned starts, each lane's device follows
    that lane's athlete start. For scratch (no lanes), *all* devices at that
    start point fire with the same timings.
    """
    # Global reference so the earliest start happens at t=0
    if racers:
        min_start_all = min(float(r.get('start', 0.0)) for r in racers.values())
    else:
        min_start_all = 0.0

    RED_D, ORANGE_D, GREEN_D, OFF_D = 5.0, 7.0, 9.0, 11.0

    def _times_from_red_on(red_on):
        return {
            'red_on':     red_on,
            'red_off':    red_on + RED_D,
            'orange_on':  red_on + RED_D,
            'orange_off': red_on + ORANGE_D,
            'green_on':   red_on + GREEN_D,
            'green_off':  red_on + OFF_D
        }

    schedule = {}

    for dist, sp in start_points.items():
        if sp.get('has_lanes'):
            # Lane-based: one device per lane
            for ln in range(1, sp['num_lanes'] + 1):
                dev = sp['device_assignments'].get(f"Lane {ln}")
                if not dev:
                    continue
                aid = sp['assignments'].get(f"Lane {ln}")
                if aid and aid in racers:
                    red_on = float(racers[aid].get('start', 0.0)) - min_start_all
                else:
                    red_on = 0.0 - min_start_all
                schedule[dev] = _times_from_red_on(red_on)
        else:
            # SCRATCH: every device at this start point fires with identical timings
            dist_aids = [a for a, r in racers.items() if r.get('start_point') == dist]
            if dist_aids:
                dist_min = min(float(racers[a].get('start', 0.0)) for a in dist_aids)
            else:
                dist_min = 0.0
            red_on = dist_min - min_start_all
            for dev in sp.get('devices', []):
                schedule[dev] = _times_from_red_on(red_on)

    return schedule

def start_race_sequence(racers):
    """
    Uses the transmitter’s START:… model:
      • Compute each device’s [red, orange, green, off] offsets
      • Send one START:<ID>{r,o,g,f}[@vol];… command
      • Let the Arduino print “STARTTIMER” when it fires
    """
    # Ensure timings/device mapping exist (in case user skipped earlier steps)
    if any(('start' not in r) or ('device' not in r) or ('pb' not in r) for r in racers.values()):
        compute_and_apply_timings(racers)

    clear_screen()
    print("\n⏱️  Upcoming Command Sequence:")
    show_command_sequence(racers)
    if not racers:
        return
    input("\nPress ENTER to start the race…")

    sched = build_device_schedule(racers)
    entries = []
    for dev, times in sched.items():
        r = int(round(times['red_on']))
        o = int(round(times['orange_on']))
        g = int(round(times['green_on']))
        f = int(round(times['green_off']))
        vol = device_volumes.get(dev, default_volume)
        entry = f"{dev}{{{r},{o},{g},{f}}}"
        if isinstance(vol, int):
            entry += f"@{vol}"
        entries.append(entry)

    cmd_str = "START:" + ";".join(entries) + ";\n"

    init_serial()
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    ser.write(cmd_str.encode())
    print(f"📤 Sent to transmitter: {cmd_str.strip()}")

    print("⏳ Waiting for Arduino to fire the start…")
    while True:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if not line:
            continue
        print(f"📡 {line}")
        if line == "STARTTIMER":
            send_start_command()
            print("🚦 Race started!")
            break

def send_start_command(host="127.0.0.1", port=6000, payload=b"s"):
    print(f"📡 Connecting to {host}:{port}...")
    try:
        for attempt in range(1, 6):
            try:
                with socket.create_connection((host, port), timeout=0.2) as sock:
                    sock.sendall(payload)
                print(f"✅ Sent {payload!r} on attempt {attempt}")
                return
            except Exception as e:
                print(f"⚠️  Start-cmd attempt {attempt} failed: {e}")
                time.sleep(0.05)
        print("❌ Giving up on starting timer!")
    except Exception as e:
        print(f"⚠️  Unexpected error in send_start_command: {e}")

def show_device_schedule(racers):
    clear_screen()
    if not racers:
        print("❌ No race has been set up yet.")
        input("\nPress ENTER to continue...")
        return

    sched = build_device_schedule(racers)
    print("\n📋 Device Light Schedule (start times for each LED)\n")
    print(f"{'Device':<8}{'Distance':<10}{'Lane':<8}{'Red(s)':<10}{'Orange(s)':<12}{'Green(s)':<10}")
    print("-" * 58)
    for dev, times in sched.items():
        distance = '-'
        lane = '-'
        for dist, sp in start_points.items():
            if sp['has_lanes']:
                for ln, d in sp['device_assignments'].items():
                    if d == dev:
                        distance = f"{dist}m"
                        lane = ln
                        break
            else:
                if dev in sp['devices']:
                    distance = f"{dist}m"
                    lane = '-'
            if distance != '-' and (sp['has_lanes'] and lane != '-' or not sp['has_lanes']):
                break
        red_start = times['red_on']
        orange_start = times['orange_on']
        green_start = times['green_on']
        print(
            f"{dev:<8}{distance:<10}{lane:<8}"
            f"{red_start:<10.2f}{orange_start:<12.2f}{green_start:<10.2f}"
        )
    input("\nPress ENTER to continue...")

# ───────────────────────── Results & persistence ─────────────────────────

def enter_race_results(racers):
    """
    racers: dict mapping athlete ID → {
        'name': str,
        'pbs': dict,
        'start': float,
        'pb': float,
        'device': str,
        'start_point': str,
    }
    """
    from collections import defaultdict

    results = {}
    # 1) Collect finish times or DLQs
    while True:
        clear_screen()
        print("\nEnter race results. Recorded times shown in brackets; 'DLQ' for disqualified.\n")
        grouped = defaultdict(list)
        for aid, data in racers.items():
            grp = data.get('start_point', 'Unknown')
            grouped[grp].append((aid, data))

        index_map = {}
        idx = 1
        for grp in sorted(grouped):
            print(f"--- {grp}m ---")
            for aid, data in grouped[grp]:
                if aid in results:
                    val = results[aid]
                    label = " (DLQ)" if val == 'DLQ' else f" ({val:.2f}s)"
                else:
                    label = ""
                print(f"{idx}. {aid} — {data['name']}{label}")
                index_map[str(idx)] = aid
                idx += 1
            print()

        sel = input("Select athlete (number or ID), or press ENTER to finish: ").strip()
        if not sel:
            break
        aid = index_map.get(sel, sel.upper())
        if aid not in racers:
            print("❌ Invalid selection.")
            time.sleep(1)
            continue

        while True:
            prompt = f"Enter finish time for {racers[aid]['name']} (seconds) or 'd' for DLQ: "
            entry = input(prompt).strip()
            if entry.lower() == 'd':
                results[aid] = 'DLQ'
                break
            try:
                results[aid] = float(entry)
                break
            except ValueError:
                print("❌ Please enter a valid number of seconds or 'd'.")

    # 2) Build flat list
    flat = []
    for aid, data in racers.items():
        start = data.get('start', 0.0)
        finish = results.get(aid)
        if finish == 'DLQ' or finish is None:
            actual = None
            new_pb = ''
        else:
            actual = finish - start
            new_pb = 'YES' if actual < data.get('pb', float('inf')) else ''
        grp = data.get('start_point', 'Unknown')
        flat.append((grp, aid, data, start, finish, actual, new_pb))

    # 3) Regroup
    groups = defaultdict(list)
    for item in flat:
        groups[item[0]].append(item)

    # 4) Display final results
    clear_screen()
    print("\n📊 Final Results by Event Group:\n")
    header = (
        f"{'Athlete ID':<11}"
        f"{'Name':<20}"
        f"{'Prev PB(s)':<11}"
        f"{'Start(s)':<10}"
        f"{'Finish(s)':<11}"
        f"{'Actual(s)':<11}"
        f"{'New PB'}"
    )
    sep = "-" * len(header)

    def sort_key(item):
        return item[5] if item[5] is not None else float('inf')

    for grp in sorted(groups, key=lambda x: float(x) if x.replace('.','',1).isdigit() else x):
        print(f"\n=== {grp}m ===")
        print(header)
        print(sep)
        for _, aid, data, start, finish, actual, new_pb in sorted(groups[grp], key=sort_key):
            prev_pb = data.get('pb', 0.0)
            start_str = f"{start:<10.2f}"
            if finish == 'DLQ' or finish is None:
                finish_str = f"{'DLQ':<11}"
                actual_str = f"{'DLQ':<11}"
            else:
                finish_str = f"{finish:<11.2f}"
                actual_str = f"{actual:<11.2f}"
            print(
                f"{aid:<11}"
                f"{data['name']:<20}"
                f"{prev_pb:<11.2f}"
                f"{start_str}"
                f"{finish_str}"
                f"{actual_str}"
                f"{new_pb}"
            )

    # Offer to save session
    session_rows = []
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    for grp, entries in groups.items():
        for _, aid, data, start, finish, actual, new_pb in entries:
            session_rows.append((
                timestamp,
                grp,
                aid,
                data['name'],
                f"{start:.2f}",
                'DLQ' if finish in (None, 'DLQ') else f"{finish:.2f}",
                'DLQ' if actual is None else f"{actual:.2f}",
                new_pb
            ))

    save = input("\nSave these results to sessions.csv? (y/n): ").strip().lower()
    if save == 'y':
        os.makedirs(os.path.dirname(SESSION_FILE), exist_ok=True)
        write_header = not os.path.exists(SESSION_FILE)
        with open(SESSION_FILE, 'a', newline='') as f:
            w = csv.writer(f)
            if write_header:
                w.writerow([
                    "Timestamp","Distance","AthleteID","Name",
                    "Start(s)","Finish(s)","Actual(s)","NewPB"
                ])
            w.writerows(session_rows)
        print("✅ Session saved to sessions.csv.")

    has_new = any(row[7] == 'YES' for row in session_rows)
    if has_new:
        upd = input("New PBs detected. Update athletes.csv with new PBs? (y/n): ").strip().lower()
        if upd == 'y':
            for timestamp, dist, aid, name, start_s, finish_s, actual_s, new_pb in session_rows:
                if new_pb == 'YES' and actual_s != 'DLQ':
                    athletes[aid]['pbs'][dist] = float(actual_s)
            write_athletes_csv(athletes, CSV_PATH)
            print("✅ athletes.csv updated with new PBs.")

    input("\nPress ENTER to continue...")

# ───────────────────────── Menus ─────────────────────────

def devices_menu():
    global devices
    while True:
        clear_screen()
        print("=== Devices ===")
        print("\nCurrent devices:", ", ".join(sorted(devices, key=lambda x:int(x))) if devices else "(none)")
        print("\n1. Discover (DISCOVER)")
        print("2. Flash test (FLASH)")
        print("3. Set default volume")
        print("4. Set per-device volumes")
        print("5. Show volume settings")
        print("6. Add virtual devices (for bench)")
        print("Enter to go back")
        choice = input("Select: ").strip()
        if choice == '1':
            found = discover_devices(timeout=2)
            if found:
                for d in found:
                    if d not in devices:
                        devices.append(d)
            input("Press ENTER to continue…")
        elif choice == '2':
            flash_all_devices(timeout=2)
            input("Press ENTER to continue…")
        elif choice == '3':
            set_default_volume_interactive()
        elif choice == '4':
            set_per_device_volumes()
        elif choice == '5':
            list_volumes()
        elif choice == '6':
            add_virtual_devices()
        elif choice == '':
            break
        else:
            input("Press ENTER to continue...")

def setup_track():
    global devices
    while True:
        clear_screen()
        print("=== Setup Track ===")
        if start_points:
            print("\nCurrent Start Points:")
            print(f"{'Distance':<10}{'Lane':<6}{'Device':<15}")
            print("-"*31)
            for dist, sp in start_points.items():
                if sp['has_lanes']:
                    for lane in range(1, sp['num_lanes']+1):
                        dev = sp['device_assignments'].get(f"Lane {lane}", '-')
                        print(f"{dist+'m':<10}{lane:<6}{dev:<15}")
                else:
                    devs = ", ".join(sp['devices']) or '-'
                    print(f"{dist+'m':<10}{'--':<6}{devs:<15}")
        else:
            print("\nNo start points defined yet.")
        print("\nDiscovered Devices:")
        if devices:
            sorted_devs = sorted(devices, key=lambda x:int(x))
            print(", ".join(sorted_devs))
        else:
            print("None")

        print("\n1. Define Start Points")
        print("2. Add Virtual Devices")
        print("3. Assign Devices")
        print("Enter to go back to Main Menu")

        choice = input("Select an option: ").strip()
        if choice == '1':
            define_start_points()
        elif choice == '2':
            devices = add_virtual_devices()
        elif choice == '3':
            for sp in start_points.values():
                sp['device_assignments'].clear()
                sp['devices'].clear()
            print(f"\nDevices to assign: {devices}")
            for dist, sp in start_points.items():
                print(f"\nAssigning devices for {dist}m:")
                if sp['has_lanes']:
                    for lane in range(1, sp['num_lanes']+1):
                        dev = input(f"  Device for lane {lane} (Enter to skip): ").strip()
                        if dev and dev in devices:
                            sp['device_assignments'][f"Lane {lane}"] = dev
                        elif dev:
                            print("❌ Invalid device ID.")
                else:
                    while True:
                        dev = input("  Device (Enter to stop): ").strip()
                        if not dev:
                            break
                        if dev in devices:
                            sp['devices'].append(dev)
                        else:
                            print("❌ Invalid device ID.")
            input("Press ENTER to continue...")
        elif choice == '':
            break
        else:
            input("Press ENTER to continue...")

def athletes_menu():
    while True:
        clear_screen()
        print("=== Athletes ===")
        print("1. Add new athlete")
        print("2. Find by name")
        print("Enter to go back")
        choice = input("Select: ").strip()
        if choice == '1':
            add_new_athlete_interactive()
        elif choice == '2':
            q = input("Enter part of name: ").strip()
            matches = search_athletes_by_name(q)
            if not matches:
                print("No matches.")
            else:
                print(f"\n{'ID':<10}{'Name':<26}")
                print("-" * 36)
                for aid, name in matches:
                    print(f"{aid:<10}{name:<26}")
            input("\nPress ENTER to continue…")
        elif choice == '':
            break
        else:
            input("Press ENTER to continue…")

# ───────────────────────── Main ─────────────────────────

def main():
    global athletes, racers
    athletes = load_athletes()
    racers = {}
    while True:
        clear_screen()
        print("🏁 Athletics Race Manager 🏁")
        print("1. Devices")
        print("2. Setup Track")
        print("3. Setup Race")
        print("4. Review/Override Timings")
        print("5. Show Execution Times")
        print("6. Start Race")
        print("7. Enter Results")
        print("8. Athletes")
        print("Enter to Exit")
        choice = input("Select: ").strip()

        if choice == '1':
            devices_menu()
        elif choice == '2':
            setup_track()
        elif choice == '3':
            racers = get_race_participants(athletes, start_points)
            if racers:
                distance = input("Distance for handicap calc (e.g. 100): ").strip()
                if distance:
                    calculate_staggered_starts(racers, distance)

                    # Optional: choose lane pattern if laned
                    if distance in start_points and start_points[distance].get("has_lanes"):
                        while True:
                            print("\nLane pattern options:")
                            print("  1) Outside-in (slowest → lanes [1, N, 2, N-1, ...])")
                            print("  2) Left-to-right (slowest..fastest → lanes 1..N)")
                            print("ENTER to skip")
                            pat = input("Choose pattern: ").strip()
                            if pat == '':
                                break
                            if pat == '1':
                                apply_handicap_lane_pattern(distance, racers, pattern="outside_in")
                                break
                            elif pat == '2':
                                apply_handicap_lane_pattern(distance, racers, pattern="left_to_right")
                                break
                            else:
                                print("❌ Invalid choice.")

                    # Auto-compute & inject timings (PB, start, device)
                    compute_and_apply_timings(racers)

                    # If scratch distance, offer one-shot global offset (here, not in Option 4)
                    sp = start_points.get(distance)
                    if sp and not sp.get("has_lanes"):
                        override_scratch_offset(distance, racers)

                    print("✅ Timings calculated and applied.")
                    input("Press ENTER to continue…")
        elif choice == '4':
            calculate_timings(racers)     # show the table
            override_start_delays(racers) # edits allowed only for laned events
        elif choice == '5':
            show_device_schedule(racers)
        elif choice == '6':
            start_race_sequence(racers)
        elif choice == '7':
            enter_race_results(racers)
        elif choice == '8':
            athletes_menu()
        elif choice == '':
            # Confirm exit
            while True:
                ans = input("Are you sure you want to exit? (y/n): ").strip().lower()
                if ans == 'y':
                    return  # triggers the finally: ser.close()
                elif ans in ('n', ''):
                    break    # back to the main menu
                else:
                    print("Please answer 'y' or 'n'.")
                    time.sleep(0.6)
        else:
            print("❌ Invalid choice.")
            input("\nPress ENTER to continue...")

if __name__ == '__main__':
    try:
        main()
    finally:
        if ser is not None:
            ser.close()
