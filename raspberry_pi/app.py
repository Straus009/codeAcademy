"""Servidor web para controlar el sistema de bombas y ventilación.

Proporciona una API REST y un canal en tiempo real mediante Socket.IO
para visualizar lecturas y actuar sobre los relevos desde cualquier
navegador conectado a la misma red que la Raspberry Pi.
"""
from __future__ import annotations

import logging
import os
from typing import Any, Dict

from flask import Flask, jsonify, render_template, request
from flask_socketio import SocketIO, emit

from arduino_bridge import ArduinoBridge

logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"))
LOGGER = logging.getLogger(__name__)


def create_app() -> Flask:
    app = Flask(__name__, static_folder="static", template_folder="templates")
    app.config["SECRET_KEY"] = os.getenv("FLASK_SECRET", "mega-bambu-control")

    socketio = SocketIO(app, cors_allowed_origins="*")

    port = os.getenv("ARDUINO_PORT", "/dev/ttyACM0")
    baudrate = int(os.getenv("ARDUINO_BAUDRATE", "115200"))
    simulate = os.getenv("ARDUINO_SIMULATE", "0") == "1"
    bridge = ArduinoBridge(port=port, baudrate=baudrate, simulate=simulate)

    @bridge.on_update
    def _broadcast_state(state) -> None:
        socketio.emit("state", state.to_dict())

    @app.route("/")
    def index() -> str:
        return render_template("index.html")

    @app.get("/api/state")
    def api_state():
        bridge.request_state()
        return jsonify(bridge.state.to_dict())

    def _validate_payload(payload: Dict[str, Any]) -> Dict[str, Any]:
        if not isinstance(payload, dict):
            raise ValueError("El cuerpo debe ser JSON válido")
        if "state" not in payload:
            raise ValueError("El JSON debe incluir la clave 'state'")
        state_value = payload["state"]
        if isinstance(state_value, bool):
            return payload
        if isinstance(state_value, (int, float)):
            payload["state"] = bool(state_value)
            return payload
        if isinstance(state_value, str):
            lowered = state_value.strip().lower()
            if lowered in {"1", "true", "on", "encendido"}:
                payload["state"] = True
                return payload
            if lowered in {"0", "false", "off", "apagado"}:
                payload["state"] = False
                return payload
        raise ValueError("Valor de 'state' no reconocido")

    @app.post("/api/pumps")
    def api_pumps():
        try:
            payload = _validate_payload(request.get_json(force=True))
        except Exception as exc:  # pragma: no cover - validación simple
            return jsonify({"error": str(exc)}), 400
        state = bridge.set_pumps(bool(payload["state"]))
        return jsonify(state.to_dict())

    @app.post("/api/ventilation")
    def api_ventilation():
        try:
            payload = _validate_payload(request.get_json(force=True))
        except Exception as exc:  # pragma: no cover - validación simple
            return jsonify({"error": str(exc)}), 400
        state = bridge.set_ventilation(bool(payload["state"]))
        return jsonify(state.to_dict())

    @socketio.on("connect")
    def handle_connect():  # pragma: no cover - evento de red
        emit("state", bridge.state.to_dict())

    app.socketio = socketio  # type: ignore[attr-defined]
    app.bridge = bridge      # type: ignore[attr-defined]
    return app


def main() -> None:
    app = create_app()
    socketio: SocketIO = app.socketio  # type: ignore[attr-defined]
    host = os.getenv("FLASK_RUN_HOST", "0.0.0.0")
    port = int(os.getenv("FLASK_RUN_PORT", os.getenv("PORT", "5000")))
    debug = os.getenv("FLASK_DEBUG", "0") == "1"
    LOGGER.info("Iniciando servidor en http://%s:%s (debug=%s)", host, port, debug)
    socketio.run(app, host=host, port=port, debug=debug)


if __name__ == "__main__":
    main()
