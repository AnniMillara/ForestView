Monitoreo forestal
Supervisa las condiciones ambientales y detecta posibles riesgos en tiempo real.

Sistema operativo
0/2 estaciones conectadas
Temperatura
--
°C
 Sin datos
Humedad
--
%
 Sin datos
Nivel de humo
--
nivel
 Sin datos
Nivel de riesgo
NORMAL
Todos los parámetros normales
MONITOREO
Condiciones ambientales

Últimas 24 horas
UBICACIÓN
Zonas monitoreadas
+
−
 Leaflet | © OpenStreetMap, © CartoDB
ALERTAS
Alertas recientes
Ver todas
Sin alertas activas
Todos los parámetros normales
DISPOSITIVOS
Estado de sensores
Auto-TEST123
Sin zona
SIN_DATOS
ForestGuard-01
Zona Centro// =============================================================
//               FORESTGUARD ESP32 - FIRMWARE v2.2
//         Con descubrimiento automático de servidor + robustez
// =============================================================
//  Hardware: ESP32 + DHT22 + MQ-2 + LED RGB
//  Backend: Flask + MySQL
// =============================================================
//  Pines definitivos:
//    DHT22    -> GPIO 13
//    MQ-2 AO  -> GPIO 35   (analógico)
//    MQ-2 DO  -> GPIO 33   (digital)
//    LED ROJO -> GPIO 26
//    LED VERDE-> GPIO 27
//    LED AZUL -> GPIO 25
// =============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <WiFiUdp.h>
#include <string.h>   // para strlen (por si acaso)

// =============================================================
//  WIFI
// =============================================================
const char* WIFI_SSID     = "b3ar";
const char* WIFI_PASSWORD = "papyrusB3st";

// IP del servidor (se sobrescribirá con descubrimiento automático)
String SERVER_IP = "172.16.50.57";  // valor por defecto
const int   SERVER_PORT     = 5000;
const char* DEFAULT_API_KEY = "3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c";

// =============================================================
//  PINES (CORREGIDOS)
// =============================================================
#define DHT_PIN     13
#define DHT_TYPE    DHT22
#define MQ2_AO      35
#define MQ2_DO      33
#define LED_ROJO    26
#define LED_VERDE   27
#define LED_AZUL    25

// =============================================================
//  PWM LED
// =============================================================
const int FRECUENCIA  = 5000;
const int RESOLUCION  = 8;

// =============================================================
//  CONSTANTES DE CALIBRACIÓN Y FILTRADO
// =============================================================
const unsigned long TIEMPO_CALENTAMIENTO = 60000;
const unsigned long INTERVALO_LECTURA   = 2000;
const unsigned long INTERVALO_ENVIO     = 5000;
const unsigned long TIMEOUT_HTTP        = 5000;
const unsigned long TIEMPO_RECONEXION_WIFI = 10000;

// Filtros
const int MUESTRAS_PROMEDIO_TEMP = 5;
const int MUESTRAS_PROMEDIO_AO   = 10;

// Persistencia de estados
const int LECTURAS_PARA_RIESGO_MODERADO = 3;
const int LECTURAS_PARA_ALERTA_MAXIMA   = 3;
const int LECTURAS_PARA_VOLVER_NORMAL   = 5;

// Umbrales
const float TEMP_RIESGO      = 35.0;
const float TEMP_CRITICA     = 40.0;
const float HUMEDAD_RIESGO   = 35.0;
const float HUMEDAD_CRITICA  = 30.0;
const float PORCENTAJE_HUMO_LEVE      = 0.25;
const float PORCENTAJE_HUMO_MODERADO  = 0.35;
const float PORCENTAJE_HUMO_FUERTE    = 0.50;

// =============================================================
//  VARIABLES GLOBALES
// =============================================================
DHT dht(DHT_PIN, DHT_TYPE);
String stationCode = "";

// ---- LED ----
enum class LedState : uint8_t {
  CALENTANDO, NORMAL, RIESGO_MODERADO, ALERTA_MAXIMA, ERROR_SENSOR
};
LedState ledState = LedState::CALENTANDO;

// ---- MQ-2 ----
int aoBase = 0;
int estadoDONormal = HIGH;
bool aoDisponible = true;
bool calentamientoCompletado = false;

// Buffers
float tempBuffer[MUESTRAS_PROMEDIO_TEMP];
float humBuffer[MUESTRAS_PROMEDIO_TEMP];
int idxTemp = 0;
bool tempBufferLleno = false;

int aoBuffer[MUESTRAS_PROMEDIO_AO];
int idxAO = 0;
bool aoBufferLleno = false;

float temperatura = 0.0;
float humedad = 0.0;
int mq2AO = 0;
int mq2DO = HIGH;
int cambioAO = 0;
float porcentajeCambio = 0.0;
bool humoDigital = false;

enum class Riesgo : uint8_t { NORMAL, MODERADO, ALERTA };
Riesgo riesgoActual = Riesgo::NORMAL;
int contadorRiesgoModerado = 0;
int contadorAlertaMaxima = 0;
int contadorNormal = 0;

bool dhtOK = true;
bool mq2OK = true;
int fallosDHT = 0;
const int MAX_FALLOS_DHT = 3;

bool wifiConectado = false;
bool flaskDisponible = false;
unsigned long ultimoIntentoWiFi = 0;
unsigned long ultimoEnvioExitoso = 0;

// ---- Descubrimiento UDP ----
WiFiUDP udp;
bool servidorDescubierto = false;
unsigned long tiempoInicioDescubrimiento = 0;
const unsigned long TIMEOUT_DESCUBRIMIENTO = 5000;  // 5 segundos

// =============================================================
//  PROTOTIPOS
// =============================================================
void actualizarLED();
void calibrarMQ2();
void leerSensores();
void evaluarRiesgo();
void enviarDatos();
void conectarWiFi();
void comprobarWiFi();
void serialDiagnostico();
bool descubrirServidorUDP();
void configurarIPPorSerial();

// =============================================================
//  FUNCIONES LED
// =============================================================
void ledApagado() {
  ledcWrite(LED_ROJO, 0);
  ledcWrite(LED_VERDE, 0);
  ledcWrite(LED_AZUL, 0);
}
void ledVerde() {
  ledcWrite(LED_ROJO, 0);
  ledcWrite(LED_VERDE, 255);
  ledcWrite(LED_AZUL, 0);
}
void ledRojo() {
  ledcWrite(LED_ROJO, 255);
  ledcWrite(LED_VERDE, 0);
  ledcWrite(LED_AZUL, 0);
}
void ledAzul() {
  ledcWrite(LED_ROJO, 0);
  ledcWrite(LED_VERDE, 0);
  ledcWrite(LED_AZUL, 255);
}
void ledAmarillo() {
  ledcWrite(LED_ROJO, 255);
  ledcWrite(LED_VERDE, 180);
  ledcWrite(LED_AZUL, 0);
}
void ledParpadeoAzul() {
  static unsigned long ultimo = 0;
  if (millis() - ultimo > 500) {
    ultimo = millis();
    static bool estado = false;
    estado = !estado;
    if (estado) ledAzul(); else ledApagado();
  }
}

void actualizarLED() {
  if (!dhtOK || !mq2OK) {
    ledParpadeoAzul();
    return;
  }
  if (!calentamientoCompletado) {
    ledAzul();
    return;
  }
  switch (riesgoActual) {
    case Riesgo::ALERTA:   ledRojo(); break;
    case Riesgo::MODERADO: ledAmarillo(); break;
    default:               ledVerde(); break;
  }
}

// =============================================================
//  CALIBRACIÓN MQ-2
// =============================================================
void calibrarMQ2() {
  Serial.println();
  Serial.println("======================================");
  Serial.println("   CALENTANDO MQ-2 (60 segundos)");
  Serial.println("======================================");
  ledState = LedState::CALENTANDO;
  actualizarLED();

  long sumaAO = 0;
  int muestrasValidas = 0;
  int high = 0, low = 0;

  unsigned long inicio = millis();
  while (millis() - inicio < TIEMPO_CALENTAMIENTO) {
    int ao = analogRead(MQ2_AO);
    int doVal = digitalRead(MQ2_DO);
    if ((millis() - inicio) % 5000 < 500) Serial.print(".");
    if (millis() - inicio > TIEMPO_CALENTAMIENTO - 10000) {
      if (ao >= 0 && ao <= 4095) { sumaAO += ao; muestrasValidas++; }
      if (doVal == HIGH) high++; else low++;
    }
    delay(500);
  }
  Serial.println();

  if (muestrasValidas == 0) {
    Serial.println("ERROR: No se pudieron leer muestras válidas del AO.");
    aoDisponible = false;
    aoBase = 0;
  } else {
    aoBase = sumaAO / muestrasValidas;
    aoDisponible = true;
    Serial.print("AO BASE = "); Serial.println(aoBase);
  }

  estadoDONormal = (high >= low) ? HIGH : LOW;
  Serial.print("DO NORMAL = "); Serial.println(estadoDONormal);

  if (aoDisponible && aoBase < 50) {
    Serial.println("ADVERTENCIA: AO muy bajo. Posiblemente el pin no está conectado.");
    aoDisponible = false;
  }

  calentamientoCompletado = true;
  ledState = LedState::NORMAL;
  actualizarLED();
  Serial.println("CALIBRACIÓN TERMINADA");
  Serial.println();
}

// =============================================================
//  LECTURA DHT22
// =============================================================
bool leerDHT22(float &temp, float &hum) {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h) || t < -10 || t > 50 || h < 10 || h > 90) {
    return false;
  }
  temp = t;
  hum = h;
  return true;
}

// =============================================================
//  FILTROS (promedio móvil)
// =============================================================
void agregarMuestraTemp(float temp, float hum) {
  tempBuffer[idxTemp] = temp;
  humBuffer[idxTemp] = hum;
  idxTemp = (idxTemp + 1) % MUESTRAS_PROMEDIO_TEMP;
  if (idxTemp == 0) tempBufferLleno = true;
}

float obtenerPromedioTemp(bool humedad) {
  int n = tempBufferLleno ? MUESTRAS_PROMEDIO_TEMP : idxTemp;
  if (n == 0) return 0;
  float suma = 0;
  for (int i = 0; i < n; i++) {
    suma += humedad ? humBuffer[i] : tempBuffer[i];
  }
  return suma / n;
}

void agregarMuestraAO(int ao) {
  aoBuffer[idxAO] = ao;
  idxAO = (idxAO + 1) % MUESTRAS_PROMEDIO_AO;
  if (idxAO == 0) aoBufferLleno = true;
}

int obtenerPromedioAO() {
  int n = aoBufferLleno ? MUESTRAS_PROMEDIO_AO : idxAO;
  if (n == 0) return analogRead(MQ2_AO);
  long suma = 0;
  for (int i = 0; i < n; i++) suma += aoBuffer[i];
  return suma / n;
}

// =============================================================
//  LECTURA DE SENSORES
// =============================================================
void leerSensores() {
  float t, h;
  if (leerDHT22(t, h)) {
    dhtOK = true;
    fallosDHT = 0;
    agregarMuestraTemp(t, h);
  } else {
    fallosDHT++;
    if (fallosDHT >= MAX_FALLOS_DHT) {
      dhtOK = false;
    }
  }

  int aoRaw = analogRead(MQ2_AO);
  int doVal = digitalRead(MQ2_DO);

  if (aoRaw >= 0 && aoRaw <= 4095) {
    mq2OK = true;
    agregarMuestraAO(aoRaw);
  } else {
    mq2OK = false;
  }

  mq2AO = obtenerPromedioAO();
  mq2DO = doVal;

  if (!aoDisponible || aoBase == 0) {
    cambioAO = 0;
    porcentajeCambio = 0.0;
  } else {
    cambioAO = abs(mq2AO - aoBase);
    porcentajeCambio = (float)cambioAO / aoBase;
  }

  humoDigital = (mq2DO != estadoDONormal);

  if (dhtOK) {
    temperatura = obtenerPromedioTemp(false);
    humedad = obtenerPromedioTemp(true);
  }
}

// =============================================================
//  EVALUACIÓN DE RIESGO (sin cambios)
// =============================================================
void evaluarRiesgo() {
  bool tempRiesgo = (temperatura >= TEMP_RIESGO);
  bool tempCritica = (temperatura >= TEMP_CRITICA);
  bool humRiesgo = (humedad <= HUMEDAD_RIESGO);
  bool humCritica = (humedad <= HUMEDAD_CRITICA);

  bool humoLeve = false, humoModerado = false, humoFuerte = false;
  if (aoDisponible && aoBase > 0) {
    if (porcentajeCambio >= PORCENTAJE_HUMO_FUERTE) humoFuerte = true;
    else if (porcentajeCambio >= PORCENTAJE_HUMO_MODERADO) humoModerado = true;
    else if (porcentajeCambio >= PORCENTAJE_HUMO_LEVE) humoLeve = true;
    if (humoDigital) humoModerado = true;
  } else {
    if (humoDigital) humoModerado = true;
  }

  bool condicionModerado = false, condicionAlerta = false;
  if (tempRiesgo || humRiesgo || humoLeve) condicionModerado = true;
  if (humoFuerte || (tempCritica && humCritica && (humoModerado || humoDigital))) condicionAlerta = true;

  Riesgo nuevoRiesgo = Riesgo::NORMAL;
  if (condicionAlerta) {
    contadorAlertaMaxima++;
    contadorRiesgoModerado = 0;
    if (contadorAlertaMaxima >= LECTURAS_PARA_ALERTA_MAXIMA) {
      nuevoRiesgo = Riesgo::ALERTA;
    } else {
      nuevoRiesgo = (riesgoActual == Riesgo::ALERTA) ? Riesgo::ALERTA : Riesgo::MODERADO;
    }
  } else if (condicionModerado) {
    contadorRiesgoModerado++;
    contadorAlertaMaxima = 0;
    if (contadorRiesgoModerado >= LECTURAS_PARA_RIESGO_MODERADO) {
      nuevoRiesgo = Riesgo::MODERADO;
    } else {
      nuevoRiesgo = (riesgoActual == Riesgo::ALERTA) ? Riesgo::ALERTA : Riesgo::MODERADO;
    }
  } else {
    contadorNormal++;
    if (contadorNormal >= LECTURAS_PARA_VOLVER_NORMAL) {
      nuevoRiesgo = Riesgo::NORMAL;
      contadorRiesgoModerado = 0;
      contadorAlertaMaxima = 0;
    } else {
      nuevoRiesgo = riesgoActual;
    }
  }

  if (nuevoRiesgo != riesgoActual) {
    riesgoActual = nuevoRiesgo;
    if (nuevoRiesgo == Riesgo::NORMAL) {
      contadorRiesgoModerado = 0; contadorAlertaMaxima = 0;
    }
    if (nuevoRiesgo == Riesgo::MODERADO) contadorAlertaMaxima = 0;
    contadorNormal = 0;
  }
}

// =============================================================
//  DIAGNÓSTICO SERIAL
// =============================================================
void serialDiagnostico() {
  Serial.println();
  Serial.println("================================================");
  Serial.println("              FORESTGUARD");
  Serial.println("================================================");
  if (dhtOK) {
    Serial.print("Temperatura : "); Serial.print(temperatura, 1); Serial.println(" C");
    Serial.print("Humedad     : "); Serial.print(humedad, 1); Serial.println(" %");
  } else {
    Serial.println("DHT22: SIN DATOS");
  }
  Serial.print("MQ-2 AO     : "); Serial.println(mq2AO);
  if (aoDisponible) {
    Serial.print("AO BASE     : "); Serial.println(aoBase);
    Serial.print("Cambio AO   : "); Serial.println(cambioAO);
    Serial.print("Porcentaje  : "); Serial.print(porcentajeCambio * 100, 1); Serial.println(" %");
  } else {
    Serial.println("AO no disponible (usando DO)");
  }
  Serial.print("MQ-2 DO     : "); Serial.println(mq2DO);
  Serial.print("Humo DO     : "); Serial.println(humoDigital ? "SI" : "NO");
  const char* estadoTexto;
  switch (riesgoActual) {
    case Riesgo::NORMAL:   estadoTexto = "NORMAL"; break;
    case Riesgo::MODERADO: estadoTexto = "RIESGO MODERADO"; break;
    case Riesgo::ALERTA:   estadoTexto = "ALERTA MAXIMA"; break;
    default: estadoTexto = "DESCONOCIDO";
  }
  Serial.print("Estado      : "); Serial.println(estadoTexto);
  Serial.print("WiFi        : "); Serial.println(wifiConectado ? "CONECTADO" : "DESCONECTADO");
  Serial.print("Flask       : "); Serial.println(flaskDisponible ? "DISPONIBLE" : "NO DISPONIBLE");
  Serial.print("Servidor IP : "); Serial.println(SERVER_IP);
  Serial.println("================================================");
}

// =============================================================
//  DESCUBRIMIENTO UDP (Broadcast) - CORREGIDO
// =============================================================
bool descubrirServidorUDP() {
  Serial.println("Buscando servidor Flask por UDP broadcast...");
  udp.begin(12345);
  IPAddress broadcastIp = WiFi.localIP();
  broadcastIp[3] = 255;  // broadcast de la subred
  udp.beginPacket(broadcastIp, 12345);
  // --- LÍNEA CORREGIDA: usamos print() en lugar de write() ---
  udp.print("FORESTGUARD_DISCOVER");
  udp.endPacket();

  unsigned long inicio = millis();
  while (millis() - inicio < TIMEOUT_DESCUBRIMIENTO) {
    int packetSize = udp.parsePacket();
    if (packetSize) {
      char buffer[64] = {0};
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        String respuesta = String(buffer);
        if (respuesta.startsWith("SERVER_IP:")) {
          String ip = respuesta.substring(10);
          ip.trim();
          if (ip.length() > 0) {
            SERVER_IP = ip;
            Serial.print("Servidor encontrado en IP: ");
            Serial.println(SERVER_IP);
            udp.stop();
            return true;
          }
        }
      }
    }
    delay(100);
  }
  udp.stop();
  Serial.println("No se encontró servidor por UDP. Usando IP por defecto.");
  return false;
}

// =============================================================
//  CONFIGURACIÓN MANUAL POR SERIAL
// =============================================================
void configurarIPPorSerial() {
  Serial.println("Escribe 'SETIP:xxx.xxx.xxx.xxx' para cambiar la IP del servidor.");
  Serial.println("Escribe 'CONTINUE' para usar la IP actual y continuar.");
  unsigned long inicio = millis();
  while (millis() - inicio < 30000) {  // espera 30 segundos
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      if (cmd.startsWith("SETIP:")) {
        String ip = cmd.substring(6);
        ip.trim();
        if (ip.length() > 0) {
          SERVER_IP = ip;
          Serial.print("IP actualizada a: ");
          Serial.println(SERVER_IP);
        }
      } else if (cmd == "CONTINUE") {
        Serial.println("Continuando con la IP actual.");
        return;
      }
    }
    delay(100);
  }
}

// =============================================================
//  ENVÍO A FLASK (MEJORADO)
// =============================================================
void enviarDatos() {
  if (!wifiConectado) {
    Serial.println("[HTTP] No se envía: WiFi desconectado");
    return;
  }

  HTTPClient http;
  String url = "http://" + SERVER_IP + ":" + String(SERVER_PORT) +
               "/api/estaciones/" + stationCode + "/datos";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(TIMEOUT_HTTP);

  JsonDocument doc;
  doc["api_key"] = DEFAULT_API_KEY;
  doc["temperatura"] = temperatura;
  doc["humedad"] = humedad;
  doc["mq2_ao"] = mq2AO;
  doc["mq2_base"] = aoBase;
  doc["cambio_ao"] = cambioAO;
  doc["mq2_do"] = mq2DO;
  doc["nivel_humo"] = (int)riesgoActual;
  doc["mac"] = stationCode;
  doc["porcentaje_cambio"] = porcentajeCambio;

  String payload;
  serializeJson(doc, payload);

  int codigo = http.POST(payload);
  if (codigo > 0) {
    Serial.print("[HTTP] Código: "); Serial.println(codigo);
    String respuesta = http.getString();
    if (codigo == 200 || codigo == 201) {
      flaskDisponible = true;
      ultimoEnvioExitoso = millis();
      Serial.println("[HTTP] Éxito");
    } else if (codigo == 403) {
      Serial.println("[HTTP] Error 403: Estación pendiente de aprobación. Ve al panel de administración y aprueba la estación.");
      flaskDisponible = false;
    } else if (codigo == 401) {
      Serial.println("[HTTP] Error 401: API key inválida. Verifica DEFAULT_API_KEY.");
      flaskDisponible = false;
    } else if (codigo == 404) {
      Serial.println("[HTTP] Error 404: Estación no registrada. El backend la registrará automáticamente si la API key es correcta.");
      flaskDisponible = false;
    } else {
      Serial.print("[HTTP] Error del servidor: "); Serial.println(codigo);
      flaskDisponible = false;
    }
    if (codigo != 200 && codigo != 201) {
      Serial.print("Respuesta: "); Serial.println(respuesta);
    }
  } else {
    Serial.print("[HTTP] Fallo: "); Serial.println(http.errorToString(codigo));
    flaskDisponible = false;
    if (codigo == -1) {
      Serial.println("Posiblemente el servidor no está accesible. Verifica IP y firewall.");
    }
  }
  http.end();
}

// =============================================================
//  WIFI (CON RECONEXIÓN)
// =============================================================
void conectarWiFi() {
  Serial.println();
  Serial.println("======================================");
  Serial.println("             CONEXIÓN WIFI");
  Serial.println("======================================");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando");
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    delay(500);
    Serial.print(".");
    intentos++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WIFI -> CONECTADO");
    Serial.print("IP ESP32: "); Serial.println(WiFi.localIP());
    wifiConectado = true;
  } else {
    Serial.println("WIFI -> ERROR");
    wifiConectado = false;
  }
}

void comprobarWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConectado = true;
    return;
  }
  wifiConectado = false;
  unsigned long ahora = millis();
  if (ahora - ultimoIntentoWiFi < TIEMPO_RECONEXION_WIFI) return;
  ultimoIntentoWiFi = ahora;
  Serial.println("WiFi desconectado, reconectando...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 10000) {
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi reconectado");
    wifiConectado = true;
  } else {
    Serial.println("No se pudo reconectar WiFi");
  }
}

// =============================================================
//  SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("           FORESTGUARD v2.2");
  Serial.println("      SISTEMA DE MONITOREO");
  Serial.println("======================================");

  // LED PWM
  ledcAttach(LED_ROJO, FRECUENCIA, RESOLUCION);
  ledcAttach(LED_VERDE, FRECUENCIA, RESOLUCION);
  ledcAttach(LED_AZUL, FRECUENCIA, RESOLUCION);
  ledApagado();

  pinMode(MQ2_AO, INPUT);
  pinMode(MQ2_DO, INPUT);
  dht.begin();

  // Secuencia LED
  ledRojo(); delay(300);
  ledVerde(); delay(300);
  ledAzul(); delay(300);
  ledAmarillo(); delay(300);
  ledApagado();

  // Calibración MQ-2
  calibrarMQ2();

  // Conectar WiFi
  conectarWiFi();
  if (wifiConectado) {
    // Descubrir servidor
    if (!descubrirServidorUDP()) {
      // Si falla, dar opción de configurar manualmente por serial
      configurarIPPorSerial();
    }
  } else {
    Serial.println("No hay WiFi, no se puede descubrir servidor.");
  }

  // Obtener MAC
  stationCode = WiFi.macAddress();
  stationCode.replace(":", "");
  Serial.print("Código estación: "); Serial.println(stationCode);

  // Inicializar buffers
  for (int i = 0; i < MUESTRAS_PROMEDIO_TEMP; i++) {
    tempBuffer[i] = 0; humBuffer[i] = 0;
  }
  for (int i = 0; i < MUESTRAS_PROMEDIO_AO; i++) {
    aoBuffer[i] = 0;
  }

  Serial.println();
  Serial.println("======================================");
  Serial.println("       FORESTGUARD LISTO");
  Serial.println("======================================");
  Serial.println("MONITOREO ACTIVO");
  Serial.println("ENVÍO A FLASK: CADA 5 SEGUNDOS");
  Serial.print("SERVIDOR IP: "); Serial.println(SERVER_IP);
  Serial.println();
}

// =============================================================
//  LOOP PRINCIPAL
// =============================================================
void loop() {
  static unsigned long ultimoSerial = 0;
  static unsigned long ultimoEnvio = 0;
  static unsigned long ultimaLectura = 0;

  comprobarWiFi();

  if (millis() - ultimaLectura >= INTERVALO_LECTURA) {
    ultimaLectura = millis();
    leerSensores();
    evaluarRiesgo();
    actualizarLED();
  }

  if (millis() - ultimoSerial >= 2000) {
    ultimoSerial = millis();
    serialDiagnostico();
  }

  if (millis() - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = millis();
    enviarDatos();
  }

  delay(50);
}