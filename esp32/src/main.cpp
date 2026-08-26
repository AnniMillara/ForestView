// =============================================================
//               FORESTGUARD ESP32 - FIRMWARE V3.6 (MQ-2 equilibrado)
//               Sensibilidad media, sin falsos positivos
// =============================================================
//  Hardware: ESP32 + DHT (3 pines, DHT22 por defecto) + MQ-2 + LED RGB
//  Backend: Flask + MySQL (sin cambios)
// =============================================================
//  LED:
//    🔵 AZUL FIJO     = Calentamiento / Calibración
//    🔵 AZUL PARP.    = Error de sensor
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
const char* WIFI_SSID_1     = "GameofThrones";
const char* WIFI_PASS_1     = "elsenordelosanillos";
const char* WIFI_SSID_2     = "b3ar";
const char* WIFI_PASS_2     = "papyrusB3st";
const int NUM_NETWORKS = 2;
const char* WIFI_SSIDS[]   = { WIFI_SSID_1, WIFI_SSID_2 };
const char* WIFI_PASSWORDS[] = { WIFI_PASS_1, WIFI_PASS_2 };

// =============================================================
//  SERVIDOR (MANTENER)
// =============================================================
const char* SERVER_IP       = "192.168.205.3";
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
//  UMBRALES (AJUSTADOS: subidos ligeramente respecto a V3.5)
// =============================================================
const float TEMP_RIESGO      = 30.0;
const float TEMP_ALTA        = 35.0;
const float TEMP_CRITICA     = 40.0;
const float HUMEDAD_RIESGO   = 45.0;
const float HUMEDAD_BAJA     = 30.0;
const float HUMEDAD_CRITICA  = 15.0;

// Umbrales de cambio AO (porcentaje) - SUBIDOS un poco
const float HUMO_PORCENTAJE_BAJO   = 10.0;   // antes 8
const float HUMO_PORCENTAJE_MEDIO  = 22.0;   // antes 18
const float HUMO_PORCENTAJE_ALTO   = 40.0;   // antes 35

// Histéresis: también subida para que no baje tan fácilmente
const float HYSTERESIS_PORCENTAJE_BAJO   = 6.0;   // antes 5
const float HYSTERESIS_PORCENTAJE_MEDIO  = 14.0;  // antes 12
const float HYSTERESIS_PORCENTAJE_ALTO   = 28.0;  // antes 25

// =============================================================
//  CONSTANTES DE TIEMPO Y FILTROS (rápidos, se mantienen)
// =============================================================
const unsigned long INTERVALO_LECTURA   = 1000;      // 1 s
const unsigned long INTERVALO_ENVIO     = 5000;      // 5 s
const unsigned long TIEMPO_CALENTAMIENTO_MQ2 = 60000; // 60 s
const unsigned long INTERVALO_RECALIB   = 600000;    // 10 min
const unsigned long TIMEOUT_HTTP        = 5000;      // 5 s

// Tamaños de filtros
const int MUESTRAS_PROMEDIO_TEMP = 5;
const int MUESTRAS_PROMEDIO_AO   = 5;
const int VENTANA_TENDENCIA      = 4;

// Persistencia (se mantiene la misma)
const int LECTURAS_PARA_POSIBLE   = 1;
const int LECTURAS_PARA_DETECTADO = 2;
const int LECTURAS_PARA_FUERTE    = 3;
const int LECTURAS_PARA_NORMAL    = 2;

// =============================================================
//  VARIABLES GLOBALES
// =============================================================
DHT dht(DHT_PIN, DHT_TYPE);

String stationCode = "";
String connectedSSID = "";

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

// Buffers para filtros AO
int aoBuffer[MUESTRAS_PROMEDIO_AO];
int idxAO = 0;
bool aoBufferLleno = false;
int ultimoAOValido = 0;

// Buffers para temperatura
float tempBuffer[MUESTRAS_PROMEDIO_TEMP];
float humBuffer[MUESTRAS_PROMEDIO_TEMP];
int idxTemp = 0;
bool tempBufferLleno = false;
float ultimaTempValida = 25.0;
float ultimaHumValida = 50.0;

// Para tendencia de cambio AO
int ultimosCambios[VENTANA_TENDENCIA];
int idxTendencia = 0;
bool tendenciaLlena = false;

// ---- Estado de sensores ----
enum class SensorStatus : uint8_t { OK, ERROR, DESCONOCIDO };
SensorStatus statusDHT = SensorStatus::DESCONOCIDO;
SensorStatus statusMQ2 = SensorStatus::DESCONOCIDO;
int fallosDHT = 0;
const int MAX_FALLOS_DHT = 5;
unsigned long ultimaLecturaDHTok = 0;

// ---- Estado del humo ----
enum class NivelHumo : uint8_t { NINGUNO, POSIBLE, DETECTADO, FUERTE };
NivelHumo nivelHumoActual = NivelHumo::NINGUNO;
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

// ---- Flask ----
bool flaskDisponible = false;
unsigned long ultimoEnvioExitoso = 0;

// =============================================================
//  PROTOTIPOS
// =============================================================
void actualizarLED();
void calibrarMQ2(const char* motivo, bool forzado = false);
bool deberiaRecalibrar();
int calcularTendencia();
NivelHumo clasificarHumo(int cambioPorcentaje, bool humoDO);
void evaluarRiesgo(float temp, float hum, NivelHumo nivel, int tendencia);
void enviarDatos(float temp, float hum, int ao, int base, int cambioPorcentaje, int doVal, NivelHumo nivel, int conf, Riesgo riesgo);
void conectarWiFi();
void comprobarWiFi();
void serialDiagnostico(float temp, float hum, int ao, int base, int cambioAbs, int cambioPorcentaje, int doVal, NivelHumo nivel, int tendencia, int conf, Riesgo riesgo);
bool leerDHT22(float &temp, float &hum);
float obtenerPromedioTemp(bool humedad);
int obtenerPromedioAO();
void agregarMuestraTemp(float temp, float hum);
void agregarMuestraAO(int ao);

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
//  CALIBRACIÓN MQ-2 (robusta)
// =============================================================
void precalentarMQ2() {
  if (mq2Precalentado) return;
  if (inicioPrecalentamiento == 0) {
    inicioPrecalentamiento = millis();
    Serial.println("[MQ-2] Iniciando precalentamiento de 60 segundos (NO DETECTAR HUMO)");
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
    Serial.println("[CALIB] Recalibración pospuesta (condiciones inestables o humo presente)");
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

  const int NUM_MUESTRAS = 30;
  long sumaAO = 0;
  int high = 0, low = 0;
  int muestrasValidas = 0;
  int valores[NUM_MUESTRAS];

  for (int i = 0; i < NUM_MUESTRAS; i++) {
    int ao = analogRead(MQ2_AO);
    int doVal = digitalRead(MQ2_DO);
    if (ao >= 0 && ao <= 4095) {
      sumaAO += ao;
      valores[muestrasValidas] = ao;
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
  if (statusMQ2 != SensorStatus::OK) return false;
  if (millis() - ultimaCalibracion < 300000) return false; // 5 min
  return true;
}

// =============================================================
//  LECTURA DHT22 (con validación)
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
//  FILTROS (promedio móvil con descarte de outliers)
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
//  TENDENCIA (pendiente del cambio AO)
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
//  CLASIFICACIÓN DE HUMO (con persistencia e histéresis)
// =============================================================
NivelHumo clasificarHumo(int cambioPorcentaje, bool humoDO) {
  NivelHumo nivel = NivelHumo::NINGUNO;

  // Clasificación con prioridad al AO
  if (cambioPorcentaje >= HUMO_PORCENTAJE_ALTO) {
    nivel = NivelHumo::FUERTE;
  } else if (cambioPorcentaje >= HUMO_PORCENTAJE_MEDIO) {
    nivel = NivelHumo::DETECTADO;
  } else if (cambioPorcentaje >= HUMO_PORCENTAJE_BAJO) {
    nivel = NivelHumo::POSIBLE;
  } else if (humoDO && cambioPorcentaje >= HUMO_PORCENTAJE_BAJO * 0.6) {
    nivel = NivelHumo::POSIBLE;
  }

  // Histéresis (con los nuevos valores)
  if (nivel < nivelHumoEstable) {
    switch (nivelHumoEstable) {
      case NivelHumo::FUERTE:
        if (cambioPorcentaje < HYSTERESIS_PORCENTAJE_ALTO) {
          if (cambioPorcentaje < HYSTERESIS_PORCENTAJE_MEDIO) {
            nivel = NivelHumo::DETECTADO;
          } else {
            nivel = NivelHumo::FUERTE;
          }
        } else {
          nivel = NivelHumo::FUERTE;
        }
        break;
      case NivelHumo::DETECTADO:
        if (cambioPorcentaje < HYSTERESIS_PORCENTAJE_MEDIO) {
          if (cambioPorcentaje < HYSTERESIS_PORCENTAJE_BAJO) {
            nivel = NivelHumo::POSIBLE;
          } else {
            nivel = NivelHumo::DETECTADO;
          }
        } else {
          nivel = NivelHumo::DETECTADO;
        }
        break;
      case NivelHumo::POSIBLE:
        if (cambioPorcentaje < HYSTERESIS_PORCENTAJE_BAJO) {
          nivel = NivelHumo::NINGUNO;
        } else {
          nivel = NivelHumo::POSIBLE;
        }
        break;
      default:
        break;
    }
  }

  // Contadores de persistencia
  int idxNivel = (int)nivel;
  contadorHumo[idxNivel]++;
  for (int i = 0; i < 4; i++) {
    if (i != idxNivel) contadorHumo[i] = 0;
  }

  int umbral;
  switch (nivel) {
    case NivelHumo::POSIBLE:   umbral = LECTURAS_PARA_POSIBLE; break;
    case NivelHumo::DETECTADO: umbral = LECTURAS_PARA_DETECTADO; break;
    case NivelHumo::FUERTE:    umbral = LECTURAS_PARA_FUERTE; break;
    default:                   umbral = 2; break;
  }

  if (contadorHumo[idxNivel] >= umbral) {
    nivelHumoEstable = nivel;
  } else {
    if (nivel == NivelHumo::NINGUNO && contadorHumo[0] >= LECTURAS_PARA_NORMAL) {
      nivelHumoEstable = NivelHumo::NINGUNO;
    }
  }

  return nivelHumoEstable;
}

// =============================================================
//  EVALUACIÓN DE RIESGO (sin cambios)
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

  if (nivel == NivelHumo::FUERTE) {
    alerta = true; conf = 80;
  } else if (nivel == NivelHumo::DETECTADO && tempAlta) {
    alerta = true; conf = 75;
  } else if (nivel == NivelHumo::DETECTADO && humBaja) {
    alerta = true; conf = 70;
  } else if (nivel == NivelHumo::POSIBLE && tempCritica) {
    alerta = true; conf = 70;
  } else if (nivel == NivelHumo::POSIBLE && humCritica) {
    alerta = true; conf = 65;
  } else if (tempCritica && humBaja) {
    alerta = true; conf = 60;
  } else if (nivel != NivelHumo::NINGUNO && tempAlta && humBaja) {
    alerta = true; conf = 75;
  }

  if (!alerta) {
    if (nivel == NivelHumo::DETECTADO) {
      alto = true; conf = 55;
    } else if (nivel == NivelHumo::POSIBLE && tempAlta) {
      alto = true; conf = 50;
    } else if (nivel == NivelHumo::POSIBLE && humBaja) {
      alto = true; conf = 45;
    } else if (tempAlta && humBaja) {
      alto = true; conf = 40;
    }
  }

  if (!alerta && !alto) {
    if (nivel == NivelHumo::POSIBLE) {
      moderado = true; conf = 30;
    } else if (tempRiesgo) {
      moderado = true; conf = 25;
    } else if (humRiesgo) {
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
  for (int i = 0; i < 4; i++) if (i != idx) contadorRiesgo[i] = 0;

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
void enviarDatos(float temp, float hum, int ao, int base, int cambioPorcentaje, int doVal, NivelHumo nivel, int conf, Riesgo riesgo) {
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
  doc["cambio_ao"] = cambioPorcentaje;
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
      return;
    }
  }
  Serial.println("WIFI -> ERROR: no se pudo conectar a ninguna red");
  wifiConectado = false;
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
  for (int i = 0; i < NUM_NETWORKS; i++) {
    Serial.print("Intentando con: ");
    Serial.println(WIFI_SSIDS[i]);
    WiFi.begin(WIFI_SSIDS[i], WIFI_PASSWORDS[i]);
    int intentos = 0;
    while (WiFi.status() != WL_CONNECTED && intentos < 15) {
      delay(300);
      intentos++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Reconectado a ");
      Serial.println(WIFI_SSIDS[i]);
      connectedSSID = WIFI_SSIDS[i];
      wifiConectado = true;
      return;
    }
  }
  Serial.println("No se pudo reconectar a ninguna red");
}

// =============================================================
//  DIAGNÓSTICO SERIAL
// =============================================================
void serialDiagnostico(float temp, float hum, int ao, int base, int cambioAbs, int cambioPorcentaje, int doVal, NivelHumo nivel, int tendencia, int conf, Riesgo riesgo) {
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
  Serial.print("CAMBIO ABS   : "); Serial.println(cambioAbs);
  Serial.print("CAMBIO %     : "); Serial.print(cambioPorcentaje); Serial.println(" %");
  Serial.print("MQ-2 DO      : "); Serial.println(doVal);
  Serial.print("DO NORMAL    : "); Serial.println(estadoDONormal);
  Serial.print("MQ-2 Estado  : ");
  if (statusMQ2 == SensorStatus::OK) Serial.println("OK");
  else if (statusMQ2 == SensorStatus::ERROR) Serial.println("ERROR");
  else Serial.println("DESCONOCIDO");

  const char* niveles[] = {"NINGUNO","POSIBLE","DETECTADO","FUERTE"};
  Serial.print("NIVEL HUMO   : "); Serial.println(niveles[(int)nivel]);
  Serial.print("CONFIRMACION : ");
  for (int i=0; i<4; i++) {
    Serial.print(niveles[i]); Serial.print(":"); Serial.print(contadorHumo[i]); Serial.print(" ");
  }
  Serial.println();

  const char* tendencias[] = {"BAJANDO","ESTABLE","SUBIENDO"};
  int idxTend = (tendencia < -10) ? 0 : (tendencia > 10 ? 2 : 1);
  Serial.print("TENDENCIA    : "); Serial.println(tendencias[idxTend]);

  Serial.print("CONFIANZA    : "); Serial.print(conf); Serial.println(" %");

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
  Serial.println("           FORESTGUARD V3.6");
  Serial.println("      SISTEMA DE MONITOREO EQUILIBRADO");
  Serial.println("======================================");

  // Inicializar LED PWM
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

  // Autodiagnóstico visual (LED)
  ledRojo(); delay(300);
  ledVerde(); delay(300);
  ledAzul(); delay(300);
  ledAmarillo(); delay(300);
  ledNaranjo(); delay(300);
  ledApagado();
  Serial.println("[DIAG] LED RGB OK");

  // DHT
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

  // MQ-2 lectura inicial
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

  // Iniciar precalentamiento
  precalentarMQ2();

  // WiFi
  conectarWiFi();

  stationCode = WiFi.macAddress();
  stationCode.replace(":", "");
  Serial.print("[DIAG] Código estación: "); Serial.println(stationCode);

  // Inicializar buffers de tendencia
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
//  LOOP PRINCIPAL
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

  // ---- LEER DHT ----
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

  // ---- LEER MQ-2 ----
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

  int cambioAbs = abs(aoFiltrado - aoBase);
  int cambioPorcentaje = 0;
  if (aoBase != 0) {
    cambioPorcentaje = (cambioAbs * 100) / aoBase;
  } else {
    cambioPorcentaje = 0;
  }

  bool humoDO = (doVal != estadoDONormal);

  // ---- TENDENCIA ----
  static int ultimoCambioPorcentaje = 0;
  if (cambioPorcentaje != ultimoCambioPorcentaje) {
    ultimosCambios[idxTendencia] = cambioPorcentaje;
    idxTendencia = (idxTendencia + 1) % VENTANA_TENDENCIA;
    if (idxTendencia == 0) tendenciaLlena = true;
    ultimoCambioPorcentaje = cambioPorcentaje;
  }
  int tendencia = calcularTendencia();

  // ---- CLASIFICAR HUMO ----
  NivelHumo nivel = clasificarHumo(cambioPorcentaje, humoDO);

  // ---- EVALUAR RIESGO ----
  float tempProm = obtenerPromedioTemp(false);
  float humProm = obtenerPromedioTemp(true);
  if (statusDHT == SensorStatus::OK && statusMQ2 == SensorStatus::OK) {
    evaluarRiesgo(tempProm, humProm, nivel, tendencia);
  } else {
    ledState = LedState::ERROR_SENSOR;
  }

  // ---- SERIAL DIAGNÓSTICO (cada 2s) ----
  if (millis() - ultimoSerial > 2000) {
    ultimoSerial = millis();
    serialDiagnostico(tempProm, humProm, aoFiltrado, aoBase, cambioAbs, cambioPorcentaje, doVal, nivel, tendencia, confianza, riesgoActual);
  }

  // ---- ENVÍO A FLASK (cada 5s) ----
  if (millis() - ultimoEnvio > INTERVALO_ENVIO) {
    ultimoEnvio = millis();
    enviarDatos(tempProm, humProm, aoFiltrado, aoBase, cambioPorcentaje, doVal, nivel, confianza, riesgoActual);
  }

  actualizarLED();
}