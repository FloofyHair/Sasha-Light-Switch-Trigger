from datetime import datetime, timedelta, time as dtime
from typing import Optional
from zoneinfo import ZoneInfo
import threading

from flask import Flask, jsonify, render_template, request

tz = ZoneInfo("America/New_York")
app = Flask(__name__)

state_lock = threading.Lock()

# Last state reported by the microcontroller on each check-in.
# The device is the source of truth; this is just what we last heard.
reported = {
    "alarm_time":      "07:00",
    "trigger_count":   0,
    "last_trigger_iso": None,
    "last_checkin_iso": None,
}

# Commands queued for delivery on the next check-in response.
pending_alarm:   Optional[str] = None   # new alarm to push to device
pending_trigger: bool          = False   # manual trigger queued for device

# Per-trigger event log for the activity chart (pruned to 30 days).
events = []  # list of {"ts": datetime, "kind": str}


# ── Helpers ───────────────────────────────────────────────────────────────────

def _prune_events() -> None:
    """Drop events older than 30 days. Must be called with state_lock held."""
    cutoff = datetime.now(tz) - timedelta(days=30)
    while events and events[0]["ts"] < cutoff:
        events.pop(0)


def _count_since(delta: timedelta) -> int:
    cutoff = datetime.now(tz) - delta
    with state_lock:
        return sum(1 for e in events if e["ts"] >= cutoff)


def _hourly_series():
    now = datetime.now(tz).replace(minute=0, second=0, microsecond=0)
    labels, counts = [], []
    with state_lock:
        for i in range(24):
            start = now - timedelta(hours=23 - i)
            end   = start + timedelta(hours=1)
            labels.append(start.strftime("%I %p").lstrip("0"))
            counts.append(sum(1 for e in events if start <= e["ts"] < end))
    return labels, counts


# ── Routes ────────────────────────────────────────────────────────────────────

@app.route("/")
def home():
    return render_template("index.html")


@app.route("/checkin", methods=["POST"])
def checkin():
    """
    ESP32 reports its current state; server returns any pending commands.

    Request  JSON: { alarm_time, trigger_count, last_trigger_iso? }
    Response JSON: { alarm?: "HH:MM", trigger?: true }  — fields omitted when not needed
    """
    global pending_alarm, pending_trigger

    data     = request.get_json(silent=True) or {}
    response = {}

    with state_lock:
        now = datetime.now(tz)

        # Log new triggers detected via count increase so the chart stays accurate
        new_count = int(data.get("trigger_count", reported["trigger_count"]))
        delta     = new_count - reported["trigger_count"]
        if delta > 0:
            for _ in range(delta):
                events.append({"ts": now, "kind": "device"})
            _prune_events()

        # Accept device-reported state
        if "alarm_time" in data:
            reported["alarm_time"] = data["alarm_time"]
        reported["trigger_count"]   = new_count
        if "last_trigger_iso" in data:
            reported["last_trigger_iso"] = data["last_trigger_iso"]
        reported["last_checkin_iso"] = now.isoformat()

        # Push pending alarm override (user changed it on the web UI)
        if pending_alarm is not None:
            response["alarm"]       = pending_alarm
            reported["alarm_time"]  = pending_alarm   # optimistic update for display
            pending_alarm           = None

        # Push pending manual trigger
        if pending_trigger:
            response["trigger"] = True
            events.append({"ts": now, "kind": "manual"})
            _prune_events()
            pending_trigger = False

    return jsonify(response)


@app.route("/api/state", methods=["GET"])
def api_state():
    now = datetime.now(tz)

    with state_lock:
        snap                = dict(reported)
        has_pending_alarm   = pending_alarm is not None
        has_pending_trigger = pending_trigger

    # Compute next scheduled fire time from the device-reported alarm for the UI countdown
    try:
        hour, minute = map(int, snap["alarm_time"].split(":"))
        candidate    = datetime.combine(now.date(), dtime(hour=hour, minute=minute), tz)
        if candidate <= now:
            candidate += timedelta(days=1)
        next_fire    = candidate
        seconds_left = max(0, int((next_fire - now).total_seconds()))
    except Exception:
        next_fire    = None
        seconds_left = None

    labels, counts = _hourly_series()

    return jsonify({
        "alarm_time":       snap["alarm_time"],
        "trigger_count":    snap["trigger_count"],
        "last_trigger_iso": snap["last_trigger_iso"],
        "last_checkin_iso": snap["last_checkin_iso"],
        "next_fire_iso":    next_fire.isoformat() if next_fire else None,
        "seconds_left":     seconds_left,
        "pending_alarm":    has_pending_alarm,
        "pending_trigger":  has_pending_trigger,
        "counts": {
            "24h": _count_since(timedelta(hours=24)),
            "7d":  _count_since(timedelta(days=7)),
            "30d": _count_since(timedelta(days=30)),
        },
        "series":      {"labels": labels, "counts": counts},
        "server_time": now.isoformat(),
    })


@app.route("/api/alarm", methods=["POST"])
def api_alarm():
    global pending_alarm
    data = request.get_json(silent=True) or {}
    raw  = data.get("time", "")
    parts = raw.split(":")
    if len(parts) != 2:
        return jsonify({"error": "time must be HH:MM"}), 400
    try:
        hour, minute = int(parts[0]), int(parts[1])
        if not (0 <= hour < 24 and 0 <= minute < 60):
            raise ValueError
    except ValueError:
        return jsonify({"error": "invalid time"}), 400

    normalized = f"{hour:02d}:{minute:02d}"
    with state_lock:
        pending_alarm = normalized
    return jsonify({"alarm_time": normalized, "status": "pending"})


@app.route("/trigger", methods=["POST"])
def api_trigger():
    global pending_trigger
    with state_lock:
        pending_trigger = True
    return jsonify({"status": "queued"})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=10000, debug=True, use_reloader=False)
