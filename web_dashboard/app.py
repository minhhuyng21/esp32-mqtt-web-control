import hmac
import os
import threading
import time
import uuid

import paho.mqtt.client as mqtt
from dotenv import load_dotenv
from flask import Flask, jsonify, render_template, request, session

# PASS.env is for local development. On Render, set variables in the service dashboard.
load_dotenv("PASS.env")

app = Flask(__name__)
app.config.update(
    # Set a private, random SECRET_KEY in PASS.env / Render environment variables.
    SECRET_KEY=os.getenv("SECRET_KEY", "local-development-only-change-this"),
    SESSION_COOKIE_HTTPONLY=True,
    SESSION_COOKIE_SAMESITE="Lax",
    SESSION_COOKIE_SECURE=os.getenv("SESSION_COOKIE_SECURE", "false").lower() == "true",
)

# Dashboard password: set CONTROL_PASSWORD in PASS.env or the Render environment.
CONTROL_PASSWORD = os.getenv("CONTROL_PASSWORD", "")

MQTT_HOST = os.getenv("MQTT_HOST")
MQTT_PORT = int(os.getenv("MQTT_PORT", "8883"))
MQTT_USER = os.getenv("MQTT_USER")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD")
MQTT_TLS = os.getenv("MQTT_TLS", "true").lower() == "true"
MQTT_ENV = os.getenv("MQTT_ENV", "local")  # Set MQTT_ENV=render on Render.
MQTT_DEBUG = os.getenv("MQTT_DEBUG", "false").lower() == "true"

TOPIC_SET = "home/led1/set"
TOPIC_STATE = "home/led1/state"

# Shared state
state_lock = threading.Lock()
mqtt_start_lock = threading.Lock()

led_state = "UNKNOWN"
mqtt_connected = False
connect_count = 0
disconnect_count = 0
last_event = ""
mqtt_error = None

client = None
client_pid = None  # Prevent reusing a Paho client created in a different process.


# MQTT callbacks
def on_connect(client, userdata, flags, reason_code, properties):
    global mqtt_connected, connect_count, last_event

    if reason_code == 0:
        with state_lock:
            mqtt_connected = True
            connect_count += 1
            last_event = f"connected {time.strftime('%H:%M:%S')}"

        print(f"[{os.getpid()}] Connected to MQTT broker", flush=True)

        result, _ = client.subscribe(TOPIC_STATE, qos=1)
        if result != mqtt.MQTT_ERR_SUCCESS:
            print(f"Subscribe request failed: {result}", flush=True)
    else:
        with state_lock:
            mqtt_connected = False
            last_event = f"connect failed {time.strftime('%H:%M:%S')} {reason_code!r}"

        print(f"MQTT connection failed: {reason_code}", flush=True)


def on_disconnect(client, userdata, disconnect_flags, reason_code, properties):
    global mqtt_connected, disconnect_count, last_event

    with state_lock:
        mqtt_connected = False
        disconnect_count += 1
        last_event = f"disconnected {time.strftime('%H:%M:%S')} {reason_code!r}"

    print(f"[{os.getpid()}] MQTT disconnect: {reason_code!r}", flush=True)


def on_connect_fail(client, userdata):
    print(
        f"MQTT TCP connection failed: host={MQTT_HOST}, "
        f"port={MQTT_PORT}, TLS={MQTT_TLS}",
        flush=True,
    )


def on_message(client, userdata, msg):
    global led_state

    if msg.topic != TOPIC_STATE:
        return

    received = msg.payload.decode("utf-8", errors="replace").strip().upper()
    if received in ("ON", "OFF"):
        with state_lock:
            led_state = received
        print(f"ESP32 reported LED state: {received}", flush=True)


def on_mqtt_log(client, userdata, level, buf):
    if MQTT_DEBUG or level in (mqtt.MQTT_LOG_ERR, mqtt.MQTT_LOG_WARNING):
        print(f"[pid {os.getpid()}] [MQTT:{level}] {buf}", flush=True)


# Start one MQTT client per worker process.
def start_mqtt():
    global client, client_pid, mqtt_connected, mqtt_error

    pid = os.getpid()

    with mqtt_start_lock:
        if client_pid == pid:
            return

        if not MQTT_HOST:
            raise RuntimeError("MQTT_HOST is not configured")
        if not MQTT_USER or not MQTT_PASSWORD:
            raise RuntimeError("MQTT credentials are not configured")

        print(
            f"[{pid}] Starting MQTT: host={MQTT_HOST}, "
            f"port={MQTT_PORT}, TLS={MQTT_TLS}",
            flush=True,
        )

        with state_lock:
            mqtt_connected = False

        mqtt_client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"web-dashboard-{MQTT_ENV}-{uuid.uuid4().hex[:6]}",
        )
        mqtt_client.username_pw_set(MQTT_USER, MQTT_PASSWORD)

        if MQTT_TLS:
            mqtt_client.tls_set()

        mqtt_client.on_connect = on_connect
        mqtt_client.on_disconnect = on_disconnect
        mqtt_client.on_connect_fail = on_connect_fail
        mqtt_client.on_message = on_message
        mqtt_client.on_log = on_mqtt_log
        mqtt_client.reconnect_delay_set(min_delay=1, max_delay=30)

        # Connect asynchronously; Paho retries if the network is temporarily unavailable.
        mqtt_client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=60)
        mqtt_client.loop_start()

        client = mqtt_client
        client_pid = pid
        mqtt_error = None


@app.before_request
def ensure_mqtt():
    """Ensure the current worker process has an MQTT client."""
    global mqtt_error

    if client_pid == os.getpid():
        return

    try:
        start_mqtt()
    except Exception as exc:
        mqtt_error = str(exc)
        print(f"start_mqtt failed: {exc}", flush=True)


@app.after_request
def no_cache(response):
    response.headers["Cache-Control"] = "no-store"
    return response


# Website routes
@app.route("/")
def index():
    return render_template("index.html")


@app.route("/health")
def health():
    return jsonify({"status": "ok"}), 200


@app.route("/api/check-password", methods=["POST"])
def check_password():
    """Check the entered password and remember the result in this browser session."""
    if not CONTROL_PASSWORD:
        return jsonify({"ok": False, "error": "CONTROL_PASSWORD is not configured"}), 500

    payload = request.get_json(silent=True) or {}
    submitted_password = payload.get("password", "")

    ok = (
        isinstance(submitted_password, str)
        and hmac.compare_digest(submitted_password, CONTROL_PASSWORD)
    )

    if ok:
        session["control_authorized"] = True
    else:
        # A wrong password returns this browser session to the locked state.
        session.pop("control_authorized", None)

    return jsonify({"ok": ok}), 200


@app.route("/api/state")
def get_state():
    with state_lock:
        current_client = client
        raw_client_id = getattr(current_client, "_client_id", None) if current_client else None
        if isinstance(raw_client_id, bytes):
            displayed_client_id = raw_client_id.decode("utf-8", errors="replace")
        elif raw_client_id is not None:
            displayed_client_id = str(raw_client_id)
        else:
            displayed_client_id = None

        data = {
            "led": led_state,
            "mqtt_connected": mqtt_connected,
            "control_authorized": bool(session.get("control_authorized", False)),
            # Useful debug details; these can be removed after everything works.
            "pid": os.getpid(),
            "client_id": displayed_client_id,
            "lib_connected": current_client.is_connected() if current_client else False,
            "connect_count": connect_count,
            "disconnect_count": disconnect_count,
            "last_event": last_event,
            "error": mqtt_error,
        }

    return jsonify(data)


@app.route("/api/led/<command>", methods=["POST"])
def set_led(command):
    command = command.upper()

    if command not in ("ON", "OFF"):
        return jsonify({"error": "Command must be ON or OFF"}), 400

    # The server checks authorization too; disabling buttons in HTML is not security by itself.
    if not session.get("control_authorized", False):
        return jsonify({"error": "Enter the correct password first"}), 401

    with state_lock:
        connected = mqtt_connected
        current_client = client

    if not connected or current_client is None:
        return jsonify({"error": "MQTT broker is disconnected"}), 503

    result = current_client.publish(TOPIC_SET, command, qos=1, retain=False)

    if result.rc != mqtt.MQTT_ERR_SUCCESS:
        return jsonify({"error": "Could not publish MQTT command"}), 503

    return jsonify({
        "sent": command,
        "message": "Command sent; waiting for ESP32 state",
    }), 200


if __name__ == "__main__":
    # Start MQTT immediately for local development.
    try:
        start_mqtt()
    except Exception as exc:
        mqtt_error = str(exc)
        print(f"Initial MQTT startup failed: {exc}", flush=True)

    app.run(
        host="0.0.0.0",
        port=int(os.getenv("PORT", "5000")),
        debug=False,
    )
