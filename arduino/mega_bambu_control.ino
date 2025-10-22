#include <Wire.h>                      // Incluye la librería para comunicarse por I2C.
#include <LiquidCrystal_I2C.h>         // Incluye la librería del display LCD con interfaz I2C.
#include <DHT.h>                       // Incluye la librería del sensor DHT.
#include <WiFiNINA.h>                  // Incluye la librería WiFi para el módulo compatible con Arduino Mega (por ejemplo, WiFiNINA o ESP32).
#include <ThingSpeak.h>                // Incluye la librería de la plataforma ThingSpeak.
#include <math.h>                      // Incluye funciones matemáticas como isnan para validar las lecturas del sensor DHT.

// -------------------- Configuración de hardware --------------------
const uint8_t RELAY_PUMPS[4] = {22, 23, 24, 25}; // Declara un arreglo con los pines digitales conectados a los 4 relés de las bombas.
const uint8_t FAN_PINS[2]    = {26, 27};         // Declara un arreglo con los pines digitales conectados a los dos ventiladores Noctua.
const uint8_t HEATER_PINS[3] = {28, 29, 30};     // Declara un arreglo con los pines digitales conectados a los tres calefactores.

const uint8_t BUTTON_MENU    = 31;               // Define el pin del botón/palanca que cambia la pantalla del menú.
const uint8_t BUTTON_PUMPS   = 32;               // Define el pin del botón que enciende/apaga las bombas.
const uint8_t BUTTON_VENT    = 33;               // Define el pin del botón que enciende/apaga el sistema de ventilación.

const uint8_t DHT_PIN        = 34;               // Define el pin digital conectado al sensor DHT22.
const uint8_t DHT_TYPE       = DHT22;            // Indica que el modelo del sensor es DHT22.

LiquidCrystal_I2C lcd(0x27, 20, 4);              // Crea el objeto LCD indicando la dirección I2C y el tamaño de 20 columnas por 4 filas.
DHT dht(DHT_PIN, DHT_TYPE);                      // Crea el objeto DHT asociado al pin y tipo configurados.

WiFiClient wifiClient;                           // Crea el objeto de cliente WiFi que se usará para ThingSpeak.

// -------------------- Configuración de red y ThingSpeak --------------------
const char WIFI_SSID[]     = "TU_SSID";         // Guarda el nombre de la red WiFi a la que se conectará el Arduino.
const char WIFI_PASSWORD[] = "TU_PASSWORD";     // Guarda la contraseña de la red WiFi.
const unsigned long THINGSPEAK_CHANNEL = 000000; // Reemplaza por el número de tu canal en ThingSpeak.
const char THINGSPEAK_API_KEY[] = "TU_API_KEY"; // Guarda la API Key de escritura del canal ThingSpeak.

// -------------------- Variables de estado --------------------
bool pumpsState = false;                         // Variable que almacena el estado (encendido/apagado) del conjunto de bombas.
bool ventState   = false;                        // Variable que almacena el estado del sistema de ventilación (ventiladores + calefactores).
float currentTemp = 0.0f;                        // Variable que guarda la temperatura actual leída por el DHT.
float currentHum  = 0.0f;                        // Variable que guarda la humedad actual leída por el DHT.
String serialBuffer;                             // Acumula los caracteres recibidos por el puerto serie hasta formar un comando completo.

uint8_t menuIndex = 0;                           // Variable que lleva la pantalla actual del menú en el LCD.
const uint8_t MENU_COUNT = 3;                    // Número de pantallas disponibles en el menú.

// -------------------- Variables de temporización --------------------
unsigned long lastSensorMillis = 0;              // Guarda el último instante en que se leyó el sensor DHT.
const unsigned long SENSOR_INTERVAL = 2000;      // Intervalo de lectura del DHT en milisegundos (2 segundos).

unsigned long lastThingSpeakMillis = 0;          // Guarda el último instante en que se envió un paquete a ThingSpeak.
const unsigned long THINGSPEAK_INTERVAL = 15000; // Intervalo entre envíos a ThingSpeak en milisegundos (15 segundos).

// -------------------- Variables para gestionar botones --------------------
struct ButtonState {                              // Declara una estructura para manejar el estado y antirrebote de cada botón.
  uint8_t pin;                                    // Guarda el pin asignado al botón.
  bool    lastStableState;                        // Guarda el último estado estable (HIGH/LOW) leído del botón.
  bool    lastReading;                            // Guarda la lectura anterior para el algoritmo de antirrebote.
  unsigned long lastDebounceTime;                 // Guarda el tiempo en que cambió el estado por última vez.
};

const unsigned long DEBOUNCE_DELAY = 50;         // Define el tiempo mínimo (ms) para considerar estable la lectura del botón.

ButtonState buttonMenu  = {BUTTON_MENU, HIGH, HIGH, 0}; // Inicializa la estructura para el botón de menú con estado alto (pull-up).
ButtonState buttonPumps = {BUTTON_PUMPS, HIGH, HIGH, 0}; // Inicializa la estructura para el botón de bombas.
ButtonState buttonVent  = {BUTTON_VENT, HIGH, HIGH, 0};  // Inicializa la estructura para el botón de ventilación.

void publishState(const char *origin);            // Declara anticipadamente la función que envía el estado por puerto serie.
void processSerialCommand(const String &command); // Declara anticipadamente la función que interpreta comandos entrantes.
void handleSerial();                              // Declara anticipadamente la función que atiende el puerto serie.

// -------------------- Funciones auxiliares --------------------
bool updateButton(ButtonState &button) {          // Declara una función que actualiza el estado de un botón y devuelve true si hubo pulsación.
  bool reading = digitalRead(button.pin);         // Lee el estado actual del pin del botón.
  if (reading != button.lastReading) {            // Comprueba si la lectura cambió con respecto a la anterior.
    button.lastDebounceTime = millis();           // Reinicia el temporizador de antirrebote si hay un cambio.
    button.lastReading = reading;                 // Actualiza la lectura anterior con la nueva lectura.
  }
  if ((millis() - button.lastDebounceTime) > DEBOUNCE_DELAY) { // Comprueba si ha pasado el tiempo de antirrebote.
    if (reading != button.lastStableState) {      // Verifica si el estado estable cambió tras el antirrebote.
      button.lastStableState = reading;           // Actualiza el estado estable.
      if (reading == LOW) {                       // Si el botón utiliza pull-up, LOW significa que fue presionado.
        return true;                              // Devuelve true para indicar que hubo una pulsación válida.
      }
    }
  }
  return false;                                   // Devuelve false cuando no se detecta una pulsación estable.
}

void setPumps(bool on) {                          // Declara una función que enciende o apaga todas las bombas simultáneamente.
  pumpsState = on;                                // Guarda el estado deseado en la variable global.
  for (uint8_t i = 0; i < 4; i++) {               // Recorre cada índice del arreglo de relés de bombas.
    digitalWrite(RELAY_PUMPS[i], on ? LOW : HIGH);// Activa el relé (LOW) o lo desactiva (HIGH) según el estado, asumiendo relés activos en LOW.
  }
  publishState("pumps");                         // Envía el estado actualizado por el puerto serie para el servidor externo.
}

void setVentilation(bool on) {                    // Declara una función que controla ventiladores y calefactores.
  ventState = on;                                 // Guarda el estado deseado en la variable global.
  for (uint8_t i = 0; i < 2; i++) {               // Recorre los pines de los ventiladores.
    digitalWrite(FAN_PINS[i], on ? HIGH : LOW);   // Enciende los ventiladores (HIGH) o los apaga (LOW) según cómo estén cableados.
  }
  for (uint8_t i = 0; i < 3; i++) {               // Recorre los pines de los calefactores.
    digitalWrite(HEATER_PINS[i], on ? HIGH : LOW);// Enciende o apaga los calefactores siguiendo la lógica del hardware.
  }
  publishState("ventilation");                    // Informa por el puerto serie que cambió el estado del sistema de ventilación.
}

void readSensors() {                              // Declara una función para actualizar las lecturas del DHT.
  float hum = dht.readHumidity();                 // Lee la humedad relativa del sensor en una variable temporal.
  float temp = dht.readTemperature();             // Lee la temperatura en grados Celsius en una variable temporal.
  if (!isnan(hum)) {                              // Comprueba que la lectura de humedad sea válida (no NaN).
    currentHum = hum;                             // Actualiza la humedad almacenada con la lectura válida.
  }
  if (!isnan(temp)) {                             // Comprueba que la lectura de temperatura sea válida.
    currentTemp = temp;                           // Actualiza la temperatura almacenada con la lectura válida.
  }
  publishState("sensors");                       // Publica la lectura actual por el puerto serie para el servidor remoto.
}

void updateDisplay() {                            // Declara una función que refresca la pantalla LCD según el menú activo.
  lcd.clear();                                    // Limpia el contenido actual del LCD.
  switch (menuIndex) {                            // Selecciona qué pantalla mostrar según el índice del menú.
    case 0:                                       // Pantalla 0: muestra temperatura y humedad.
      lcd.setCursor(0, 0);                        // Coloca el cursor en la columna 0, fila 0.
      lcd.print("Temp: ");                        // Escribe la etiqueta de temperatura.
      lcd.print(currentTemp, 1);                  // Muestra la temperatura con un decimal.
      lcd.print(" C");                            // Añade la unidad Celsius.
      lcd.setCursor(0, 1);                        // Coloca el cursor en la columna 0, fila 1.
      lcd.print("Hum:  ");                        // Escribe la etiqueta de humedad.
      lcd.print(currentHum, 1);                   // Muestra la humedad con un decimal.
      lcd.print(" %");                           // Añade el símbolo de porcentaje.
      lcd.setCursor(0, 2);                        // Posiciona el cursor en la tercera fila.
      lcd.print("Bombas: ");                      // Escribe la etiqueta del estado de las bombas.
      lcd.print(pumpsState ? "ON " : "OFF");       // Muestra si las bombas están encendidas o apagadas.
      lcd.setCursor(0, 3);                        // Posiciona el cursor en la cuarta fila.
      lcd.print("Vent:   ");                      // Escribe la etiqueta del sistema de ventilación.
      lcd.print(ventState ? "ON" : "OFF");        // Muestra el estado del sistema de ventilación.
      break;                                      // Sale del switch para esta pantalla.
    case 1:                                       // Pantalla 1: detalle de bombas.
      lcd.setCursor(0, 0);                        // Posiciona el cursor en la primera fila.
      lcd.print("Estado Bombas");                 // Muestra un título descriptivo.
      for (uint8_t i = 0; i < 4; i++) {           // Recorre las cuatro bombas.
        lcd.setCursor(0, i % 3 + 1);              // Distribuye el texto en las filas 1 a 3.
        lcd.print("B");                           // Imprime la letra identificadora de bomba.
        lcd.print(i + 1);                         // Añade el número de bomba (1 a 4).
        lcd.print(": ");                          // Añade separador.
        lcd.print(pumpsState ? "ON " : "OFF");     // Muestra el estado común a todas las bombas.
      }
      break;                                      // Finaliza el caso 1.
    case 2:                                       // Pantalla 2: detalle de ventilación.
      lcd.setCursor(0, 0);                        // Posiciona el cursor en la primera fila.
      lcd.print("Ventilacion");                   // Muestra el título de la pantalla.
      lcd.setCursor(0, 1);                        // Posiciona el cursor en la segunda fila.
      lcd.print("Ventiladores: ");                // Escribe la etiqueta para los ventiladores.
      lcd.print(ventState ? "ON" : "OFF");        // Indica el estado de los ventiladores.
      lcd.setCursor(0, 2);                        // Posiciona el cursor en la tercera fila.
      lcd.print("Calefactores: ");                // Escribe la etiqueta para los calefactores.
      lcd.print(ventState ? "ON" : "OFF");        // Indica el estado de los calefactores.
      lcd.setCursor(0, 3);                        // Posiciona el cursor en la cuarta fila.
      lcd.print("Menu -> Boton");                 // Muestra una instrucción para cambiar de pantalla.
      break;                                      // Finaliza el caso 2.
  }
}

void connectWiFi() {                              // Declara una función que gestiona la conexión WiFi.
  if (WiFi.status() == WL_NO_MODULE) {            // Comprueba si el módulo WiFi está presente.
    while (true) {                                // Entra en un bucle infinito si no se detecta módulo (previene continuar sin red).
      delay(1000);                                // Espera un segundo en cada iteración para evitar bloquear completamente.
    }
  }
  while (WiFi.status() != WL_CONNECTED) {         // Intenta conectarse mientras el estado no sea conectado.
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);         // Inicia la conexión utilizando las credenciales.
    unsigned long startAttempt = millis();        // Registra el instante en el que se inicia el intento.
    while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt) < 10000) { // Espera hasta 10 segundos por la conexión.
      delay(500);                                 // Realiza una espera corta antes de volver a chequear el estado.
    }
  }
}

void sendToThingSpeak() {                         // Declara una función que envía los datos actuales a ThingSpeak.
  ThingSpeak.setField(1, currentTemp);            // Configura el campo 1 del canal con la temperatura.
  ThingSpeak.setField(2, currentHum);             // Configura el campo 2 del canal con la humedad.
  for (uint8_t i = 0; i < 4; i++) {               // Recorre los cuatro relés de las bombas.
    ThingSpeak.setField(3 + i, pumpsState ? 1 : 0); // Publica el estado de cada bomba en campos consecutivos (3 al 6).
  }
  ThingSpeak.setField(7, ventState ? 1 : 0);      // Configura el campo 7 con el estado binario de los ventiladores.
  ThingSpeak.setField(8, ventState ? 1 : 0);      // Configura el campo 8 con el estado binario de los calefactores.
  int status = ThingSpeak.writeFields(THINGSPEAK_CHANNEL, THINGSPEAK_API_KEY); // Envía todos los campos al canal y guarda el estado devuelto.
  if (status != 200) {                            // Verifica si el código HTTP de respuesta es diferente de 200 (éxito).
    // Aquí podrías añadir manejo de errores, por ejemplo mostrar un mensaje en el LCD o por Serial.
  }
}

void publishState(const char *origin) {           // Declara una función que envía por el puerto serie el estado de sensores y actuadores.
  Serial.print("STATE;origin=");                  // Escribe el prefijo del mensaje indicando que es un estado y quién lo generó.
  Serial.print(origin);                           // Añade el origen del mensaje (sensores, pumps, ventilation, etc.).
  Serial.print(";temp=");                         // Continúa el mensaje con la etiqueta de temperatura.
  Serial.print(currentTemp, 2);                   // Inserta la temperatura con dos decimales.
  Serial.print(";hum=");                          // Añade la etiqueta de humedad.
  Serial.print(currentHum, 2);                    // Inserta la humedad con dos decimales.
  Serial.print(";pumps=");                        // Añade la etiqueta para el estado de las bombas.
  Serial.print(pumpsState ? 1 : 0);               // Inserta el estado de las bombas como 1 (encendido) o 0 (apagado).
  Serial.print(";vent=");                         // Añade la etiqueta para el estado de la ventilación.
  Serial.print(ventState ? 1 : 0);                // Inserta el estado del sistema de ventilación como 1 u 0.
  Serial.println();                               // Termina el mensaje con un salto de línea para que el servidor pueda leerlo.
}

void processSerialCommand(const String &command) {// Declara una función que interpreta comandos recibidos desde el servidor.
  String cmd = command;                           // Copia el comando recibido en una variable mutable.
  cmd.trim();                                     // Elimina espacios o saltos de línea sobrantes en los extremos.
  cmd.toUpperCase();                              // Convierte todo a mayúsculas para comparar sin distinción de caso.
  if (cmd == "PUMPS:ON") {                       // Comprueba si el comando solicita encender las bombas.
    setPumps(true);                               // Enciende las bombas.
  } else if (cmd == "PUMPS:OFF") {               // Comprueba si el comando solicita apagar las bombas.
    setPumps(false);                              // Apaga las bombas.
  } else if (cmd == "VENT:ON") {                 // Comprueba si se solicita encender el sistema de ventilación.
    setVentilation(true);                         // Enciende ventiladores y calefactores.
  } else if (cmd == "VENT:OFF") {                // Comprueba si se solicita apagarlos.
    setVentilation(false);                        // Apaga ventiladores y calefactores.
  } else if (cmd == "REQUEST:STATE") {           // Comprueba si se pide un estado inmediato.
    publishState("request");                      // Envía el estado actual sin cambiar nada.
  }
}

void handleSerial() {                             // Declara una función que lee datos entrantes del puerto serie.
  while (Serial.available() > 0) {                // Revisa si hay datos pendientes.
    char incoming = static_cast<char>(Serial.read()); // Lee el siguiente carácter disponible.
    if (incoming == '\n' || incoming == '\r') {  // Detecta fin de línea o retorno de carro.
      if (serialBuffer.length() > 0) {            // Comprueba que haya algún comando almacenado.
        processSerialCommand(serialBuffer);       // Procesa el comando completo recibido.
        serialBuffer = "";                       // Vacía el buffer para recibir el siguiente comando.
      }
    } else if (serialBuffer.length() < 64) {      // Limita el tamaño del comando para evitar desbordes de memoria.
      serialBuffer += incoming;                   // Agrega el carácter leído al buffer de comando.
    }
  }
}

// -------------------- Configuración inicial --------------------
void setup() {                                    // Función que se ejecuta una vez al iniciar el Arduino.
  Serial.begin(115200);                           // Inicializa el puerto serie a 115200 baudios para comunicarse con la Raspberry Pi.
  while (!Serial) {                               // Espera a que el puerto serie esté listo (relevante en placas con USB nativo).
    delay(10);                                    // Realiza pequeñas esperas para no saturar el bucle de espera.
  }
  for (uint8_t i = 0; i < 4; i++) {               // Recorre los pines de las bombas.
    pinMode(RELAY_PUMPS[i], OUTPUT);              // Configura cada pin de bomba como salida.
    digitalWrite(RELAY_PUMPS[i], HIGH);           // Pone cada relé en estado apagado (HIGH si son activos en LOW).
  }
  for (uint8_t i = 0; i < 2; i++) {               // Recorre los pines de los ventiladores.
    pinMode(FAN_PINS[i], OUTPUT);                 // Configura cada pin como salida.
    digitalWrite(FAN_PINS[i], LOW);               // Asegura que los ventiladores empiezan apagados.
  }
  for (uint8_t i = 0; i < 3; i++) {               // Recorre los pines de los calefactores.
    pinMode(HEATER_PINS[i], OUTPUT);              // Configura cada pin como salida.
    digitalWrite(HEATER_PINS[i], LOW);            // Asegura que los calefactores empiezan apagados.
  }
  pinMode(BUTTON_MENU, INPUT_PULLUP);             // Configura el botón de menú con resistencia pull-up interna.
  pinMode(BUTTON_PUMPS, INPUT_PULLUP);            // Configura el botón de bombas con pull-up.
  pinMode(BUTTON_VENT, INPUT_PULLUP);             // Configura el botón de ventilación con pull-up.

  lcd.init();                                     // Inicializa el LCD I2C.
  lcd.backlight();                                // Enciende la retroiluminación del LCD.
  lcd.print("Iniciando...");                    // Muestra un mensaje de inicio.

  dht.begin();                                    // Inicializa el sensor DHT.

  connectWiFi();                                  // Llama a la función que establece la conexión WiFi.
  ThingSpeak.begin(wifiClient);                   // Inicializa la comunicación con ThingSpeak usando el cliente WiFi.

  readSensors();                                  // Realiza una primera lectura para inicializar temperatura y humedad.
  lastSensorMillis = millis();                    // Toma el instante actual como referencia para la primera lectura de sensores.
  lastThingSpeakMillis = millis();                // Toma el instante actual como referencia para el primer envío a ThingSpeak.
  updateDisplay();                                // Refresca la pantalla para mostrar la primera pantalla del menú.
  publishState("boot");                          // Informa al servidor externo del estado inicial del sistema.
}

// -------------------- Bucle principal --------------------
void loop() {                                     // Función que se ejecuta continuamente mientras Arduino está encendido.
  handleSerial();                                 // Atiende cualquier comando recibido desde el servidor web.
  if (updateButton(buttonMenu)) {                 // Comprueba si se pulsó el botón de menú.
    menuIndex = (menuIndex + 1) % MENU_COUNT;     // Avanza al siguiente índice de menú y regresa a 0 cuando llega al final.
    updateDisplay();                              // Actualiza el LCD para reflejar la nueva pantalla.
  }
  if (updateButton(buttonPumps)) {                // Comprueba si se pulsó el botón de las bombas.
    setPumps(!pumpsState);                        // Cambia el estado de las bombas al contrario del actual.
    updateDisplay();                              // Refresca la pantalla para mostrar el nuevo estado.
  }
  if (updateButton(buttonVent)) {                 // Comprueba si se pulsó el botón del sistema de ventilación.
    setVentilation(!ventState);                   // Cambia el estado del sistema de ventilación.
    updateDisplay();                              // Refresca la pantalla para mostrar el nuevo estado.
  }

  unsigned long now = millis();                   // Guarda el tiempo actual en milisegundos.
  if (now - lastSensorMillis >= SENSOR_INTERVAL) {// Comprueba si ya pasó el intervalo de lectura del sensor.
    lastSensorMillis = now;                       // Actualiza el registro del último instante de lectura.
    readSensors();                                // Llama a la función que lee temperatura y humedad.
    updateDisplay();                              // Refresca la pantalla para mostrar los valores recientes.
  }

  if (now - lastThingSpeakMillis >= THINGSPEAK_INTERVAL) { // Comprueba si ya pasó el intervalo de envío a ThingSpeak.
    lastThingSpeakMillis = now;                   // Actualiza el registro del último envío.
    sendToThingSpeak();                           // Llama a la función que envía los datos a la nube.
  }
}
