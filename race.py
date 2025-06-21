import csv
import os
import time
import serial
import serial.tools.list_ports
import socket

CSV_PATH = "data/athletes.csv"
CSV_PATH2= "data/athletes2.csv"
SESSION_FILE = "data/sessions.csv"
SERIAL_PORT = None
BAUD_RATE = 115200
SERIAL_TIMEOUT = 1

WRITE_TIMEOUT  = 1       # max seconds to block on write

# Global state
devices = []
start_points = {}
athletes = {}
ser = None

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

def test_all_devices():
    init_serial()

    # 1) Clear any old data
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # 2) Send the broadcast command
    cmd = "broadcast\n"
    ser.write(cmd.encode())
    ser.flush()
    print(f"→ Sent: {cmd.strip()}")

    # 3) Wait up to 10 s for the completion marker (no per‐line prints)
    start = time.time()
    deadline = start + 10
    while time.time() < deadline:
        if ser.in_waiting:
            line = ser.readline().decode(errors='ignore').strip()
            if not line:
                continue
            if line.lower() == "broadcast sequence complete.":
                elapsed = time.time() - start
                print(f"✅ Broadcast complete in {elapsed:.3f}s\n")
                break
    else:
        print(f"⚠️  No completion message after {10:.1f}s.\n")

    input("Press ENTER to continue...")

    
def load_athletes(file_path=CSV_PATH):
    """
    Load athletes from CSV. Expects columns:
      ID, Name, <distance1>, <distance2>, ...
    Returns a dict:
      { athlete_id: { "name": str, "pbs": { distance: float, ... } } }
    """
    athletes = {}
    with open(file_path, newline='') as csvfile:
        reader = csv.DictReader(csvfile)
        for row in reader:
            aid = row['ID']
            name = row['Name']
            pbs = {}
            for col, val in row.items():
                if col in ('ID', 'Name'):
                    continue
                if val.strip():
                    try:
                        pbs[col] = float(val)
                    except ValueError:
                        print(f"⚠️  Skipping non-numeric PB for athlete {aid}: {col}='{val}'")
            athletes[aid] = {"name": name, "pbs": pbs}
    return athletes


def send_all_reset_and_listen():
    """
    Broadcast ALL RESET to serial and listen 5s for any "Device:XX" replies.
    """
    global SERIAL_PORT
    init_serial()
    
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    ser.write(b'ALL RESET\n')

    discovered = set()
    end_time = time.time() + 5
    while time.time() < end_time:
        if ser.in_waiting>0:
            raw = ser.readline()
            line = raw.decode('utf-8', errors='ignore').strip()
            if line.startswith("Device:"):
                parts = line.split(":", 1)
                if len(parts) == 2:
                    dev_id = parts[1].strip()
                    if dev_id not in discovered:
                        discovered.add(dev_id)
                        print(f"✅ Found device: {dev_id}")
    input("Press ENTER to continue...")
    return list(discovered)


def get_race_participants(athletes, start_points):
    racers = {}
    for dist, sp in start_points.items():
        print(f"Setting up race for {dist}m:")
        if sp["has_lanes"]:
            for lane in range(1, sp["num_lanes"] + 1):
                while True:
                    aid = input(f"  Lane {lane} Athlete ID (Enter to skip): ").strip().upper()
                    if aid == "":
                        break
                    if aid not in athletes:
                        print("❌ ID not found.")
                        continue
                    #racers[aid] = {"name": athletes[aid]["name"], "pbs": athletes[aid]["pbs"].copy()}
                    racers[aid] = {
                        "name": athletes[aid]["name"],
                        "pbs":  athletes[aid]["pbs"].copy(),
                        "start_point": dist      # ← record the event distance
                    }
                    sp["assignments"][f"Lane {lane}"] = aid
                    break
        else:
            print("  Enter athletes for this start point (press Enter to finish):")
            while True:
                aid = input("  Athlete ID: ").strip().upper()
                if aid == "":
                    break
                if aid not in athletes:
                    print("❌ ID not found.")
                    continue
                if aid in racers:
                    print("⚠️ Already added.")
                    continue
                #racers[aid] = {"name": athletes[aid]["name"], "pbs": athletes[aid]["pbs"].copy()}
                racers[aid] = {
                    "name": athletes[aid]["name"],
                    "pbs":  athletes[aid]["pbs"].copy(),
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
        # 1) Clear & show header
        clear_screen()
        print("=== Define Start Points ===\n")
        print(f"Valid distances: {opts}")

        # 2) Show existing
        if start_points:
            print("Already defined:")
            for d, sp in sorted(start_points.items(), key=lambda x: int(x[0])):
                if sp["has_lanes"]:
                    print(f"  • {d} m  → {sp['num_lanes']} lane(s)")
                else:
                    print(f"  • {d} m  → no lanes")
            print()

        # 3) Prompt
        dist = input("Enter distance, 'c' to clear, or press ENTER to finish: ").strip().lower()

        # 3a) Clear-all option
        if dist == 'c':
            confirm = input("❗ This will remove all start points. Are you sure? (y/n): ").strip().lower()
            if confirm == 'y':
                start_points.clear()
            continue  # redisplay menu

        # 3b) Finish
        if dist == "":
            break

        # 4) Validate
        if dist not in valid:
            print(f"\n❌ Invalid distance. Choose from: {opts}")
            input("\nPress ENTER to try again…")
            continue
        if dist in start_points:
            print(f"\n❌ You’ve already defined {dist} m.")
            input("\nPress ENTER to try again…")
            continue

        # 5) Gather lanes info
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

        # 6) Save and loop (will auto-refresh view)
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


def collect_start_point_timings(start_points, athletes):
    data = {}
    for dist, sp in start_points.items():
        rows = []
        if sp["has_lanes"]:
            entries = [(lane, aid) for lane, aid in sp["assignments"].items() if aid in athletes]
            pbs = [athletes[aid]["pbs"].get(dist) for _, aid in entries if dist in athletes[aid]["pbs"]]
            slowest = max(pbs) if pbs else 0.0
        else:
            entries = [(None, aid) for aid in sp["assignments"].values() if aid in athletes]
            slowest = None
        for lane, aid in entries:
            pb_val = athletes[aid]["pbs"].get(dist)
            if pb_val is None:
                continue
            headstart = round(slowest - pb_val, 2) if sp["has_lanes"] else 0.0
            rows.append({
                "lane": lane if sp["has_lanes"] else "-",
                "id": aid,
                "name": athletes[aid]["name"],
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

    # 1) Collect timing data for every distance
    timing_data = collect_start_point_timings(start_points, athletes)

    # 2) Display tables for each distance automatically
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

    # 3) Inject data back into racers for every distance
    for dist, rows in timing_data.items():
        sp = start_points[dist]
        if sp['has_lanes']:
            for row in rows:
                aid = row['id']
                racers[aid]['start'] = row['start']
                racers[aid]['device'] = sp['device_assignments'].get(row['lane'], '-')
                racers[aid]['pb'] = row['pb']
        else:
            default_dev = sp['devices'][0] if sp['devices'] else '-'
            for row in rows:
                aid = row['id']
                racers[aid]['start'] = 0.0
                racers[aid]['device'] = default_dev
                racers[aid]['pb'] = row['pb']

    return timing_data


def show_command_sequence(racers):
    if not racers:
        print("❌ No race has been set up yet.")
        input("\nPress ENTER to continue...")
        return

    # build and sort the event list as before
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

    # mapping from code → word
    code_map = {
        '0': "-",
        '1': "Marks",
        '2': "Set",
        '3': "Go"
        # leave '0' as-is, or add '0': "Off" if you like
    }

    print("\n⏱️  Command Sequence:")
    print(f"{'Time(s)':<8}{'Device':<12}{'Action'}")
    print("-" * 28)
    for t, dev, cmd in events:
        action = code_map.get(cmd, cmd)
        print(f"{t:>7.2f}  {dev:<12} {action}")


def build_device_schedule(racers):
    starts = [r.get('start', 0.0) for r in racers.values()]
    min_start = min(starts) if starts else 0.0
    schedule = {}
    RED_D = 5.0; ORANGE_D = 5.0; GREEN_D = 10.0; OFF_D = 8.0
    for aid, r in racers.items():
        dev = r.get('device', '-')
        red_on = r.get('start', 0.0) - min_start
        schedule[dev] = {
            'red_on':     red_on,
            'red_off':    red_on + RED_D,
            'orange_on':  red_on + RED_D,
            'orange_off': red_on + RED_D + ORANGE_D,
            'green_on':   red_on + GREEN_D,
            'green_off':  red_on + GREEN_D + OFF_D
        }
    return schedule


def start_race_sequence(racers):
    clear_screen()
    print("\n⏱️  Upcoming Command Sequence:")
    show_command_sequence(racers)
    if not racers:
          return
    input("\nPress ENTER to start the race...")
    sched = build_device_schedule(racers)
    events = [(t, dev, cmd) for dev, times in sched.items() for t, cmd in [
        (times['red_on'], '1'),
        (times['orange_on'], '2'),
        (times['green_on'], '3'),
        (times['green_off'], '0')
    ]]
    events.sort(key=lambda e: e[0])
    global SERIAL_PORT
    init_serial()
    
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    
    t0 = time.time(); launched=False
    for t_event, dev, cmd in events:
        delay = t_event - (time.time() - t0)
        if delay>0: time.sleep(delay)
        ser.write(f"{dev} {cmd}\n".encode())
        print(f"{time.time()-t0:>6.2f}s: {dev} {cmd}")
        if cmd == '3' and not launched:
            send_start_command()
            launched = True
 
 
def send_start_command(host="127.0.0.1", port=6000, payload=b"s"):
    for attempt in range(1, 6):
        try:
            with socket.create_connection((host, port), timeout=0.2) as sock:
                sock.sendall(payload)
            print(f"✅ Sent “{payload!r}” on attempt {attempt}")
            return
        except Exception as e:
            print(f"⚠️  Start‐cmd attempt {attempt} failed: {e}")
            time.sleep(0.05)
    print("❌ Giving up on starting timer!")


def show_device_schedule(racers):
    clear_screen()
    if not racers:
        print("❌ No race has been set up yet.")
        input("\nPress ENTER to continue...")
        return

    # Build device light schedule
    sched = build_device_schedule(racers)

    # Header: include device, start point (distance), lane, and start times for each LED
    print("\n📋 Device Light Schedule (start times for each LED)\n")
    print(f"{'Device':<8}{'Distance':<10}{'Lane':<8}{'Red(s)':<10}{'Orange(s)':<12}{'Green(s)':<10}")
    print("-" * 58)

    # For each device, determine its distance, lane, and LED start times
    for dev, times in sched.items():
        # Locate distance and lane assignment
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

        # Extract LED start times
        red_start = times['red_on']
        orange_start = times['orange_on']
        green_start = times['green_on']

        # Print row
        print(
            f"{dev:<8}{distance:<10}{lane:<8}"
            f"{red_start:<10.2f}{orange_start:<12.2f}{green_start:<10.2f}"
        )

    # Wait for user before returning
    input("\nPress ENTER to continue...")


def enter_race_results(racers):
    """
    racers: dict mapping athlete ID → {
        'name': str,
        'pbs': dict,
        'start': float,
        'pb': float,
        'device': str,
        'start_point': str,   # e.g. "100", "200", etc.
    }
    """
    from collections import defaultdict

    results = {}

    # 1) Collect finish times or DLQs, grouped by start_point
    while True:
        clear_screen()
        print("\nEnter race results. Recorded times shown in brackets; 'DLQ' for disqualified.\n")

        # Group athletes by their event
        grouped = defaultdict(list)
        for aid, data in racers.items():
            grp = data.get('start_point', 'Unknown')
            grouped[grp].append((aid, data))

        # Display and build index→ID map
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

        # Prompt for finish time or DLQ
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

    # 2) Build flat list: (group, aid, start, finish, actual, new_pb)
    flat = []
    for aid, data in racers.items():
        start = data.get('start', 0.0)
        finish = results.get(aid)           # either float or 'DLQ' or None
        if finish == 'DLQ' or finish is None:
            actual = None
            new_pb = ''
        else:
            actual = finish - start
            new_pb = 'YES' if actual < data.get('pb', float('inf')) else ''
        grp = data.get('start_point', 'Unknown')
        flat.append((grp, aid, data, start, finish, actual, new_pb))

    # 3) Regroup by start_point
    groups = defaultdict(list)
    for item in flat:
        groups[item[0]].append(item)

    # 4) Display final results, sorting DLQs after real times
    clear_screen()
    print("\n📊 Final Results by Event Group:\n")
    header = f"{'Athlete ID':<12}{'Name':<20}{'Start(s)':<10}" \
             f"{'Finish(s)':<12}{'Actual(s)':<12}{'New PB'}"
    sep = "-" * len(header)

    def sort_key(item):
        # item[5] is actual; put None (DLQ) at end
        return item[5] if item[5] is not None else float('inf')

    # print out the grid as before
    for grp in sorted(groups, key=lambda x: float(x) if x.replace('.','',1).isdigit() else x):
        print(f"\n=== {grp}m ===")
        print(header)
        print(sep)
        for _, aid, data, start, finish, actual, new_pb in sorted(groups[grp], key=sort_key):
            start_str = f"{start:<10.2f}"
            if finish == 'DLQ' or finish is None:
                finish_str = f"{'DLQ':<12}"
                actual_str = f"{'DLQ':<12}"
            else:
                finish_str = f"{finish:<12.2f}"
                actual_str = f"{actual:<12.2f}"
            print(f"{aid:<12}{data['name']:<20}"
                  f"{start_str}{finish_str}{actual_str}{new_pb}")

    # ─── New: Offer to save this session ─────────────────────────────────────
    # First, flatten out the entries into a list we can write
    import time, os, csv
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

        # ─── New: Offer to update athletes PB file if there are new PBs ──────────
    has_new = any(row[7] == 'YES' for row in session_rows)
    if has_new:
        upd = input("New PBs detected. Update athletes.csv with new PBs? (y/n): ").strip().lower()
        if upd == 'y':
            # 1) Update in-memory PBs from session_rows
            for timestamp, dist, aid, name, start_s, finish_s, actual_s, new_pb in session_rows:
                if new_pb == 'YES' and actual_s != 'DLQ':
                    try:
                        athletes[aid]['pbs'][dist] = float(actual_s)
                    except ValueError:
                        # in case actual_s wasn't numeric, skip
                        pass

            # 2) Gather all distance columns (sorted numerically where possible)
            distances = sorted(
                { d for ath in athletes.values() for d in ath['pbs'].keys() },
                key=lambda x: float(x) if x.replace('.', '', 1).isdigit() else x
            )

            # 3) Rewrite the athletes CSV using the updated dict
            import csv
            with open(CSV_PATH, 'w', newline='') as f:
                writer = csv.writer(f)
                writer.writerow(['ID','Name'] + distances)
                for aid, ath in athletes.items():
                    row = [aid, ath['name']]
                    for dist in distances:
                        val = ath['pbs'].get(dist)
                        row.append(f"{val:.2f}" if isinstance(val, float) else '')
                    writer.writerow(row)

            print("✅ athletes.csv updated with new PBs.")

    input("\nPress ENTER to continue...")


def setup_track():
    global devices
    while True:
        clear_screen()
        print("=== Setup Track ===")
        if start_points:
            print("\nCurrent Start Points:")
            print(f"{'Distance':<10}{'Lane':<6}{'Device':<15}")
            print("-"*31)
            for dist,sp in start_points.items():
                if sp['has_lanes']:
                    for lane in range(1,sp['num_lanes']+1):
                        dev=sp['device_assignments'].get(f"Lane {lane}",'-')
                        print(f"{dist+'m':<10}{lane:<6}{dev:<15}")
                else:
                    devs=", ".join(sp['devices'])or'-'
                    print(f"{dist+'m':<10}{'--':<6}{devs:<15}")
        else:
            print("\nNo start points defined yet.")
        print("\nDiscovered Devices:")
        if devices:
            sorted_devs=sorted(devices,key=lambda x:int(x))
            print(", ".join(sorted_devs))
        else:
            print("None")
        print("\n1. Define Start Points")
        print("2. Find Devices")
        print("3. Add Virtual Devices")
        print("4. Assign Devices")
        print("5. Test All Devices")
        print("Enter to go back to Main Menu")
        choice=input("Select an option: ").strip()
        if choice=='1': define_start_points()
        elif choice=='2': devices=send_all_reset_and_listen()
        elif choice=='3': devices=add_virtual_devices()
        elif choice=='4':
            for sp in start_points.values(): sp['device_assignments'].clear(); sp['devices'].clear()
            print(f"\nDevices to assign: {devices}")
            for dist,sp in start_points.items():
                print(f"\nAssigning devices for {dist}m:")
                if sp['has_lanes']:
                    for lane in range(1,sp['num_lanes']+1):
                        dev=input(f"  Device for lane {lane} (Enter to skip): ").strip()
                        if dev and dev in devices: sp['device_assignments'][f"Lane {lane}"]=dev
                        elif dev: print("❌ Invalid device ID.")
                else:
                    while True:
                        dev=input("  Device (Enter to stop): ").strip()
                        if not dev: break
                        if dev in devices: sp['devices'].append(dev)
                        else: print("❌ Invalid device ID.")
            input("Press ENTER to continue...")
        elif choice=='5': test_all_devices()
        elif choice=='': break
        else: input("Press ENTER to continue...")


def main():
    global athletes, racers
    athletes = load_athletes()
    racers = {}
    while True:
        clear_screen()
        print("🏁 Athletics Race Manager 🏁")
        print("1. Setup Track")
        print("2. Setup Race")
        print("3. Calculate Timings")
        print("4. Show Execution Times")
        print("5. Start Race")
        print("6. Enter Results")
        print("Enter to Exit")
        choice=input("Select: ").strip()
        if choice=='1': setup_track()
        elif choice=='2':
            racers=get_race_participants(athletes,start_points)
            if racers: distance=input("Hit Enter to continue").strip(); calculate_staggered_starts(racers,distance)
        elif choice=='3': calculate_timings(racers)
        elif choice=='4': show_device_schedule(racers)
        elif choice=='5': start_race_sequence(racers)
        elif choice=='6': enter_race_results(racers)
        elif choice=='': break
        else: print("❌ Invalid choice."); input("\nPress ENTER to continue...")

if __name__ == '__main__':
    try:
        main()
    finally:
        if ser is not None:
            ser.close()
