from datetime import datetime, timedelta, time as dtime
from typing import Optional
from zoneinfo import ZoneInfo
import threading
import time as time_module

from flask import Flask, jsonify, render_template, request


tz = ZoneInfo("America/New_York")

app = Flask(__name__)

state_lock = threading.Lock()
trigger_flag = False
alarm_time_str = "07:00"  # default daily alarm time
next_alarm_at: Optional[datetime] = None
events = []  # list of {ts: datetime, kind: str}


def _compute_next_alarm(now: Optional[datetime] = None) -> datetime:
    """Return the next datetime the alarm should fire after `now`."""
    current = now or datetime.now(tz)
    hour, minute = map(int, alarm_time_str.split(":"))
    candidate = datetime.combine(current.date(), dtime(hour=hour, minute=minute), tz)
    if candidate <= current:
        candidate += timedelta(days=1)
    return candidate


def _prev_alarm_reference(now: datetime) -> datetime:
    """Return the most recent scheduled alarm time before `now`."""
    hour, minute = map(int, alarm_time_str.split(":"))
    candidate = datetime.combine(now.date(), dtime(hour=hour, minute=minute), tz)
    if candidate > now:
        candidate -= timedelta(days=1)
    return candidate


def _log_event(kind: str) -> None:
    with state_lock:
        events.append({"ts": datetime.now(tz), "kind": kind})


def _count_since(delta: timedelta) -> int:
    cutoff = datetime.now(tz) - delta
    with state_lock:
        return sum(1 for e in events if e["ts"] >= cutoff)


def _hourly_series(hours: int = 24):
    now = datetime.now(tz).replace(minute=0, second=0, microsecond=0)
    labels = []
    counts = []
    with state_lock:
        for i in range(hours):
            bucket_start = now - timedelta(hours=hours - i - 1)
            bucket_end = bucket_start + timedelta(hours=1)
            labels.append(bucket_start.strftime("%I %p").lstrip("0"))
            counts.append(sum(1 for e in events if bucket_start <= e["ts"] < bucket_end))
    return labels, counts


def _ensure_next_alarm():
    global next_alarm_at
    with state_lock:
        next_alarm_at = _compute_next_alarm()


def _scheduler_loop():
    global trigger_flag, next_alarm_at
    while True:
        with state_lock:
            now = datetime.now(tz)
            if next_alarm_at and now >= next_alarm_at:
                trigger_flag = True
                events.append({"ts": now, "kind": "alarm"})
                next_alarm_at = _compute_next_alarm(now + timedelta(seconds=1))
        time_module.sleep(1)


# Start the scheduler thread once
_ensure_next_alarm()
threading.Thread(target=_scheduler_loop, daemon=True).start()


@app.route("/")
def home():
    return render_template("index.html")


@app.route("/api/state", methods=["GET"])
def api_state():
    now = datetime.now(tz)
    with state_lock:
        trigger = trigger_flag
        next_fire = next_alarm_at

    if next_fire:
        seconds_left = max(0, int((next_fire - now).total_seconds()))
        prev_fire = _prev_alarm_reference(now)
        window = (next_fire - prev_fire).total_seconds()
        progress = 1 - min(max((next_fire - now).total_seconds() / window, 0), 1)
    else:
        seconds_left = None
        progress = 0

    labels, counts = _hourly_series(24)

    return jsonify(
        {
            "trigger": trigger,
            "alarm_time": alarm_time_str,
            "next_fire_iso": next_fire.isoformat() if next_fire else None,
            "seconds_left": seconds_left,
            "ring_progress": progress,  # 0.0 -> full, 1.0 -> empty
            "counts": {
                "24h": _count_since(timedelta(hours=24)),
                "7d": _count_since(timedelta(days=7)),
                "30d": _count_since(timedelta(days=30)),
            },
            "series": {"labels": labels, "counts": counts},
            "server_time": now.isoformat(),
        }
    )


@app.route("/api/alarm", methods=["POST"])
def api_alarm():
    global alarm_time_str, next_alarm_at
    data = request.get_json(silent=True) or {}
    new_time = data.get("time")
    if not isinstance(new_time, str) or len(new_time.split(":")) != 2:
        return jsonify({"error": "time must be HH:MM"}), 400

    try:
        hour, minute = map(int, new_time.split(":"))
        if not (0 <= hour < 24 and 0 <= minute < 60):
            raise ValueError
    except ValueError:
        return jsonify({"error": "invalid time"}), 400

    with state_lock:
        alarm_time_str = f"{hour:02d}:{minute:02d}"
        next_alarm_at = _compute_next_alarm()

    return jsonify({"alarm_time": alarm_time_str, "next_fire_iso": next_alarm_at.isoformat()})


@app.route("/trigger", methods=["POST"])
def api_trigger():
    global trigger_flag
    with state_lock:
        trigger_flag = True
        events.append({"ts": datetime.now(tz), "kind": "manual"})
    return jsonify({"status": "trigger armed"})


@app.route("/status", methods=["GET"])
def status():
    with state_lock:
        return jsonify({"trigger": trigger_flag})


@app.route("/check", methods=["GET"])
def check_trigger():
    global trigger_flag
    with state_lock:
        value = trigger_flag
        trigger_flag = False
    return jsonify({"trigger": value})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=10000, debug=True, use_reloader=False)
