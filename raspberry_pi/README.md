# Servidor web en Raspberry Pi 5

Este directorio contiene el código necesario para instalar un servidor
Flask + Socket.IO en una Raspberry Pi 5 (16 GB) que se comunica con el
Arduino Mega encargado de accionar bombas, ventiladores y calefactores
para el cerramiento de la impresora 3D.

## Requisitos previos

- Raspberry Pi OS (Bookworm o superior) actualizado.
- Python 3.11 (incluido en la distribución oficial).
- Acceso a internet para instalar dependencias.
- Arduino Mega conectado por USB a la Raspberry Pi y cargado con el
  sketch `arduino/mega_bambu_control.ino` incluido en este repositorio.

## Instalación

```bash
sudo apt update && sudo apt install -y python3-venv git
cd /opt
sudo git clone https://github.com/<tu-usuario>/codeAcademy.git bambu-control
cd bambu-control/raspberry_pi
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install -r requirements.txt
```

Configura las variables de entorno en un archivo `.env` o exportándolas
directamente antes de arrancar el servicio:

- `ARDUINO_PORT`: puerto serie donde está el Arduino (por ejemplo,
  `/dev/ttyACM0` o `/dev/ttyUSB0`).
- `ARDUINO_BAUDRATE`: velocidad del puerto serie (por defecto 115200).
- `ARDUINO_SIMULATE`: establece `1` para ejecutar sin hardware físico
  (útil durante el desarrollo).
- `FLASK_SECRET`: cadena aleatoria para asegurar la sesión de Flask.

## Ejecución manual

```bash
source .venv/bin/activate
export ARDUINO_PORT=/dev/ttyACM0
python app.py
```

El panel estará disponible en `http://<ip-de-la-pi>:5000/`. Desde ahí
podrás ver temperatura y humedad, encender/apagar bombas y controlar el
sistema de ventilación. Los cambios también quedan registrados en tiempo
real en la consola mediante Socket.IO.

## Servicio systemd opcional

Copia el archivo `systemd/bambu-control.service` a `/etc/systemd/system`
(con permisos de superusuario) y ajusta las rutas y variables de entorno
según tus necesidades. Luego ejecuta:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now bambu-control.service
```

El servicio iniciará automáticamente en cada arranque de la Raspberry Pi.

## Desarrollo sin hardware

Si el Arduino no está conectado, activa el modo simulación:

```bash
export ARDUINO_SIMULATE=1
python app.py
```

El simulador generará valores aleatorios de temperatura y humedad para
validar la interfaz web sin depender del hardware físico.
