
import os
import threading

import paho.mqtt.client as mqtt
from dotenv import load_dotenv
from flask import Flask, jsonify, render_template

# PASS.env chỉ dùng ở local.
# Trên Render, cấu hình biến môi trường trong Dashboard.
load_dotenv("PASS.env")

app = Flask(__name__)

MQTT_HOST = os.getenv("MQTT_HOST")
MQTT_PORT = int(os.getenv("MQTT_PORT", "8883"))
MQTT_USER = os.getenv("MQTT_USER")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD")

# Đặt MQTT_TLS=true nếu dùng MQTT qua TLS.
MQTT_TLS = os.getenv("MQTT_TLS", "true").lower() == "true"

TOPIC_SET = "home/led1/set"
TOPIC_STATE = "home/led1/state"

led_state = "UNKNOWN"
mqtt_connected = False

state_lock = threading.Lock()
mqtt_start_lock = threading.Lock()
mqtt_started = False
client = None


def on_connect(client, userdata, flags, reason_code, properties):
    global mqtt_connected

    if reason_code == 0:
        with state_lock:
            mqtt_connected = True

        print("Connected to MQTT broker")

        result, _ = client.subscribe(TOPIC_STATE, qos=1)
        if result != mqtt.MQTT_ERR_SUCCESS:
            print(f"Subscribe request failed: {result}")
    else:
        with state_lock:
            mqtt_connected = False

        print(f"MQTT connection failed: {reason_code}")


def on_disconnect(client, userdata, disconnect_flags, reason_code, properties):
    global mqtt_connected

    with state_lock:
        mqtt_connected = False

    print(f"Disconnected from MQTT broker: {reason_code}")


def on_message(client, userdata, msg):
    global led_state

    if msg.topic != TOPIC_STATE:
        return

    received = msg.payload.decode(
        "utf-8", errors="replace"
    ).strip().upper()

    if received in ("ON", "OFF"):
        with state_lock:
            led_state = received

        print(f"ESP32 reported LED state: {received}")


def start_mqtt():
    """Start one MQTT client for this application process."""
    global client, mqtt_started

    with mqtt_start_lock:
        if mqtt_started:
            return

        if not MQTT_HOST:
            raise RuntimeError("MQTT_HOST is not configured")

        if not MQTT_USER or not MQTT_PASSWORD:
            raise RuntimeError("MQTT credentials are not configured")

        mqtt_client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="esp32_web_dashboard",
        )

        mqtt_client.username_pw_set(MQTT_USER, MQTT_PASSWORD)

        if MQTT_TLS:
            mqtt_client.tls_set()

        mqtt_client.on_connect = on_connect
        mqtt_client.on_disconnect = on_disconnect
        mqtt_client.on_message = on_message

        # loop_start() handles the network loop and reconnection.
        mqtt_client.reconnect_delay_set(
            min_delay=1,
            max_delay=30,
        )

        client = mqtt_client
        mqtt_started = True

        try:
            client.connect(
                MQTT_HOST,
                MQTT_PORT,
                keepalive=60,
            )
            client.loop_start()
        except Exception:
            mqtt_started = False
            client = None
            raise


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/health")
def health():
    # Render can use this endpoint to check HTTP availability.
    return jsonify({"status": "ok"}), 200


@app.route("/api/state")
def get_state():
    with state_lock:
        state = led_state
        connected = mqtt_connected

    return jsonify({
        "led": state,
        "mqtt_connected": connected,
    })


@app.route("/api/led/<command>", methods=["POST"])
def set_led(command):
    command = command.upper()

    if command not in ("ON", "OFF"):
        return jsonify({
            "error": "Command must be ON or OFF"
        }), 400

    with state_lock:
        connected = mqtt_connected

    if not connected or client is None:
        return jsonify({
            "error": "MQTT broker is disconnected"
        }), 503

    result = client.publish(
        TOPIC_SET,
        command,
        qos=1,
        retain=False,
    )

    if result.rc != mqtt.MQTT_ERR_SUCCESS:
        return jsonify({
            "error": "Could not publish MQTT command"
        }), 503

    return jsonify({
        "sent": command,
        "message": "Command sent; waiting for ESP32 state",
    }), 200


# IMPORTANT: runs when Gunicorn imports app.py.
# This implementation assumes one Gunicorn worker.
start_mqtt()


if __name__ == "__main__":
    app.run(
        host="0.0.0.0",
        port=int(os.getenv("PORT", "5000")),
        debug=False,
    )
