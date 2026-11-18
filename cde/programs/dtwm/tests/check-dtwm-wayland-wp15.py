#!/usr/bin/env python3
"""WP-15 parallel dual-environment integration check (Wayland + X11/XLibre):
1. Runtime Toolkit & Library Linkage:
   - Verifies both `ck-mixer` and `ck-status` map CK Core Motif ABI 9 (`libXm.so.9`),
     `libpulse.so.0`, and `libdbus-1.so.3`.
2. PipeWire / PulseAudio Mixer, Zero-ALSA Fallback, Mic Meter & Routing (`ck-mixer`):
   - Runs `ck-mixer` with `CK_MIXER_NO_ALSA=1` against an isolated `pipewire` +
     `wireplumber` + `pipewire-pulse` stack (`alsa_devices == 0`).
   - Verifies live microphone peak detection (`mic_peak_pct >= 10`) on a virtual mic,
     mic mute/unmute peak gating, default output sink switching (`desk_spk` -> `bt_a2dp`),
     sink volume & mute updates, per-application stream routing (`MusicStream` moved to
     `bt_a2dp`) & stream volume, and sound card profile switching (`pro-audio`).
3. Native Motif Wi-Fi (WPA2-PSK & WPA-Enterprise 802.1X), VPN & SecretAgent (`ck-status`):
   - Exercises `AddAndActivateConnection` onboarding for WPA2-PSK and WPA-Enterprise
     (`PEAP`/`MSCHAPv2`), VPN activation, and asynchronous `SecretAgent.GetSecrets`
     Motif dialog prompt & `vpn` secret dictionary round-trip.
4. Built-in BlueZ Pairing Agent (`org.bluez.Agent1`) & MPRIS Transport (`ck-status`):
   - Verifies `AgentManager1.RegisterAgent` (`KeyboardDisplay`), adapter discovery
     (`StartDiscovery`), non-blocking `Pair()` -> `Agent1.RequestConfirmation(614829)`
     Motif confirmation modal round-trip, `Trusted` toggle, `RemoveDevice` forget, and
     MPRIS `PlayPause` + `Next` media transport controls.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


X11_PROP_HELPER_C = r"""
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int win_has_prop(Display *dpy, Window w, Atom state_atom) {
    Atom act = None;
    int fmt = 0;
    unsigned long n = 0, rem = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, state_atom, 0, 4, False, AnyPropertyType,
                           &act, &fmt, &n, &rem, &data) == Success && data) {
        XFree(data);
        return act != None && n > 0;
    }
    return 0;
}

static Window find_by_state_atom(Display *dpy, Window w, Atom state_atom) {
    if (win_has_prop(dpy, w, state_atom)) return w;
    Window root = None, parent = None, *kids = NULL;
    unsigned int nkids = 0;
    if (!XQueryTree(dpy, w, &root, &parent, &kids, &nkids)) return None;
    Window found = None;
    for (unsigned int i = 0; i < nkids && !found; ++i) {
        found = find_by_state_atom(dpy, kids[i], state_atom);
    }
    if (kids) XFree(kids);
    return found;
}

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 2;
    if (argc < 2 || strcmp(argv[1], "ping") == 0) {
        XCloseDisplay(dpy);
        return 0;
    }
    if (argc >= 4 && strcmp(argv[1], "get") == 0) {
        Atom prop = XInternAtom(dpy, argv[3], False);
        Window w = find_by_state_atom(dpy, DefaultRootWindow(dpy), prop);
        if (!w) { XCloseDisplay(dpy); return 3; }
        Atom act = None;
        int fmt = 0;
        unsigned long n = 0, rem = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, w, prop, 0, 65536, False, AnyPropertyType,
                               &act, &fmt, &n, &rem, &data) == Success && data) {
            fwrite(data, 1, n, stdout);
            XFree(data);
            XCloseDisplay(dpy);
            return 0;
        }
        XCloseDisplay(dpy);
        return 4;
    }
    if (argc >= 5 && strcmp(argv[1], "set") == 0) {
        Atom state_atom = XInternAtom(dpy, argv[2], False);
        Window w = find_by_state_atom(dpy, DefaultRootWindow(dpy), state_atom);
        if (!w) { XCloseDisplay(dpy); return 3; }
        Atom prop = XInternAtom(dpy, argv[3], False);
        Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        XChangeProperty(dpy, w, prop, utf8, 8, PropModeReplace,
                        (const unsigned char *)argv[4], (int)strlen(argv[4]));
        XFlush(dpy);
        XCloseDisplay(dpy);
        return 0;
    }
    XCloseDisplay(dpy);
    return 1;
}
"""


PULSE_TONE_GEN_C = r"""
#include <math.h>
#include <pulse/pulseaudio.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(void) {
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    pa_mainloop *ml = pa_mainloop_new();
    pa_proplist *pl = pa_proplist_new();
    pa_proplist_sets(pl, PA_PROP_APPLICATION_NAME, "CDE Media Synth");
    pa_context *c = pa_context_new_with_proplist(pa_mainloop_get_api(ml), "CDE Media Synth", pl);
    pa_proplist_free(pl);
    if (pa_context_connect(c, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0) return 1;

    while (!g_stop && pa_context_get_state(c) != PA_CONTEXT_READY) {
        int r = 0;
        if (pa_mainloop_iterate(ml, 1, &r) < 0) return 2;
    }

    pa_sample_spec ss = {.format = PA_SAMPLE_FLOAT32NE, .rate = 8000, .channels = 2};
    pa_stream *ps_out = pa_stream_new(c, "MusicStream", &ss, NULL);
    pa_stream *ps_mic = pa_stream_new(c, "MicSignal", &ss, NULL);
    pa_stream_connect_playback(ps_out, "desk_spk", NULL, PA_STREAM_NOFLAGS, NULL, NULL);
    pa_stream_connect_playback(ps_mic, "mic_backing", NULL, PA_STREAM_NOFLAGS, NULL, NULL);

    float buf[512];
    for (int i = 0; i < 256; ++i) {
        float v = 0.72f * sinf((float)i * 0.35f);
        buf[i * 2] = v;
        buf[i * 2 + 1] = v;
    }

    while (!g_stop) {
        int r = 0;
        pa_mainloop_iterate(ml, 0, &r);
        if (pa_stream_get_state(ps_out) == PA_STREAM_READY && pa_stream_writable_size(ps_out) >= sizeof(buf)) {
            pa_stream_write(ps_out, buf, sizeof(buf), NULL, 0, PA_SEEK_RELATIVE);
        }
        if (pa_stream_get_state(ps_mic) == PA_STREAM_READY && pa_stream_writable_size(ps_mic) >= sizeof(buf)) {
            pa_stream_write(ps_mic, buf, sizeof(buf), NULL, 0, PA_SEEK_RELATIVE);
        }
        usleep(20000);
    }

    pa_stream_disconnect(ps_out);
    pa_stream_unref(ps_out);
    pa_stream_disconnect(ps_mic);
    pa_stream_unref(ps_mic);
    pa_context_disconnect(c);
    pa_context_unref(c);
    pa_mainloop_free(ml);
    return 0;
}
"""


MOCK_SYSTEM_SERVICES_PY = r"""#!/usr/bin/env python3
import json
import os
import sys
import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

STATE_FILE = sys.argv[1]
READY_FILE = sys.argv[2]

state_log = {
    "nm_agent_registered": False,
    "nm_agent_sender": "",
    "added_wifi": [],
    "last_vpn_secret": "",
    "bt_agent_registered": False,
    "bt_agent_sender": "",
    "bt_agent_path": "",
    "bt_discovering": False,
    "bt_paired": False,
    "bt_trusted": False,
    "bt_removed": False,
    "mpris_status": "Paused",
    "mpris_title": "Solaris Symphony",
    "mpris_calls": [],
}

def flush_log():
    tmp = STATE_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(state_log, f)
    os.replace(tmp, STATE_FILE)


class NmRoot(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/freedesktop/NetworkManager")
        self.bus = bus
        self.wireless_enabled = True
        self.active_ap = dbus.ObjectPath("/")
        self.active_conns = []

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        if prop == "WirelessEnabled":
            return dbus.Boolean(self.wireless_enabled)
        if prop == "ActiveConnections":
            return dbus.Array([dbus.ObjectPath(p) for p in self.active_conns], signature="o")
        return dbus.String("")

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ssv", out_signature="")
    def Set(self, iface, prop, val):
        if prop == "WirelessEnabled":
            self.wireless_enabled = bool(val)

    @dbus.service.method("org.freedesktop.NetworkManager", in_signature="", out_signature="ao")
    def GetDevices(self):
        return dbus.Array([dbus.ObjectPath("/org/freedesktop/NetworkManager/Devices/1")], signature="o")

    @dbus.service.method("org.freedesktop.NetworkManager", in_signature="a{sa{sv}}oo", out_signature="oo")
    def AddAndActivateConnection(self, conn_dict, dev, spec_obj):
        rec = {"dev": str(dev), "ap": str(spec_obj)}
        if "connection" in conn_dict and "id" in conn_dict["connection"]:
            rec["id"] = str(conn_dict["connection"]["id"])
        if "802-11-wireless-security" in conn_dict:
            wsec = conn_dict["802-11-wireless-security"]
            rec["key_mgmt"] = str(wsec.get("key-mgmt", ""))
            if "psk" in wsec:
                rec["psk"] = str(wsec["psk"])
        if "802-1x" in conn_dict:
            x1 = conn_dict["802-1x"]
            rec["eap"] = [str(x) for x in x1.get("eap", [])]
            rec["phase2"] = str(x1.get("phase2-auth", ""))
            rec["identity"] = str(x1.get("identity", ""))
            rec["password"] = str(x1.get("password", ""))
            rec["system_ca"] = bool(x1.get("system-ca-certs", False))
        state_log["added_wifi"].append(rec)
        if str(spec_obj) != "/":
            self.active_ap = dbus.ObjectPath(str(spec_obj))
        flush_log()
        return (
            dbus.ObjectPath("/org/freedesktop/NetworkManager/Settings/99"),
            dbus.ObjectPath("/org/freedesktop/NetworkManager/ActiveConnection/99"),
        )

    @dbus.service.method("org.freedesktop.NetworkManager", in_signature="ooo", out_signature="o",
                         async_callbacks=("reply_cb", "err_cb"))
    def ActivateConnection(self, conn_path, dev, spec, reply_cb, err_cb):
        cpath = str(conn_path)
        if cpath == "/org/freedesktop/NetworkManager/Settings/10" and state_log["nm_agent_sender"]:
            # Call SecretAgent.GetSecrets asynchronously on ck-status!
            conn_settings = dbus.Dictionary({
                "connection": dbus.Dictionary({
                    "id": dbus.String("Corp_WireGuard_VPN", variant_level=1),
                    "type": dbus.String("vpn", variant_level=1),
                }, signature="sv")
            }, signature="sa{sv}")

            def on_secrets(secrets):
                vpn_sec = secrets.get("vpn", {}).get("secrets", {})
                state_log["last_vpn_secret"] = str(vpn_sec.get("password", ""))
                ac_path = "/org/freedesktop/NetworkManager/ActiveConnection/10"
                if ac_path not in self.active_conns:
                    self.active_conns.append(ac_path)
                flush_log()
                reply_cb(dbus.ObjectPath(ac_path))

            def on_err(e):
                err_cb(e)

            self.bus.call_async(
                state_log["nm_agent_sender"],
                "/org/freedesktop/NetworkManager/SecretAgent",
                "org.freedesktop.NetworkManager.SecretAgent",
                "GetSecrets",
                "a{sa{sv}}osasu",
                (conn_settings, dbus.ObjectPath(cpath), "vpn", dbus.Array(["password"], signature="s"), dbus.UInt32(1)),
                on_secrets,
                on_err,
            )
            return
        reply_cb(dbus.ObjectPath("/org/freedesktop/NetworkManager/ActiveConnection/1"))

    @dbus.service.method("org.freedesktop.NetworkManager", in_signature="o", out_signature="")
    def DeactivateConnection(self, ac_path):
        ap = str(ac_path)
        if ap in self.active_conns:
            self.active_conns.remove(ap)
        flush_log()


class NmAgentMgr(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/freedesktop/NetworkManager/AgentManager")

    @dbus.service.method("org.freedesktop.NetworkManager.AgentManager", in_signature="s", out_signature="",
                         sender_keyword="sender")
    def Register(self, ident, sender=None):
        state_log["nm_agent_registered"] = True
        state_log["nm_agent_sender"] = str(sender or "")
        flush_log()


class NmSettings(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/freedesktop/NetworkManager/Settings")

    @dbus.service.method("org.freedesktop.NetworkManager.Settings", in_signature="", out_signature="ao")
    def ListConnections(self):
        return dbus.Array([dbus.ObjectPath("/org/freedesktop/NetworkManager/Settings/10")], signature="o")


class NmVpnConn(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/freedesktop/NetworkManager/Settings/10")

    @dbus.service.method("org.freedesktop.NetworkManager.Settings.Connection", in_signature="", out_signature="a{sa{sv}}")
    def GetSettings(self):
        return dbus.Dictionary({
            "connection": dbus.Dictionary({
                "id": dbus.String("Corp_WireGuard_VPN", variant_level=1),
                "type": dbus.String("vpn", variant_level=1),
            }, signature="sv")
        }, signature="sa{sv}")


class NmActiveVpn(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/freedesktop/NetworkManager/ActiveConnection/10")

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return dbus.Dictionary({
            "Connection": dbus.ObjectPath("/org/freedesktop/NetworkManager/Settings/10", variant_level=1),
            "Type": dbus.String("vpn", variant_level=1),
            "State": dbus.UInt32(2, variant_level=1),
        }, signature="sv")


class NmWifiDev(dbus.service.Object):
    def __init__(self, bus, nm_root):
        super().__init__(bus, "/org/freedesktop/NetworkManager/Devices/1")
        self.nm_root = nm_root
        self.aps = [
            "/org/freedesktop/NetworkManager/AccessPoint/1",
            "/org/freedesktop/NetworkManager/AccessPoint/2",
            "/org/freedesktop/NetworkManager/AccessPoint/3",
        ]

    @dbus.service.method("org.freedesktop.NetworkManager.Device.Wireless", in_signature="", out_signature="ao")
    def GetAllAccessPoints(self):
        return dbus.Array([dbus.ObjectPath(p) for p in self.aps], signature="o")

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        if prop == "ActiveAccessPoint":
            return self.nm_root.active_ap
        return dbus.String("")


class NmAp(dbus.service.Object):
    def __init__(self, bus, path, ssid, strength, wpa_flags, rsn_flags):
        super().__init__(bus, path)
        self.ssid = ssid
        self.strength = strength
        self.wpa_flags = wpa_flags
        self.rsn_flags = rsn_flags

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return dbus.Dictionary({
            "Ssid": dbus.Array([dbus.Byte(b) for b in self.ssid.encode("utf-8")], signature="y", variant_level=1),
            "Strength": dbus.Byte(self.strength, variant_level=1),
            "Flags": dbus.UInt32(1 if (self.wpa_flags or self.rsn_flags) else 0, variant_level=1),
            "WpaFlags": dbus.UInt32(self.wpa_flags, variant_level=1),
            "RsnFlags": dbus.UInt32(self.rsn_flags, variant_level=1),
        }, signature="sv")


class BluezService(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/")
        self.bus = bus
        self.powered = True
        self.discovering = False
        self.dev_present = True
        self.dev_paired = False
        self.dev_connected = False
        self.dev_trusted = False

    @dbus.service.method("org.freedesktop.DBus.ObjectManager", in_signature="", out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        objs = {
            dbus.ObjectPath("/org/bluez/hci0"): {
                "org.bluez.Adapter1": dbus.Dictionary({
                    "Powered": dbus.Boolean(self.powered, variant_level=1),
                    "Discovering": dbus.Boolean(self.discovering, variant_level=1),
                    "Alias": dbus.String("CDE-Workstation-BT", variant_level=1),
                }, signature="sv")
            }
        }
        if self.dev_present and (self.discovering or self.dev_paired):
            objs[dbus.ObjectPath("/org/bluez/hci0/dev_AA_BB_CC_44_55_66")] = {
                "org.bluez.Device1": dbus.Dictionary({
                    "Name": dbus.String("Sony WH-1000XM5", variant_level=1),
                    "Alias": dbus.String("Sony WH-1000XM5", variant_level=1),
                    "Address": dbus.String("AA:BB:CC:44:55:66", variant_level=1),
                    "Icon": dbus.String("audio-headset", variant_level=1),
                    "Paired": dbus.Boolean(self.dev_paired, variant_level=1),
                    "Trusted": dbus.Boolean(self.dev_trusted, variant_level=1),
                    "Connected": dbus.Boolean(self.dev_connected, variant_level=1),
                    "RSSI": dbus.Int16(-48, variant_level=1),
                }, signature="sv")
            }
        return dbus.Dictionary(objs, signature="oa{sa{sv}}")


class BluezAgentMgr(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/bluez")

    @dbus.service.method("org.bluez.AgentManager1", in_signature="os", out_signature="", sender_keyword="sender")
    def RegisterAgent(self, path, cap, sender=None):
        state_log["bt_agent_registered"] = True
        state_log["bt_agent_sender"] = str(sender or "")
        state_log["bt_agent_path"] = str(path)
        flush_log()

    @dbus.service.method("org.bluez.AgentManager1", in_signature="o", out_signature="")
    def RequestDefaultAgent(self, path):
        pass


class BluezAdapter(dbus.service.Object):
    def __init__(self, bus, root):
        super().__init__(bus, "/org/bluez/hci0")
        self.root = root

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ssv", out_signature="")
    def Set(self, iface, prop, val):
        if prop == "Powered":
            self.root.powered = bool(val)

    @dbus.service.method("org.bluez.Adapter1", in_signature="", out_signature="")
    def StartDiscovery(self):
        self.root.discovering = True
        state_log["bt_discovering"] = True
        flush_log()

    @dbus.service.method("org.bluez.Adapter1", in_signature="", out_signature="")
    def StopDiscovery(self):
        self.root.discovering = False
        state_log["bt_discovering"] = False
        flush_log()

    @dbus.service.method("org.bluez.Adapter1", in_signature="o", out_signature="")
    def RemoveDevice(self, dev_path):
        if str(dev_path) == "/org/bluez/hci0/dev_AA_BB_CC_44_55_66":
            self.root.dev_present = False
            state_log["bt_removed"] = True
            flush_log()


class BluezDevice(dbus.service.Object):
    def __init__(self, bus, root):
        super().__init__(bus, "/org/bluez/hci0/dev_AA_BB_CC_44_55_66")
        self.bus = bus
        self.root = root

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ssv", out_signature="")
    def Set(self, iface, prop, val):
        if prop == "Trusted":
            self.root.dev_trusted = bool(val)
            state_log["bt_trusted"] = self.root.dev_trusted
            flush_log()

    @dbus.service.method("org.bluez.Device1", in_signature="", out_signature="",
                         async_callbacks=("reply_cb", "err_cb"))
    def Pair(self, reply_cb, err_cb):
        sender = state_log["bt_agent_sender"]
        apath = state_log["bt_agent_path"]
        assert sender and apath

        def on_ok():
            self.root.dev_paired = True
            self.root.dev_connected = True
            state_log["bt_paired"] = True
            flush_log()
            reply_cb()

        def on_err(e):
            err_cb(e)

        self.bus.call_async(
            sender, apath, "org.bluez.Agent1", "RequestConfirmation", "ou",
            (dbus.ObjectPath("/org/bluez/hci0/dev_AA_BB_CC_44_55_66"), dbus.UInt32(614829)),
            on_ok, on_err,
        )

    @dbus.service.method("org.bluez.Device1", in_signature="", out_signature="")
    def Connect(self):
        self.root.dev_connected = True

    @dbus.service.method("org.bluez.Device1", in_signature="", out_signature="")
    def Disconnect(self):
        self.root.dev_connected = False


class MprisPlayer(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/mpris/MediaPlayer2")

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        if prop == "Identity":
            return dbus.String("CDE Audio Deck")
        return dbus.String("")

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return dbus.Dictionary({
            "PlaybackStatus": dbus.String(state_log["mpris_status"], variant_level=1),
            "CanPlay": dbus.Boolean(True, variant_level=1),
            "CanPause": dbus.Boolean(True, variant_level=1),
            "CanGoNext": dbus.Boolean(True, variant_level=1),
            "CanGoPrevious": dbus.Boolean(True, variant_level=1),
            "Metadata": dbus.Dictionary({
                "xesam:title": dbus.String(state_log["mpris_title"], variant_level=1),
                "xesam:artist": dbus.Array([dbus.String("SunSoft Sound")], signature="s", variant_level=1),
            }, signature="sv", variant_level=1),
        }, signature="sv")

    @dbus.service.method("org.mpris.MediaPlayer2.Player", in_signature="", out_signature="")
    def PlayPause(self):
        state_log["mpris_calls"].append("PlayPause")
        state_log["mpris_status"] = "Playing" if state_log["mpris_status"] != "Playing" else "Paused"
        flush_log()

    @dbus.service.method("org.mpris.MediaPlayer2.Player", in_signature="", out_signature="")
    def Next(self):
        state_log["mpris_calls"].append("Next")
        state_log["mpris_title"] = "Motif Overture"
        flush_log()

    @dbus.service.method("org.mpris.MediaPlayer2.Player", in_signature="", out_signature="")
    def Previous(self):
        state_log["mpris_calls"].append("Previous")
        state_log["mpris_title"] = "Solaris Symphony"
        flush_log()


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
nm_name = dbus.service.BusName("org.freedesktop.NetworkManager", bus)
bz_name = dbus.service.BusName("org.bluez", bus)
mp_name = dbus.service.BusName("org.mpris.MediaPlayer2.cdedeck", bus)

nm_root = NmRoot(bus)
nm_am = NmAgentMgr(bus)
nm_set = NmSettings(bus)
nm_vpn = NmVpnConn(bus)
nm_avpn = NmActiveVpn(bus)
nm_dev = NmWifiDev(bus, nm_root)
ap1 = NmAp(bus, "/org/freedesktop/NetworkManager/AccessPoint/1", "Cafe_Open", 65, 0, 0)
ap2 = NmAp(bus, "/org/freedesktop/NetworkManager/AccessPoint/2", "Home_Fiber_5G", 92, 0x100, 0x100)
ap3 = NmAp(bus, "/org/freedesktop/NetworkManager/AccessPoint/3", "Corp_8021X", 84, 0x200, 0x200)

bz_root = BluezService(bus)
bz_am = BluezAgentMgr(bus)
bz_adp = BluezAdapter(bus, bz_root)
bz_dev = BluezDevice(bus, bz_root)
mpris = MprisPlayer(bus)

flush_log()
with open(READY_FILE, "w", encoding="utf-8") as rf:
    rf.write("ready\n")

GLib.MainLoop().run()
"""


def wait_for(pred, timeout: float = 8.0, interval: float = 0.08, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    last_err = None
    while time.monotonic() < deadline:
        try:
            val = pred()
            if val:
                return val
        except Exception as exc:  # noqa: BLE001
            last_err = exc
        time.sleep(interval)
    raise RuntimeError(f"Timed out waiting for {desc}" + (f" (last_err={last_err})" if last_err else ""))


def verify_proc_libs(pid: int, libs: tuple[str, ...], label: str) -> None:
    maps = Path(f"/proc/{pid}/maps").read_text(encoding="utf-8", errors="replace")
    for lib in libs:
        if lib not in maps:
            raise AssertionError(f"{label} (pid={pid}) does not map {lib}!\n{maps}")


def kill_procs(procs: list[subprocess.Popen]) -> None:
    for p in reversed(procs):
        if p.poll() is None:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
    time.sleep(0.15)
    for p in reversed(procs):
        if p.poll() is None:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                p.wait(timeout=2.0)
            except Exception:  # noqa: BLE001
                pass


def run_lane(
    lane: str,
    disp_idx: int,
    work_root: Path,
    comp_bin: Path,
    dtwm_x11_bin: Path,
    mixer_bin: Path,
    status_bin: Path,
    x11_helper: Path,
    tone_bin: Path,
    mock_py: Path,
) -> str:
    lane_root = work_root / lane
    home = lane_root / "home"
    xdg_rt = lane_root / "rt"
    for d in (home, xdg_rt, home / ".config", home / ".cache", home / ".local/share"):
        d.mkdir(parents=True, exist_ok=True)
    os.chmod(xdg_rt, 0o700)
    (home / ".Xdefaults").write_text("*fontList: fixed\n", encoding="utf-8")

    cde_build_root = comp_bin.parent.parent.parent
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = ":".join([
        "/var/tmp/ck-wayland-build/wp00/fs-prefix/lib",
        "/var/tmp/ck-wayland-build/wp00/motif-prefix/lib",
        "/usr/dt/lib",
        str(cde_build_root / "lib/DtSvc/.libs"),
        str(cde_build_root / "lib/DtWidget/.libs"),
        str(cde_build_root / "lib/tt/lib/.libs"),
        str(cde_build_root / "lib/DtXinerama/.libs"),
    ])
    env.update({
        "HOME": str(home),
        "XDG_RUNTIME_DIR": str(xdg_rt),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "XDG_DATA_HOME": str(home / ".local/share"),
        "CK_MIXER_NO_ALSA": "1",
        "CK_STATUS_SYSTEM_BUS": "session",
        "LC_ALL": "C.UTF-8",
        "LANG": "C.UTF-8",
    })
    env.pop("PULSE_SERVER", None)

    procs: list[subprocess.Popen] = []
    try:
        # 1. Isolated session D-Bus
        dbus_addr_file = lane_root / "dbus.addr"
        with dbus_addr_file.open("w", encoding="utf-8") as dfp:
            dbus_proc = subprocess.Popen(
                ["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                env=env, stdout=dfp, stderr=subprocess.DEVNULL, start_new_session=True,
            )
        procs.append(dbus_proc)
        wait_for(lambda: dbus_addr_file.is_file() and dbus_addr_file.read_text(encoding="utf-8").strip(),
                 timeout=6.0, desc=f"{lane} dbus-daemon")
        env["DBUS_SESSION_BUS_ADDRESS"] = dbus_addr_file.read_text(encoding="utf-8").splitlines()[0].strip()

        # 2. Isolated PipeWire + WirePlumber + PipeWire-Pulse
        for cmd in (["pipewire"], ["wireplumber"], ["pipewire-pulse"]):
            p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            procs.append(p)
            time.sleep(0.2)
        wait_for(lambda: subprocess.run(["pactl", "info"], env=env, capture_output=True).returncode == 0,
                 timeout=8.0, desc=f"{lane} pipewire-pulse ready")

        subprocess.run(["pactl", "load-module", "module-null-sink", "sink_name=desk_spk",
                        "sink_properties=device.description=Workstation_Speakers"], env=env, check=True, capture_output=True)
        subprocess.run(["pactl", "load-module", "module-null-sink", "sink_name=bt_a2dp",
                        "sink_properties=device.description=Bluetooth_Headphones"], env=env, check=True, capture_output=True)
        subprocess.run(["pactl", "load-module", "module-null-sink", "sink_name=mic_backing",
                        "sink_properties=device.description=Mic_Backing"], env=env, check=True, capture_output=True)
        subprocess.run(["pactl", "load-module", "module-remap-source", "master=mic_backing.monitor",
                        "source_name=usb_mic", "source_properties=device.description=USB_Studio_Microphone"],
                       env=env, check=True, capture_output=True)
        subprocess.run(["pactl", "set-default-sink", "desk_spk"], env=env, check=True)
        subprocess.run(["pactl", "set-default-source", "usb_mic"], env=env, check=True)

        tone_proc = subprocess.Popen([str(tone_bin)], env=env, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL, start_new_session=True)
        procs.append(tone_proc)

        # 3. Mock NetworkManager + BlueZ + MPRIS D-Bus service
        mock_state_file = lane_root / "mock_state.json"
        mock_ready_file = lane_root / "mock.ready"
        mock_proc = subprocess.Popen(
            ["python3", str(mock_py), str(mock_state_file), str(mock_ready_file)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, start_new_session=True,
        )
        procs.append(mock_proc)
        wait_for(lambda: mock_ready_file.is_file(), timeout=6.0, desc=f"{lane} mock D-Bus services")

        # 4. Start Wayland (`dtwm-wayland`) or X11 (`Xvfb` + `dtwm`)
        if lane == "wayland":
            status_file = lane_root / "comp.status"
            wenv = dict(env, WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1",
                        WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1")
            comp_proc = subprocess.Popen(
                [str(comp_bin), "--status-file", str(status_file)],
                env=wenv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            )
            procs.append(comp_proc)

            def wl_up():
                if not status_file.is_file():
                    return None
                kv = dict(ln.split("=", 1) for ln in status_file.read_text(encoding="utf-8").splitlines() if "=" in ln)
                return kv if kv.get("WAYLAND_DISPLAY") and kv.get("DISPLAY") else None

            kv = wait_for(wl_up, timeout=10.0, desc=f"{lane} dtwm-wayland")
            env["WAYLAND_DISPLAY"] = kv["WAYLAND_DISPLAY"]
            env["DISPLAY"] = kv["DISPLAY"]
        else:
            xdisp = f":{disp_idx}"
            xvfb_proc = subprocess.Popen(
                ["Xvfb", xdisp, "-screen", "0", "1280x900x24", "-nolisten", "tcp"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            )
            procs.append(xvfb_proc)
            env["DISPLAY"] = xdisp
            wait_for(lambda: subprocess.run([str(x11_helper), "ping"], env=env, capture_output=True).returncode == 0,
                     timeout=8.0, desc=f"{lane} Xvfb")
            dtwm_proc = subprocess.Popen(
                [str(dtwm_x11_bin)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True
            )
            procs.append(dtwm_proc)
            time.sleep(0.5)

        def get_json(win_cls: str, prop: str) -> dict:
            r = subprocess.run([str(x11_helper), "get", win_cls, prop], env=env, capture_output=True, text=True)
            if r.returncode != 0 or not r.stdout.strip():
                return {}
            return json.loads(r.stdout)

        def send_ipc(win_cls: str, prop: str, cmd: str) -> None:
            state_prop = "_CK_MIXER_STATE" if win_cls == "CkMixer" else "_CK_STATUS_STATE"
            subprocess.run([str(x11_helper), "set", state_prop, prop, cmd], env=env, check=True)

        # ---- PART A: Verify `ck-mixer` (PipeWire/PulseAudio, Zero-ALSA, Mic Peak, Stream & Profile Routing) ----
        mixer_proc = subprocess.Popen(
            [str(mixer_bin), "--tab", "inputs"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(mixer_proc)

        ms = wait_for(
            lambda: s if (s := get_json("CkMixer", "_CK_MIXER_STATE")).get("pulse_connected") else None,
            timeout=8.0, desc=f"{lane} ck-mixer _CK_MIXER_STATE",
        )
        verify_proc_libs(mixer_proc.pid, ("libXm.so.9", "libpulse.so.0"), f"{lane} ck-mixer")
        assert ms["alsa_devices"] == 0, f"{lane}: expected zero ALSA devices under CK_MIXER_NO_ALSA=1, got {ms}"

        # Live microphone peak meter must be > 10%
        ms = wait_for(
            lambda: s if (s := get_json("CkMixer", "_CK_MIXER_STATE")).get("mic_peak_pct", 0) >= 10 else None,
            timeout=6.0, desc=f"{lane} ck-mixer live mic peak >= 10%",
        )

        # Find usb_mic, desk_spk, bt_a2dp, and MusicStream indices
        bt_sink = next(x for x in ms["sinks"] if x["name"] == "bt_a2dp")
        usb_mic = next(x for x in ms["sources"] if x["name"] == "usb_mic")
        music_st = next(x for x in ms["sink_inputs"] if x["media"] == "MusicStream")

        # Mute usb_mic -> peak drops to 0; unmute -> peak recovers >= 10
        send_ipc("CkMixer", "_CK_MIXER_IPC", f"set-source-mute:{usb_mic['index']}:1")
        wait_for(
            lambda: get_json("CkMixer", "_CK_MIXER_STATE").get("mic_peak_pct", 99) == 0,
            timeout=5.0, desc=f"{lane} muted mic peak == 0%",
        )
        send_ipc("CkMixer", "_CK_MIXER_IPC", f"set-source-mute:{usb_mic['index']}:0")
        send_ipc("CkMixer", "_CK_MIXER_IPC", f"set-source-vol:{usb_mic['index']}:82")
        wait_for(
            lambda: (s := get_json("CkMixer", "_CK_MIXER_STATE")).get("mic_peak_pct", 0) >= 10
            and abs(next(x for x in s["sources"] if x["index"] == usb_mic["index"])["vol"] - 82) <= 2,
            timeout=5.0, desc=f"{lane} unmuted mic peak >= 10% & gain=82%",
        )

        # Switch Default Output Sink to bt_a2dp, set volume=68%, and route MusicStream to bt_a2dp at 55%
        send_ipc("CkMixer", "_CK_MIXER_IPC", "tab:outputs")
        send_ipc("CkMixer", "_CK_MIXER_IPC", "set-default-sink:bt_a2dp")
        send_ipc("CkMixer", "_CK_MIXER_IPC", f"set-sink-vol:{bt_sink['index']}:68")
        send_ipc("CkMixer", "_CK_MIXER_IPC", "tab:apps")
        send_ipc("CkMixer", "_CK_MIXER_IPC", f"move-sink-input:{music_st['index']}:{bt_sink['index']}")
        send_ipc("CkMixer", "_CK_MIXER_IPC", f"set-sink-input-vol:{music_st['index']}:55")

        wait_for(
            lambda: (s := get_json("CkMixer", "_CK_MIXER_STATE")).get("default_sink") == "bt_a2dp"
            and abs(next(x for x in s["sinks"] if x["index"] == bt_sink["index"])["vol"] - 68) <= 2
            and next(x for x in s["sink_inputs"] if x["index"] == music_st["index"])["sink"] == bt_sink["index"]
            and abs(next(x for x in s["sink_inputs"] if x["index"] == music_st["index"])["vol"] - 55) <= 2,
            timeout=6.0, desc=f"{lane} default_sink=bt_a2dp, vol=68%, MusicStream routed to bt_a2dp@55%",
        )

        # Verify PipeWire server agrees via pactl get-default-sink
        def_out = subprocess.run(["pactl", "get-default-sink"], env=env, capture_output=True, text=True, check=True).stdout.strip()
        assert def_out == "bt_a2dp", f"{lane}: pactl get-default-sink={def_out!r}"

        # Card profile switch if snd_dummy card is present in this isolated PipeWire session
        ms_cards = get_json("CkMixer", "_CK_MIXER_STATE").get("cards", [])
        if ms_cards:
            card0 = ms_cards[0]
            orig_prof = card0["active_profile"]
            send_ipc("CkMixer", "_CK_MIXER_IPC", "tab:profiles")
            send_ipc("CkMixer", "_CK_MIXER_IPC", f"set-card-profile:{card0['index']}:pro-audio")
            wait_for(
                lambda: get_json("CkMixer", "_CK_MIXER_STATE")["cards"][0]["active_profile"] == "pro-audio",
                timeout=5.0, desc=f"{lane} card profile -> pro-audio",
            )
            if orig_prof:
                send_ipc("CkMixer", "_CK_MIXER_IPC", f"set-card-profile:{card0['index']}:{orig_prof}")

        # ---- PART B: Verify `ck-status` (Wi-Fi PSK/802.1X, VPN + SecretAgent, BlueZ Agent1, MPRIS) ----
        status_proc = subprocess.Popen(
            [str(status_bin), "--standalone", "--tab", "network"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        procs.append(status_proc)

        ss = wait_for(
            lambda: s if len((s := get_json("CkStatus", "_CK_STATUS_STATE")).get("aps", [])) == 3 else None,
            timeout=8.0, desc=f"{lane} ck-status _CK_STATUS_STATE with 3 APs",
        )
        verify_proc_libs(status_proc.pid, ("libXm.so.9", "libdbus-1.so.3", "libpulse.so.0"), f"{lane} ck-status")

        mock_st = json.loads(mock_state_file.read_text(encoding="utf-8"))
        assert mock_st["nm_agent_registered"] and mock_st["bt_agent_registered"], f"{lane}: agents not registered: {mock_st}"

        # 1. WPA2-PSK Onboarding (`Home_Fiber_5G`, index 1)
        ap_names = [a["ssid"] for a in ss["aps"]]
        psk_idx = ap_names.index("Home_Fiber_5G")
        send_ipc("CkStatus", "_CK_STATUS_IPC", f"wifi-select-connect:{psk_idx}")
        wait_for(lambda: get_json("CkStatus", "_CK_STATUS_STATE").get("wifi_dlg_open") is True,
                 timeout=5.0, desc=f"{lane} WPA2-PSK onboarding dialog open")
        send_ipc("CkStatus", "_CK_STATUS_IPC", "wifi-dlg-submit-psk:cde-wayland-2026")
        wait_for(
            lambda: any(w.get("id") == "Home_Fiber_5G" and w.get("psk") == "cde-wayland-2026"
                        for w in json.loads(mock_state_file.read_text(encoding="utf-8"))["added_wifi"])
            and get_json("CkStatus", "_CK_STATUS_STATE").get("active_ssid") == "Home_Fiber_5G",
            timeout=5.0, desc=f"{lane} WPA2-PSK AddAndActivateConnection verified",
        )

        # 2. WPA-Enterprise 802.1X Onboarding (`Corp_8021X`)
        eap_idx = ap_names.index("Corp_8021X")
        send_ipc("CkStatus", "_CK_STATUS_IPC", f"wifi-select-connect:{eap_idx}")
        wait_for(lambda: get_json("CkStatus", "_CK_STATUS_STATE").get("wifi_dlg_open") is True,
                 timeout=5.0, desc=f"{lane} WPA-Enterprise onboarding dialog open")
        send_ipc("CkStatus", "_CK_STATUS_IPC", "wifi-dlg-submit-eap:peap:mschapv2:alice@corp.example:ent-pass-77")
        wait_for(
            lambda: any(w.get("id") == "Corp_8021X" and w.get("key_mgmt") == "wpa-eap"
                        and w.get("eap") == ["peap"] and w.get("phase2") == "mschapv2"
                        and w.get("identity") == "alice@corp.example" and w.get("password") == "ent-pass-77"
                        and w.get("system_ca") is True
                        for w in json.loads(mock_state_file.read_text(encoding="utf-8"))["added_wifi"]),
            timeout=5.0, desc=f"{lane} WPA-Enterprise 802.1X AddAndActivateConnection verified",
        )

        # 3. VPN Activation + Asynchronous NetworkManager SecretAgent.GetSecrets Dialog
        send_ipc("CkStatus", "_CK_STATUS_IPC", "vpn-connect:0")
        wait_for(
            lambda: (s := get_json("CkStatus", "_CK_STATUS_STATE")).get("secret_dlg_open") is True
            and s.get("secret_conn") == "Corp_WireGuard_VPN",
            timeout=5.0, desc=f"{lane} SecretAgent dialog opened for Corp_WireGuard_VPN",
        )
        send_ipc("CkStatus", "_CK_STATUS_IPC", "secret-submit:vpn-token-4455")
        wait_for(
            lambda: json.loads(mock_state_file.read_text(encoding="utf-8")).get("last_vpn_secret") == "vpn-token-4455"
            and get_json("CkStatus", "_CK_STATUS_STATE")["vpns"][0]["active"] is True,
            timeout=5.0, desc=f"{lane} VPN SecretAgent reply & active state verified",
        )

        # 4. BlueZ Discovery + org.bluez.Agent1 RequestConfirmation(614829) + Trust + Forget
        send_ipc("CkStatus", "_CK_STATUS_IPC", "tab:bluetooth")
        send_ipc("CkStatus", "_CK_STATUS_IPC", "bt-scan:1")
        wait_for(
            lambda: (s := get_json("CkStatus", "_CK_STATUS_STATE")).get("bt_discovering") is True
            and len(s.get("bt_devices", [])) == 1
            and s["bt_devices"][0]["name"] == "Sony WH-1000XM5",
            timeout=5.0, desc=f"{lane} Bluetooth StartDiscovery discovered Sony WH-1000XM5",
        )
        send_ipc("CkStatus", "_CK_STATUS_IPC", "bt-pair:0")
        wait_for(
            lambda: (s := get_json("CkStatus", "_CK_STATUS_STATE")).get("bt_dlg_open") is True
            and s.get("bt_dlg_passkey") == 614829,
            timeout=5.0, desc=f"{lane} BlueZ Agent1 RequestConfirmation(614829) modal open",
        )
        send_ipc("CkStatus", "_CK_STATUS_IPC", "bt-agent-accept")
        wait_for(
            lambda: (d := get_json("CkStatus", "_CK_STATUS_STATE")["bt_devices"][0])["paired"] is True
            and d["connected"] is True,
            timeout=5.0, desc=f"{lane} BlueZ Agent1 Pair confirmed & connected",
        )
        send_ipc("CkStatus", "_CK_STATUS_IPC", "bt-trust:0")
        wait_for(
            lambda: get_json("CkStatus", "_CK_STATUS_STATE")["bt_devices"][0]["trusted"] is True,
            timeout=5.0, desc=f"{lane} Bluetooth Trusted=True",
        )
        send_ipc("CkStatus", "_CK_STATUS_IPC", "bt-forget:0")
        wait_for(
            lambda: len(get_json("CkStatus", "_CK_STATUS_STATE").get("bt_devices", [])) == 0
            and json.loads(mock_state_file.read_text(encoding="utf-8"))["bt_removed"] is True,
            timeout=5.0, desc=f"{lane} Bluetooth RemoveDevice verified",
        )

        # 5. MPRIS Media Transport (`PlayPause` + `Next`)
        send_ipc("CkStatus", "_CK_STATUS_IPC", "tab:overview")
        wait_for(
            lambda: (s := get_json("CkStatus", "_CK_STATUS_STATE")).get("mpris_available") is True
            and s.get("mpris_title") == "Solaris Symphony",
            timeout=5.0, desc=f"{lane} MPRIS initial track metadata",
        )
        send_ipc("CkStatus", "_CK_STATUS_IPC", "mpris:PlayPause")
        send_ipc("CkStatus", "_CK_STATUS_IPC", "mpris:Next")
        wait_for(
            lambda: (s := get_json("CkStatus", "_CK_STATUS_STATE")).get("mpris_status") == "Playing"
            and s.get("mpris_title") == "Motif Overture",
            timeout=5.0, desc=f"{lane} MPRIS PlayPause + Next verified",
        )

        return f"{lane}: PASS (PipeWire mixer/mic-peak/routing + Wi-Fi PSK/802.1X + VPN SecretAgent + BlueZ Agent1 + MPRIS)"
    finally:
        kill_procs(procs)


def resolve_app_bin(explicit: str | None, name: str) -> Path:
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    candidates.append(Path(f"/var/tmp/ck-wayland-build/wp00/apps-build/bin/{name}"))
    w = shutil.which(name)
    if w:
        candidates.append(Path(w))
    candidates.append(Path(f"/usr/dt/bin/{name}"))
    candidates.append(Path(f"/usr/bin/{name}"))
    for c in candidates:
        if c.is_file():
            return c.resolve()
    raise SystemExit(f"missing required binary: {name}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compositor", default="/var/tmp/ck-wayland-build/wp00/cde-build/programs/dtwm/dtwm-wayland")
    ap.add_argument("--dtwm-x11", default="")
    ap.add_argument("--ck-mixer", default="")
    ap.add_argument("--ck-status", default="")
    args = ap.parse_args()

    comp_bin = Path(args.compositor).resolve()
    dtwm_x11_bin = Path(args.dtwm_x11).resolve() if args.dtwm_x11 else (comp_bin.parent / "dtwm").resolve()
    if not dtwm_x11_bin.is_file():
        dtwm_x11_bin = Path("/usr/dt/bin/dtwm")
    mixer_bin = resolve_app_bin(args.ck_mixer or None, "ck-mixer")
    status_bin = resolve_app_bin(args.ck_status or None, "ck-status")

    cc = os.environ.get("CC", "cc").split()
    tmp_parent = os.environ.get("TMPDIR", "/var/tmp")
    with tempfile.TemporaryDirectory(prefix="wp15-dual-", dir=tmp_parent) as td:
        root = Path(td)
        x11_c = root / "x11_prop_helper.c"
        x11_bin = root / "x11_prop_helper"
        x11_c.write_text(X11_PROP_HELPER_C, encoding="utf-8")
        subprocess.run(cc + ["-O2", str(x11_c), "-o", str(x11_bin), "-lX11"], check=True)

        tone_c = root / "pulse_tone_gen.c"
        tone_bin = root / "pulse_tone_gen"
        tone_c.write_text(PULSE_TONE_GEN_C, encoding="utf-8")
        pulse_cflags = subprocess.check_output(["pkg-config", "--cflags", "libpulse"], text=True).split()
        pulse_libs = subprocess.check_output(["pkg-config", "--libs", "libpulse"], text=True).split()
        subprocess.run(cc + ["-O2", str(tone_c), "-o", str(tone_bin)] + pulse_cflags + pulse_libs + ["-lm"], check=True)

        mock_py = root / "mock_services.py"
        mock_py.write_text(MOCK_SYSTEM_SERVICES_PY, encoding="utf-8")

        with ThreadPoolExecutor(max_workers=2) as pool:
            f_wl = pool.submit(
                run_lane, "wayland", 94, root,
                comp_bin, dtwm_x11_bin,
                mixer_bin, status_bin,
                x11_bin, tone_bin, mock_py,
            )
            f_x11 = pool.submit(
                run_lane, "x11", 95, root,
                comp_bin, dtwm_x11_bin,
                mixer_bin, status_bin,
                x11_bin, tone_bin, mock_py,
            )
            res_wl = f_wl.result()
            res_x11 = f_x11.result()

        print(res_wl)
        print(res_x11)
    print("WP-15 parallel Wayland + X11 verification: ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
