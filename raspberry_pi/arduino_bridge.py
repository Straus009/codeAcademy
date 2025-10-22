"""Puente de comunicación entre la Raspberry Pi y el Arduino Mega.

Este módulo abstrae el puerto serie para ofrecer métodos sencillos que
permiten obtener el estado del sistema y enviar órdenes a los actuadores.
Puede ejecutarse en modo simulación cuando no existe un Arduino conectado,
lo que facilita las pruebas de la interfaz web.
"""
from __future__ import annotations

import logging
import random
import threading
import time
from dataclasses import dataclass, field, asdict
from typing import Callable, Dict, List, Optional

try:
    import serial  # type: ignore
    SerialException = serial.SerialException  # type: ignore[attr-defined]
except ModuleNotFoundError:  # pragma: no cover - permite ejecutar pruebas sin hardware
    serial = None  # type: ignore

    class SerialException(Exception):
        """Excepción genérica usada cuando pyserial no está instalado."""

        pass


LOGGER = logging.getLogger(__name__)


@dataclass
class DeviceState:
    """Representa el estado agregado del ecosistema controlado por el Arduino."""

    temperature: Optional[float] = None
    humidity: Optional[float] = None
    pumps: bool = False
    ventilation: bool = False
    last_origin: str = "boot"
    last_update: float = field(default_factory=lambda: time.time())

    def update_from_payload(self, payload: Dict[str, str]) -> None:
        """Actualiza el estado en función de los campos recibidos por serie."""

        if "temp" in payload:
            try:
                self.temperature = float(payload["temp"])
            except ValueError:
                LOGGER.debug("Lectura de temperatura inválida: %s", payload["temp"])
        if "hum" in payload:
            try:
                self.humidity = float(payload["hum"])
            except ValueError:
                LOGGER.debug("Lectura de humedad inválida: %s", payload["hum"])
        if "pumps" in payload:
            self.pumps = payload["pumps"].strip() in {"1", "ON", "TRUE"}
        if "vent" in payload:
            self.ventilation = payload["vent"].strip() in {"1", "ON", "TRUE"}
        if "origin" in payload:
            self.last_origin = payload["origin"]
        self.last_update = time.time()

    def to_dict(self) -> Dict[str, Optional[float]]:
        """Convierte el estado a un diccionario serializable."""

        return {
            "temperature": self.temperature,
            "humidity": self.humidity,
            "pumps": self.pumps,
            "ventilation": self.ventilation,
            "last_origin": self.last_origin,
            "last_update": self.last_update,
        }


class ArduinoBridge:
    """Gestiona la comunicación con el Arduino y notifica los cambios."""

    def __init__(
        self,
        port: str,
        baudrate: int = 115200,
        simulate: bool = False,
        read_timeout: float = 1.0,
    ) -> None:
        self.port = port
        self.baudrate = baudrate
        self.simulate = simulate
        self.read_timeout = read_timeout
        self._callbacks: List[Callable[[DeviceState], None]] = []
        self._stop_event = threading.Event()
        self.state = DeviceState()
        self._serial: Optional[serial.Serial] = None

        if not simulate:
            if serial is None:
                LOGGER.warning(
                    "pyserial no está disponible; se activará el modo simulación automáticamente."
                )
                self.simulate = True
            else:
                try:
                    self._serial = serial.Serial(port, baudrate=baudrate, timeout=read_timeout)
                    LOGGER.info("Conexión serie abierta en %s a %s baudios", port, baudrate)
                except SerialException as exc:
                    LOGGER.warning(
                        "No fue posible abrir el puerto %s (%s). Se activará el modo simulación.",
                        port,
                        exc,
                    )
                    self.simulate = True

        if self.simulate:
            LOGGER.info("Iniciando puente en modo simulación")
            self._thread = threading.Thread(target=self._simulate_loop, daemon=True)
        else:
            self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()

    # ------------------------------------------------------------------
    # Registro y notificación de callbacks
    # ------------------------------------------------------------------
    def on_update(self, callback: Callable[[DeviceState], None]) -> Callable[[DeviceState], None]:
        """Registra una función que será llamada cuando cambie el estado."""

        self._callbacks.append(callback)
        return callback

    def _emit(self) -> None:
        for callback in list(self._callbacks):
            try:
                callback(self.state)
            except Exception:  # pragma: no cover - evita que una vista rompa el hilo lector
                LOGGER.exception("Error notificando un callback de estado")

    # ------------------------------------------------------------------
    # Bucle de lectura en modo real
    # ------------------------------------------------------------------
    def _reader_loop(self) -> None:
        assert self._serial is not None
        while not self._stop_event.is_set():
            try:
                chunk = self._serial.read_until(b"\n")
            except SerialException as exc:
                LOGGER.error("Error leyendo del puerto serie: %s", exc)
                break
            if not chunk:
                continue
            try:
                line = chunk.decode("utf-8", errors="ignore").strip()
            except UnicodeDecodeError:
                LOGGER.debug("Se descartó una línea por error de codificación")
                continue
            if not line:
                continue
            self._handle_line(line)

    # ------------------------------------------------------------------
    # Bucle de simulación para desarrollo
    # ------------------------------------------------------------------
    def _simulate_loop(self) -> None:
        while not self._stop_event.is_set():
            time.sleep(2)
            payload = {
                "temp": f"{random.uniform(20.0, 30.0):.2f}",
                "hum": f"{random.uniform(40.0, 60.0):.2f}",
                "pumps": "1" if self.state.pumps else "0",
                "vent": "1" if self.state.ventilation else "0",
                "origin": "simulator",
            }
            self.state.update_from_payload(payload)
            self._emit()

    # ------------------------------------------------------------------
    # Procesamiento de líneas entrantes
    # ------------------------------------------------------------------
    def _handle_line(self, line: str) -> None:
        if not line:
            return
        parts = line.split(";")
        payload: Dict[str, str] = {}
        for part in parts:
            if "=" in part:
                key, value = part.split("=", 1)
                payload[key.strip().lower()] = value.strip()
        if not payload:
            LOGGER.debug("Línea de estado descartada: %s", line)
            return
        self.state.update_from_payload(payload)
        LOGGER.debug("Estado actualizado desde serie: %s", asdict(self.state))
        self._emit()

    # ------------------------------------------------------------------
    # API pública
    # ------------------------------------------------------------------
    def request_state(self) -> None:
        """Solicita explícitamente al Arduino que envíe su estado actual."""

        if self.simulate:
            return
        if self._serial is None:
            return
        self._write_line("REQUEST:STATE")

    def set_pumps(self, enabled: bool) -> DeviceState:
        """Enciende o apaga las bombas de agua."""

        if self.simulate:
            self.state.pumps = enabled
            self.state.last_origin = "api"
            self.state.last_update = time.time()
            self._emit()
            return self.state
        self._write_line(f"PUMPS:{'ON' if enabled else 'OFF'}")
        return self.state

    def set_ventilation(self, enabled: bool) -> DeviceState:
        """Enciende o apaga ventiladores y calefactores."""

        if self.simulate:
            self.state.ventilation = enabled
            self.state.last_origin = "api"
            self.state.last_update = time.time()
            self._emit()
            return self.state
        self._write_line(f"VENT:{'ON' if enabled else 'OFF'}")
        return self.state

    def _write_line(self, text: str) -> None:
        if self._serial is None:
            LOGGER.warning("No hay puerto serie abierto para enviar: %s", text)
            return
        try:
            self._serial.write((text + "\n").encode("utf-8"))
        except SerialException as exc:
            LOGGER.error("No se pudo enviar '%s' por el puerto serie: %s", text, exc)

    def close(self) -> None:
        """Detiene los hilos y cierra el puerto serie."""

        self._stop_event.set()
        if self._serial is not None and self._serial.is_open:
            try:
                self._serial.close()
            except SerialException:
                LOGGER.exception("Error cerrando el puerto serie")

    # Permite usar la clase como un gestor de contexto.
    def __enter__(self) -> "ArduinoBridge":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()


__all__ = ["ArduinoBridge", "DeviceState"]
