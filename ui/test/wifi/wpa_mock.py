#!/usr/bin/env python3
"""wpa_mock.py SOCKET LOG - a tiny stand-in for wpa_supplicant's control socket (wpa_ctrl datagrams), enough for
Orchard's wifi.c with ORCHARD_WIFI=real: one saved network "HomeNet-5G" on 5180 MHz, connected. Answers at once (the
snap build runs on a simulated clock), logs every command with a counter to LOG. DISCONNECT / RECONNECT / SCAN
behave like wpa_supplicant's (DISCONNECT sets "disconnected": no joining by itself until RECONNECT).
MOCK_FAIL_RECONNECT=N: the first N RECONNECTs answer FAIL (as while wlan0 is still down). MOCK_DROP_RECONNECT=N /
MOCK_DROP_SCAN=N: the first N RECONNECTs / SCANs get no reply at all and change nothing (a busy or restarted
wpa_supplicant: Orchard gives the socket up after 5 s and opens it again). MOCK_LATE_RECONNECT=N: the first N
RECONNECTs are answered (and acted on) only when the next ATTACH comes, to the address they came from (a stuck
wpa_supplicant catching up after Orchard reopened its socket). Stops on SIGTERM."""
import os, signal, socket, sys

path, logp = sys.argv[1], sys.argv[2]
fail_left = int(os.environ.get("MOCK_FAIL_RECONNECT", "0"))
late_left = {"RECONNECT": int(os.environ.get("MOCK_LATE_RECONNECT", "0"))}
held = []
drop_left = {"RECONNECT": int(os.environ.get("MOCK_DROP_RECONNECT", "0")), "SCAN": int(os.environ.get("MOCK_DROP_SCAN", "0"))}
BSSID, SSID, FREQ = "02:00:00:00:00:01", "HomeNet-5G", 5180
st = {"state": "COMPLETED", "ip": "198.51.100.37", "disconnected": False}
attached = set()
log = open(logp, "w", buffering=1)
n = 0

try:
    os.unlink(path)
except FileNotFoundError:
    pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(path)
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))


def event(msg):
    for a in list(attached):
        try:
            s.sendto(msg.encode(), a)
        except OSError:
            attached.discard(a)


def status():
    up = st["state"] == "COMPLETED"
    r = f"bssid={BSSID}\nfreq={FREQ}\nssid={SSID}\nid=0\nmode=station\nkey_mgmt=WPA2-PSK\n" if up else ""
    r += f"wpa_state={st['state']}\n"
    if up and st["ip"]:
        r += f"ip_address={st['ip']}\n"
    return r + "address=02:00:00:00:00:a0\n"


def connect():
    st.update(state="COMPLETED", ip="198.51.100.37", disconnected=False)
    event(f"<3>CTRL-EVENT-CONNECTED - Connection to {BSSID} completed [id=0 id_str=]")


while True:
    data, addr = s.recvfrom(4096)
    cmd = data.decode(errors="replace").strip()
    n += 1
    log.write(f"{n} {cmd}\n")
    r = "OK\n"
    word = cmd.split(" ")[0]
    if late_left.get(word, 0) > 0:
        late_left[word] -= 1
        held.append((addr, cmd))
        log.write(f"{n} (held, answered at the next ATTACH)\n")
        continue
    if drop_left.get(word, 0) > 0:
        drop_left[word] -= 1
        log.write(f"{n} (dropped, no reply)\n")
        continue
    if cmd == "ATTACH":
        attached.add(addr)
        for a, c in held:
            try:
                s.sendto(b"OK\n", a)
                log.write(f"late reply to {c} delivered to {a}\n")
            except OSError as e:
                log.write(f"late reply to {c} lost ({e.__class__.__name__})\n")
            if c == "RECONNECT" and st["disconnected"]:
                connect()
        held.clear()
    elif cmd == "DETACH":
        attached.discard(addr)
    elif cmd == "STATUS":
        r = status()
    elif cmd == "LIST_NETWORKS":
        r = "network id / ssid / bssid / flags\n0\t%s\tany\t%s\n" % (SSID, "[CURRENT]" if st["state"] == "COMPLETED" else "")
    elif cmd == "SCAN_RESULTS":
        r = f"bssid / frequency / signal level / flags / ssid\n{BSSID}\t{FREQ}\t-51\t[WPA2-PSK-CCMP][ESS]\t{SSID}\n"
    elif cmd == "SIGNAL_POLL":
        r = "RSSI=-51\nLINKSPEED=433\nNOISE=9999\nFREQUENCY=%d\n" % FREQ if st["state"] == "COMPLETED" else "FAIL\n"
    elif cmd.startswith("SCAN"):
        s.sendto(r.encode(), addr)
        event("<2>CTRL-EVENT-SCAN-STARTED ")
        event("<2>CTRL-EVENT-SCAN-RESULTS ")
        continue
    elif cmd == "DISCONNECT":
        st.update(state="DISCONNECTED", ip="", disconnected=True)
        s.sendto(r.encode(), addr)
        event(f"<3>CTRL-EVENT-DISCONNECTED bssid={BSSID} reason=3 locally_generated=1")
        continue
    elif cmd in ("RECONNECT", "REASSOCIATE"):
        if cmd == "RECONNECT" and fail_left > 0:
            fail_left -= 1
            r = "FAIL\n"
        else:
            s.sendto(r.encode(), addr)
            if st["disconnected"] or st["state"] != "COMPLETED":
                connect()
            continue
    s.sendto(r.encode(), addr)
