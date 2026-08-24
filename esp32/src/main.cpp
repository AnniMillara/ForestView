// =============================================================
//               FORESTGUARD ESP32 - FIRMWARE V2
//               Robusto, confiable y seguro
// =============================================================
//  Hardware: ESP32 + DHT (3 pines, compatible DHT11/DHT22)
//            + MQ-2 + LED RGB
//  Backend: Flask + MySQL (sin cambios)
// =============================================================
//  LED:
//    🔵 AZUL FIJO     = Calibración / calentamiento
//    🔵 AZUL PARP.    = Error de sensor (DHT o MQ-2)
//    🟢 VERDE         = NORMAL
//    🟡 AMARILLO      = RIESGO MODERADO
//    🟠 NARANJO       = RIESGO ALTO
//    🔴 ROJO          = ALERTA (posible incendio)
// =============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// =============================================================
//  PINES (MANTENER)
// =============================================================
#define DHT_PIN     13
#define DHT_TYPE    DHT22           // Cambia a DHT11 si usas ese modelo
#define MQ2_AO      35
#define MQ2_DO      33
#define LED_ROJO    26
#define LED_VERDE   27
#define LED_AZUL    25

// =============================================================
//  WIFI (dos redes, prioridad)
// =============================================================
// Red principal
const char* WIFI_SSID_1     = "GameofThrones";
const char* WIFI_PASS_1     = "elsenordelosanillos";
// Red secundaria
const char* WIFI_SSID_2     = "B3ar";
const char* WIFI_PASS_2     = "papyrusB3st";

// Lista de redes (orden de prioridad)
const int NUM_NETWORKS = 2;
const char* WIFI_SSIDS[]   = { WIFI_SSID_1, WIFI_SSID_2 };
const char* WIFI_PASSWORDS[] = { WIFI_PASS_1, WIFI_PASS_2 };

// =============================================================
//  SERVIDOR (MANTENER)
// =============================================================
const char* SERVER_IP       = "192.168.18.116";
const int   SERVER_PORT     = 5000;
const char* DEFAULT_API_KEY = "3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c";

// =============================================================
//  PWM LED
// =============================================================
const int FRECUENCIA  = 5000;
const int RESOLUCION  = 8;
const int CANAL_ROJO  = 0;
const int CANAL_VERDE = 1;
const int CANAL_AZUL  = 2;

// =============================================================
//  UMBRALES (sin cambios)
// =============================================================
const float TEMP_RIESGO      = 30.0;
const float TEMP_ALTA        = 35.0;
const float TEMP_CRITICA     = 40.0;
const float HUMEDAD_RIESGO   = 45.0;
const float HUMEDAD_BAJA     = 30.0;
const float HUMEDAD_CRITICA  = 15.0;

const int HUMO_CAMBIO_BAJO   = 80;
const int HUMO_CAMBIO_MEDIO  = 200;
const int HUMO_CAMBIO_ALTO   = 400;

const int HYSTERESIS_BAJO    = 60;
const int HYSTERESIS_MEDIO   = 150;
const int HYSTERESIS_ALTO    = 320;

// =============================================================
//  CONSTANTES DE TIEMPO
// =============================================================
const unsigned long INTERVALO_LECTURA   = 2000;      // 2 s
const unsigned long INTERVALO_ENVIO     = 5000;      // 5 s
const unsigned long TIEMPO_CALENTAMIENTO_MQ2 = 60000; // 60 s
const unsigned long INTERVALO_RECALIB   = 600000;    // 10 min
const unsigned long TIMEOUT_HTTP        = 5000;      // 5 s

const int MUESTRAS_PROMEDIO_TEMP = 5;
const int MUESTRAS_PROMEDIO_AO   = 10;
const int VENTANA_TENDENCIA      = 6;

const int LECTURAS_PARA_RIESGO_MODERADO = 3;
const int LECTURAS_PARA_RIESGO_ALTO     = 4;
const int LECTURAS_PARA_ALERTA          = 5;

// =============================================================
//  VARIABLES GLOBALES
// =============================================================
DHT dht(DHT_PIN, DHT_TYPE);

String stationCode = "";
String connectedSSID = "";   // SSID al que estamos conectados

// ---- LED ----
enum class LedState : uint8_t {
  NORMAL, RIESGO_MODERADO, RIESGO_ALTO, ALERTA, ERROR_SENSOR, CALIBRACION
};
LedState ledState = LedState::NORMAL;

// ---- MQ-2 ----
int aoBase = 0;
int estadoDONormal = HIGH;
unsigned long ultimaCalibracion = 0;
bool recalibracionPendiente = false;
bool mq2Precalentado = false;
unsigned long inicioPrecalentamiento = 0;

// ---- Buffers ----
float tempBuffer[MUESTRAS_PROMEDIO_TEMP];
float humBuffer[MUESTRAS_PROMEDIO_TEMP];
int idxTemp = 0;
bool tempBufferLleno = false;

int aoBuffer[MUESTRAS_PROMEDIO_AO];
int idxAO = 0;
bool aoBufferLleno = false;

int ultimosCambios[VENTANA_TENDENCIA];
int idxTendencia = 0;
bool tendenciaLlena = false;

float ultimaTempValida = 25.0;
float ultimaHumValida = 50.0;
int ultimoAOValido = 0;

// ---- Estado de sensores ----
enum class SensorStatus : uint8_t { OK, ERROR, DESCONOCIDO };
SensorStatus statusDHT = SensorStatus::DESCONOCIDO;
SensorStatus statusMQ2 = SensorStatus::DESCONOCIDO;
int fallosDHT = 0;
const int MAX_FALLOS_DHT = 5;
unsigned long ultimaLecturaDHTok = 0;

// ---- Humo ----
enum class NivelHumo : uint8_t { NINGUNO, BAJO, MEDIO, ALTO };
NivelHumo nivelHumoEstable = NivelHumo::NINGUNO;
int contadorHumo[4] = {0,0,0,0};

// ---- Riesgo ----
enum class Riesgo : uint8_t { NORMAL, MODERADO, ALTO, ALERTA };
Riesgo riesgoActual = Riesgo::NORMAL;
int confianza = 0;
int contadorRiesgo[4] = {0,0,0,0};
Riesgo ultimoRiesgoEstable = Riesgo::NORMAL;

// ---- WiFi ----
bool wifiConectado = false;
unsigned long ultimoIntentoWiFi = 0;
int redActual = 0;               // índice de la red que se está intentando

// ---- Flask ----
bool flaskDisponible = false;
unsigned long ultimoEnvioExitoso = 0;

// =============================================================
//  PROTOTIPOS (declaraciones anticipadas)
// =============================================================
void actualizarLED();
void calibrarMQ2(const char* motivo, bool forzado = false);
bool deberiaRecalibrar();
int calcularTendencia();
NivelHumo clasificarHumo(int cambio, bool humoDO);
void evaluarRiesgo(float temp, float hum, NivelHumo nivel, int tendencia);
void enviarDatos(float temp, float hum, int ao, int base, int cambio, int doVal, NivelHumo nivel, int conf, Riesgo riesgo);
void conectarWiFi();
void comprobarWiFi();
void serialDiagnostico(float temp, float hum, int ao, int base, int cambio, int doVal, NivelHumo nivel, int tendencia, int conf, Riesgo riesgo);
bool leerDHT22(float &temp, float &hum);
// NUEVOS PROTOTIPOS (añadidos para evitar error de compilación)
float obtenerPromedioTemp(bool humedad);
int obtenerPromedioAO();

// =============================================================
//  FUNCIONES LED
// =============================================================
void ledApagado() {
  ledcWrite(CANAL_ROJO, 0);
  ledcWrite(CANAL_VERDE, 0);
  ledcWrite(CANAL_AZUL, 0);
}
void ledVerde() {
  ledcWrite(CANAL_ROJO, 0);
  ledcWrite(CANAL_VERDE, 255);
  ledcWrite(CANAL_AZUL, 0);
}
void ledRojo() {
  ledcWrite(CANAL_ROJO, 255);
  ledcWrite(CANAL_VERDE, 0);
  ledcWrite(CANAL_AZUL, 0);
}
void ledAzul() {
  ledcWrite(CANAL_ROJO, 0);
  ledcWrite(CANAL_VERDE, 0);
  ledcWrite(CANAL_AZUL, 255);
}
void ledAmarillo() {
  ledcWrite(CANAL_ROJO, 255);
  ledcWrite(CANAL_VERDE, 70);
  ledcWrite(CANAL_AZUL, 0);
}
void ledNaranjo() {
  ledcWrite(CANAL_ROJO, 255);
  ledcWrite(CANAL_VERDE, 25);
  ledcWrite(CANAL_AZUL, 0);
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
  if (statusDHT == SensorStatus::ERROR || statusMQ2 == SensorStatus::ERROR) {
    ledParpadeoAzul();
    return;
  }
  if (ledState == LedState::CALIBRACION || !mq2Precalentado) {
    ledAzul();
    return;
  }
  switch (ledState) {
    case LedState::ALERTA:          ledRojo(); break;
    case LedState::RIESGO_ALTO:     ledNaranjo(); break;
    case LedState::RIESGO_MODERADO: ledAmarillo(); break;
    default:                        ledVerde(); break;
  }
}

// =============================================================
//  CALIBRACIÓN MQ-2
// =============================================================
void precalentarMQ2() {
  if (mq2Precalentado) return;
  if (inicioPrecalentamiento == 0) {
    inicioPrecalentamiento = millis();
    Serial.println("[MQ-2] Iniciando precalentamiento de 60 segundos...");
    ledState = LedState::CALIBRACION;
    actualizarLED();
  }
  unsigned long transcurrido = millis() - inicioPrecalentamiento;
  if (transcurrido < TIEMPO_CALENTAMIENTO_MQ2) {
    if (transcurrido % 5000 < 100) {
      Serial.print("[MQ-2] Calentando... ");
      Serial.print(transcurrido / 1000);
      Serial.println("s / 60s");
    }
    return;
  }
  mq2Precalentado = true;
  Serial.println("[MQ-2] Precalentamiento completado.");
  calibrarMQ2("INICIAL", true);
}

void calibrarMQ2(const char* motivo, bool forzado) {
  if (!forzado && !deberiaRecalibrar()) {
    Serial.println("[CALIB] Recalibración pospuesta (condiciones inestables)");
    recalibracionPendiente = true;
    return;
  }
  recalibracionPendiente = false;
  ledState = LedState::CALIBRACION;
  actualizarLED();

  Serial.println();
  Serial.println("======================================");
  Serial.print("   CALIBRANDO MQ-2: ");
  Serial.println(motivo);
  Serial.println("======================================");
  Serial.println("Asegúrate de que el sensor esté en aire limpio.");
  delay(100);

  const int NUM_MUESTRAS = 20;
  long sumaAO = 0;
  int high = 0, low = 0;
  int muestrasValidas = 0;

  for (int i = 0; i < NUM_MUESTRAS; i++) {
    int ao = analogRead(MQ2_AO);
    int doVal = digitalRead(MQ2_DO);
    if (ao >= 0 && ao <= 4095) {
      sumaAO += ao;
      muestrasValidas++;
    }
    if (doVal == HIGH) high++; else low++;
    delay(100);
  }

  if (muestrasValidas == 0) {
    Serial.println("ERROR: No se pudieron leer muestras válidas de AO. Manteniendo calibración anterior.");
    ledState = LedState::NORMAL;
    actualizarLED();
    return;
  }

  aoBase = sumaAO / muestrasValidas;
  estadoDONormal = (high >= low) ? HIGH : LOW;
  ultimoAOValido = aoBase;

  Serial.println("--------------------------------------");
  Serial.print("AO BASE     = "); Serial.println(aoBase);
  Serial.print("DO NORMAL   = "); Serial.println(estadoDONormal);
  Serial.println("--------------------------------------");
  Serial.println("CALIBRACION TERMINADA");
  ultimaCalibracion = millis();
  ledState = LedState::NORMAL;
  actualizarLED();
}

bool deberiaRecalibrar() {
  if (riesgoActual != Riesgo::NORMAL) return false;
  if (nivelHumoEstable != NivelHumo::NINGUNO) return false;
  return true;
}

// =============================================================
//  LECTURA DHT
// =============================================================
bool leerDHT22(float &temp, float &hum) {
  temp = dht.readTemperature();
  hum = dht.readHumidity();
  if (!isnan(temp) && !isnan(hum) && temp >= -40 && temp <= 80 && hum >= 0 && hum <= 100) {
    return true;
  }
  return false;
}

// =============================================================
//  FILTROS CON OUTLIER
// =============================================================
void agregarMuestraTemp(float temp, float hum) {
  if (tempBufferLleno) {
    float promTemp = obtenerPromedioTemp(false);
    if (fabs(temp - promTemp) > 0.2 * promTemp && promTemp > 0) {
      return;
    }
  }
  tempBuffer[idxTemp] = temp;
  humBuffer[idxTemp] = hum;
  idxTemp = (idxTemp + 1) % MUESTRAS_PROMEDIO_TEMP;
  if (idxTemp == 0) tempBufferLleno = true;
}

float obtenerPromedioTemp(bool humedad) {
  int n = tempBufferLleno ? MUESTRAS_PROMEDIO_TEMP : idxTemp;
  if (n == 0) return humedad ? ultimaHumValida : ultimaTempValida;
  float suma = 0;
  for (int i = 0; i < n; i++) {
    suma += humedad ? humBuffer[i] : tempBuffer[i];
  }
  return suma / n;
}

void agregarMuestraAO(int ao) {
  if (aoBufferLleno) {
    int prom = obtenerPromedioAO();
    if (abs(ao - prom) > 0.3 * prom && prom > 0) {
      return;
    }
  }
  aoBuffer[idxAO] = ao;
  idxAO = (idxAO + 1) % MUESTRAS_PROMEDIO_AO;
  if (idxAO == 0) aoBufferLleno = true;
}

int obtenerPromedioAO() {
  int n = aoBufferLleno ? MUESTRAS_PROMEDIO_AO : idxAO;
  if (n == 0) return ultimoAOValido;
  long suma = 0;
  for (int i = 0; i < n; i++) {
    suma += aoBuffer[i];
  }
  return suma / n;
}

// =============================================================
//  TENDENCIA
// =============================================================
int calcularTendencia() {
  int n = tendenciaLlena ? VENTANA_TENDENCIA : idxTendencia;
  if (n < 2) return 0;
  long sumaX = 0, sumaY = 0, sumaXY = 0, sumaX2 = 0;
  for (int i = 0; i < n; i++) {
    int x = i;
    int y = ultimosCambios[i];
    sumaX += x;
    sumaY += y;
    sumaXY += x * y;
    sumaX2 += x * x;
  }
  float denominador = (n * sumaX2 - sumaX * sumaX);
  if (denominador == 0) return 0;
  float pendiente = (n * sumaXY - sumaX * sumaY) / denominador;
  return (int)(pendiente * 100);
}

// =============================================================
//  CLASIFICACIÓN HUMO
// =============================================================
NivelHumo clasificarHumo(int cambio, bool humoDO) {
  NivelHumo nivel = NivelHumo::NINGUNO;
  if (cambio >= HUMO_CAMBIO_ALTO || humoDO) nivel = NivelHumo::ALTO;
  else if (cambio >= HUMO_CAMBIO_MEDIO) nivel = NivelHumo::MEDIO;
  else if (cambio >= HUMO_CAMBIO_BAJO) nivel = NivelHumo::BAJO;

  if (nivel == NivelHumo::NINGUNO) {
    if (nivelHumoEstable == NivelHumo::BAJO && cambio >= HYSTERESIS_BAJO) nivel = NivelHumo::BAJO;
    else if (nivelHumoEstable == NivelHumo::MEDIO && cambio >= HYSTERESIS_MEDIO) nivel = NivelHumo::MEDIO;
    else if (nivelHumoEstable == NivelHumo::ALTO && cambio >= HYSTERESIS_ALTO) nivel = NivelHumo::ALTO;
  }

  int idxNivel = (int)nivel;
  contadorHumo[idxNivel]++;
  for (int i = 0; i < 4; i++) {
    if (i != idxNivel) contadorHumo[i] = 0;
  }

  int umbral;
  switch (nivel) {
    case NivelHumo::BAJO:  umbral = LECTURAS_PARA_RIESGO_MODERADO; break;
    case NivelHumo::MEDIO: umbral = LECTURAS_PARA_RIESGO_ALTO; break;
    case NivelHumo::ALTO:  umbral = LECTURAS_PARA_ALERTA; break;
    default:               umbral = 2; break;
  }

  if (contadorHumo[idxNivel] >= umbral) {
    nivelHumoEstable = nivel;
  } else {
    if (nivel == NivelHumo::NINGUNO && contadorHumo[0] >= 2) {
      nivelHumoEstable = NivelHumo::NINGUNO;
    }
  }
  return nivelHumoEstable;
}

// =============================================================
//  EVALUACIÓN DE RIESGO
// =============================================================
void evaluarRiesgo(float temp, float hum, NivelHumo nivel, int tendencia) {
  bool tempRiesgo = temp >= TEMP_RIESGO;
  bool tempAlta   = temp >= TEMP_ALTA;
  bool tempCritica= temp >= TEMP_CRITICA;
  bool humRiesgo  = hum <= HUMEDAD_RIESGO;
  bool humBaja    = hum <= HUMEDAD_BAJA;
  bool humCritica = hum <= HUMEDAD_CRITICA;

  bool alerta = false, alto = false, moderado = false;
  int conf = 0;

  if (nivel == NivelHumo::ALTO) {
    alerta = true; conf = 80;
  }
  else if (nivel == NivelHumo::MEDIO && tempAlta) {
    alerta = true; conf = 75;
  }
  else if (nivel == NivelHumo::MEDIO && humBaja) {
    alerta = true; conf = 70;
  }
  else if (nivel == NivelHumo::BAJO && tempCritica) {
    alerta = true; conf = 70;
  }
  else if (nivel == NivelHumo::BAJO && humCritica) {
    alerta = true; conf = 65;
  }
  else if (tempCritica && humBaja) {
    alerta = true; conf = 60;
  }
  else if (nivel != NivelHumo::NINGUNO && tempAlta && humBaja) {
    alerta = true; conf = 75;
  }

  if (!alerta) {
    if (nivel == NivelHumo::MEDIO) {
      alto = true; conf = 55;
    }
    else if (nivel == NivelHumo::BAJO && tempAlta) {
      alto = true; conf = 50;
    }
    else if (nivel == NivelHumo::BAJO && humBaja) {
      alto = true; conf = 45;
    }
    else if (tempAlta && humBaja) {
      alto = true; conf = 40;
    }
  }

  if (!alerta && !alto) {
    if (nivel == NivelHumo::BAJO) {
      moderado = true; conf = 30;
    }
    else if (tempRiesgo) {
      moderado = true; conf = 25;
    }
    else if (humRiesgo) {
      moderado = true; conf = 20;
    }
  }

  if (tendencia > 0) conf = min(100, conf + 10);
  if (tendencia < -50) conf = max(0, conf - 10);
  confianza = conf;

  Riesgo nuevoRiesgo = Riesgo::NORMAL;
  if (alerta) nuevoRiesgo = Riesgo::ALERTA;
  else if (alto) nuevoRiesgo = Riesgo::ALTO;
  else if (moderado) nuevoRiesgo = Riesgo::MODERADO;

  int idx = (int)nuevoRiesgo;
  contadorRiesgo[idx]++;
  for (int i = 0; i < 4; i++) {
    if (i != idx) contadorRiesgo[i] = 0;
  }

  int umbralSubida = 2;
  int umbralBajada = 3;

  if (contadorRiesgo[idx] >= umbralSubida && nuevoRiesgo > ultimoRiesgoEstable) {
    riesgoActual = nuevoRiesgo;
    ultimoRiesgoEstable = nuevoRiesgo;
    for (int i = 0; i < 4; i++) contadorRiesgo[i] = 0;
  } else if (contadorRiesgo[0] >= umbralBajada && nuevoRiesgo == Riesgo::NORMAL && ultimoRiesgoEstable != Riesgo::NORMAL) {
    riesgoActual = Riesgo::NORMAL;
    ultimoRiesgoEstable = Riesgo::NORMAL;
    for (int i = 0; i < 4; i++) contadorRiesgo[i] = 0;
  } else if (contadorRiesgo[(int)ultimoRiesgoEstable] >= umbralSubida && nuevoRiesgo == ultimoRiesgoEstable) {
    contadorRiesgo[(int)ultimoRiesgoEstable] = 0;
  }

  if (statusDHT == SensorStatus::OK && statusMQ2 == SensorStatus::OK) {
    switch (riesgoActual) {
      case Riesgo::ALERTA:  ledState = LedState::ALERTA; break;
      case Riesgo::ALTO:    ledState = LedState::RIESGO_ALTO; break;
      case Riesgo::MODERADO: ledState = LedState::RIESGO_MODERADO; break;
      default:              ledState = LedState::NORMAL; break;
    }
  } else {
    ledState = LedState::ERROR_SENSOR;
  }
}

// =============================================================
//  ENVÍO A FLASK
// =============================================================
void enviarDatos(float temp, float hum, int ao, int base, int cambio, int doVal, NivelHumo nivel, int conf, Riesgo riesgo) {
  if (!wifiConectado) {
    Serial.println("[HTTP] No se envía: WiFi desconectado");
    return;
  }

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) +
               "/api/estaciones/" + stationCode + "/datos";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(TIMEOUT_HTTP);

  JsonDocument doc;
  doc["api_key"] = DEFAULT_API_KEY;
  doc["temperatura"] = temp;
  doc["humedad"] = hum;
  doc["mq2_ao"] = ao;
  doc["mq2_base"] = base;
  doc["cambio_ao"] = cambio;
  doc["mq2_do"] = doVal;
  doc["nivel_humo"] = (int)nivel;
  doc["mac"] = stationCode;
  doc["confianza"] = conf;
  doc["riesgo"] = (int)riesgo;

  String payload;
  serializeJson(doc, payload);

  int codigo = http.POST(payload);
  if (codigo > 0) {
    Serial.print("[HTTP] Código: "); Serial.println(codigo);
    if (codigo == 200 || codigo == 201) {
      flaskDisponible = true;
      ultimoEnvioExitoso = millis();
    } else if (codigo >= 400) {
      Serial.println("[HTTP] Error del servidor");
      flaskDisponible = false;
    }
  } else {
    Serial.print("[HTTP] Falló: "); Serial.println(http.errorToString(codigo));
    flaskDisponible = false;
  }
  http.end();
}

// =============================================================
//  WIFI (MULTI-RED)
// =============================================================
void conectarWiFi() {
  Serial.println();
  Serial.println("======================================");
  Serial.println("             CONEXION WIFI");
  Serial.println("======================================");
  WiFi.mode(WIFI_STA);
  // Intentar cada red en orden de prioridad
  for (int i = 0; i < NUM_NETWORKS; i++) {
    Serial.print("Intentando con: ");
    Serial.println(WIFI_SSIDS[i]);
    WiFi.begin(WIFI_SSIDS[i], WIFI_PASSWORDS[i]);
    int intentos = 0;
    while (WiFi.status() != WL_CONNECTED && intentos < 20) {
      delay(500);
      Serial.print(".");
      intentos++;
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WIFI -> CONECTADO a ");
      Serial.println(WIFI_SSIDS[i]);
      connectedSSID = WIFI_SSIDS[i];
      Serial.print("IP ESP32: "); Serial.println(WiFi.localIP());
      wifiConectado = true;
      redActual = i;
      return;
    }
  }
  // Si ninguna funciona
  Serial.println("WIFI -> ERROR: no se pudo conectar a ninguna red");
  wifiConectado = false;
  connectedSSID = "";
}

void comprobarWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConectado = true;
    return;
  }
  wifiConectado = false;
  unsigned long ahora = millis();
  if (ahora - ultimoIntentoWiFi < 10000) return;
  ultimoIntentoWiFi = ahora;

  Serial.println("WIFI DESCONECTADO, reconectando...");
  WiFi.disconnect();
  // Recorrer todas las redes en orden
  for (int i = 0; i < NUM_NETWORKS; i++) {
    Serial.print("Intentando con: ");
    Serial.println(WIFI_SSIDS[i]);
    WiFi.begin(WIFI_SSIDS[i], WIFI_PASSWORDS[i]);
    int intentos = 0;
    while (WiFi.status() != WL_CONNECTED && intentos < 15) {  // menos intentos para no bloquear mucho
      delay(300);
      intentos++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Reconectado a ");
      Serial.println(WIFI_SSIDS[i]);
      connectedSSID = WIFI_SSIDS[i];
      wifiConectado = true;
      redActual = i;
      return;
    }
  }
  Serial.println("No se pudo reconectar a ninguna red");
}

// =============================================================
//  DIAGNÓSTICO SERIAL (con SSID)
// =============================================================
void serialDiagnostico(float temp, float hum, int ao, int base, int cambio, int doVal, NivelHumo nivel, int tendencia, int conf, Riesgo riesgo) {
  Serial.println();
  Serial.println("======================================");
  Serial.println("        FORESTGUARD - DIAGNÓSTICO");
  Serial.println("======================================");

  Serial.print("Temp         : "); Serial.print(temp, 1); Serial.println(" °C");
  Serial.print("Humedad      : "); Serial.print(hum, 1); Serial.println(" %");
  Serial.print("DHT Estado   : "); 
  if (statusDHT == SensorStatus::OK) Serial.println("OK");
  else if (statusDHT == SensorStatus::ERROR) Serial.println("ERROR (fallos: " + String(fallosDHT) + ")");
  else Serial.println("DESCONOCIDO");

  Serial.print("MQ-2 AO      : "); Serial.println(ao);
  Serial.print("AO BASE      : "); Serial.println(base);
  Serial.print("CAMBIO       : "); Serial.println(cambio);
  Serial.print("CAMBIO %     : "); 
  if (base != 0) Serial.print((cambio * 100) / base); else Serial.print("N/A");
  Serial.println("%");
  Serial.print("MQ-2 DO      : "); Serial.println(doVal);
  Serial.print("MQ-2 Estado  : ");
  if (statusMQ2 == SensorStatus::OK) Serial.println("OK");
  else if (statusMQ2 == SensorStatus::ERROR) Serial.println("ERROR");
  else Serial.println("DESCONOCIDO");

  const char* niveles[] = {"NINGUNO","BAJO","MEDIO","ALTO"};
  Serial.print("NIVEL HUMO   : "); Serial.println(niveles[(int)nivel]);

  const char* tendencias[] = {"BAJANDO","ESTABLE","SUBIENDO"};
  int idxTend = (tendencia < -10) ? 0 : (tendencia > 10 ? 2 : 1);
  Serial.print("TENDENCIA    : "); Serial.println(tendencias[idxTend]);

  Serial.print("CONFIANZA    : "); Serial.print(conf); Serial.println("%");

  const char* estados[] = {"NORMAL","MODERADO","ALTO","ALERTA"};
  Serial.print("RIESGO       : "); Serial.println(estados[(int)riesgo]);

  Serial.print("WIFI         : "); Serial.println(wifiConectado ? "CONECTADO" : "DESCONECTADO");
  if (wifiConectado) {
    Serial.print("SSID         : "); Serial.println(connectedSSID);
    Serial.print("IP           : "); Serial.println(WiFi.localIP());
    Serial.print("RSSI         : "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
  }
  Serial.print("FLASK        : "); Serial.println(flaskDisponible ? "DISPONIBLE" : "NO DISPONIBLE");
  Serial.print("MQ-2 CALIB.  : "); Serial.println(mq2Precalentado ? "LISTO" : "CALENTANDO...");
  Serial.println("======================================");
}

// =============================================================
//  SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("           FORESTGUARD V2");
  Serial.println("      SISTEMA DE MONITOREO");
  Serial.println("======================================");

  ledcSetup(CANAL_ROJO, FRECUENCIA, RESOLUCION);
  ledcSetup(CANAL_VERDE, FRECUENCIA, RESOLUCION);
  ledcSetup(CANAL_AZUL, FRECUENCIA, RESOLUCION);
  ledcAttachPin(LED_ROJO, CANAL_ROJO);
  ledcAttachPin(LED_VERDE, CANAL_VERDE);
  ledcAttachPin(LED_AZUL, CANAL_AZUL);
  ledApagado();

  pinMode(MQ2_AO, INPUT);
  pinMode(MQ2_DO, INPUT);
  dht.begin();

  ledRojo(); delay(300);
  ledVerde(); delay(300);
  ledAzul(); delay(300);
  ledAmarillo(); delay(300);
  ledNaranjo(); delay(300);
  ledApagado();
  Serial.println("[DIAG] LED RGB OK");

  float t, h;
  if (leerDHT22(t, h)) {
    Serial.println("[DIAG] DHT22 OK");
    statusDHT = SensorStatus::OK;
    ultimaTempValida = t;
    ultimaHumValida = h;
    ultimaLecturaDHTok = millis();
    for (int i = 0; i < MUESTRAS_PROMEDIO_TEMP; i++) {
      tempBuffer[i] = t;
      humBuffer[i] = h;
    }
    tempBufferLleno = true;
  } else {
    Serial.println("[DIAG] DHT22 ERROR (comprueba conexión)");
    statusDHT = SensorStatus::ERROR;
    for (int i = 0; i < MUESTRAS_PROMEDIO_TEMP; i++) {
      tempBuffer[i] = ultimaTempValida;
      humBuffer[i] = ultimaHumValida;
    }
    tempBufferLleno = true;
  }

  int ao = analogRead(MQ2_AO);
  int doVal = digitalRead(MQ2_DO);
  if (ao >= 0 && ao <= 4095) {
    Serial.println("[DIAG] MQ-2 AO OK");
    statusMQ2 = SensorStatus::OK;
    ultimoAOValido = ao;
    for (int i = 0; i < MUESTRAS_PROMEDIO_AO; i++) aoBuffer[i] = ao;
    aoBufferLleno = true;
  } else {
    Serial.println("[DIAG] MQ-2 AO fuera de rango");
    statusMQ2 = SensorStatus::ERROR;
  }
  Serial.print("[DIAG] MQ-2 DO = "); Serial.println(doVal);

  precalentarMQ2();  // inicia calentamiento y calibración

  conectarWiFi();

  stationCode = WiFi.macAddress();
  stationCode.replace(":", "");
  Serial.print("[DIAG] Código estación: "); Serial.println(stationCode);

  for (int i = 0; i < VENTANA_TENDENCIA; i++) ultimosCambios[i] = 0;

  Serial.println();
  Serial.println("======================================");
  Serial.println("       FORESTGUARD LISTO");
  Serial.println("======================================");
  Serial.println("MONITOREO ACTIVO (precalentando MQ-2)");
  Serial.println("ENVIO A FLASK: CADA 5 SEGUNDOS");
  Serial.println("RECALIBRACION: CONDICIONAL");
  Serial.println();
}

// =============================================================
//  LOOP
// =============================================================
void loop() {
  static unsigned long ultimoSerial = 0;
  static unsigned long ultimoEnvio = 0;
  static unsigned long ultimaLectura = 0;

  comprobarWiFi();

  if (!mq2Precalentado) {
    precalentarMQ2();
    actualizarLED();
    return;
  }

  if (millis() - ultimaCalibracion > INTERVALO_RECALIB && !recalibracionPendiente) {
    if (deberiaRecalibrar()) {
      calibrarMQ2("PERIODICA", false);
    } else {
      recalibracionPendiente = true;
    }
  }

  if (millis() - ultimaLectura < INTERVALO_LECTURA) {
    actualizarLED();
    return;
  }
  ultimaLectura = millis();

  float temp, hum;
  if (leerDHT22(temp, hum)) {
    statusDHT = SensorStatus::OK;
    fallosDHT = 0;
    ultimaLecturaDHTok = millis();
    ultimaTempValida = temp;
    ultimaHumValida = hum;
    agregarMuestraTemp(temp, hum);
  } else {
    fallosDHT++;
    if (fallosDHT >= MAX_FALLOS_DHT) {
      statusDHT = SensorStatus::ERROR;
      Serial.println("[DHT] Error crítico: sensor no responde.");
    }
  }

  int aoRaw = analogRead(MQ2_AO);
  if (aoRaw < 0 || aoRaw > 4095) {
    statusMQ2 = SensorStatus::ERROR;
    Serial.println("[MQ-2] Lectura AO fuera de rango");
  } else {
    statusMQ2 = SensorStatus::OK;
    ultimoAOValido = aoRaw;
    agregarMuestraAO(aoRaw);
  }

  int aoFiltrado = obtenerPromedioAO();
  int doVal = digitalRead(MQ2_DO);
  int cambio = abs(aoFiltrado - aoBase);
  bool humoDO = (doVal != estadoDONormal);

  static int ultimoCambio = 0;
  if (cambio != ultimoCambio) {
    ultimosCambios[idxTendencia] = cambio;
    idxTendencia = (idxTendencia + 1) % VENTANA_TENDENCIA;
    if (idxTendencia == 0) tendenciaLlena = true;
    ultimoCambio = cambio;
  }
  int tendencia = calcularTendencia();

  NivelHumo nivel = clasificarHumo(cambio, humoDO);

  float tempProm = obtenerPromedioTemp(false);
  float humProm = obtenerPromedioTemp(true);
  if (statusDHT == SensorStatus::OK && statusMQ2 == SensorStatus::OK) {
    evaluarRiesgo(tempProm, humProm, nivel, tendencia);
  } else {
    ledState = LedState::ERROR_SENSOR;
  }

  if (millis() - ultimoSerial > 2000) {
    ultimoSerial = millis();
    serialDiagnostico(tempProm, humProm, aoFiltrado, aoBase, cambio, doVal, nivel, tendencia, confianza, riesgoActual);
  }

  if (millis() - ultimoEnvio > INTERVALO_ENVIO) {
    ultimoEnvio = millis();
    enviarDatos(tempProm, humProm, aoFiltrado, aoBase, cambio, doVal, nivel, confianza, riesgoActual);
  }

  actualizarLED();
}