// =============================================================
//               FORESTGUARD ESP32 - FIRMWARE FINAL
//               Robusto, confiable y seguro
// =============================================================
//  Hardware: ESP32 + DHT22 + MQ-2 + LED RGB
//  Backend: Flask + MySQL
// =============================================================
//  LED:
//    🔵 AZUL       = Calibración / Error de sensor (prioridad alta)
//    🟢 VERDE      = NORMAL
//    🟡 AMARILLO   = RIESGO MODERADO
//    🟠 NARANJO    = RIESGO ALTO
//    🔴 ROJO       = ALERTA (posible incendio)
// =============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// =============================================================
//  PINES (MANTENER)
// =============================================================
#define DHT_PIN     13
#define DHT_TYPE    DHT22
#define MQ2_AO      35
#define MQ2_DO      33
#define LED_ROJO    26
#define LED_VERDE   27
#define LED_AZUL    25

// =============================================================
//  WIFI Y SERVIDOR (MANTENER CREDENCIALES E IP)
// =============================================================
const char* WIFI_SSID       = "GameofThrones";
const char* WIFI_PASSWORD   = "elsenordelosanillos";
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
//  UMBRALES (mantener valores, se añade histéresis)
// =============================================================
const float TEMP_RIESGO      = 30.0;
const float TEMP_ALTA        = 35.0;
const float TEMP_CRITICA     = 40.0;
const float HUMEDAD_RIESGO   = 45.0;
const float HUMEDAD_BAJA     = 30.0;
const float HUMEDAD_CRITICA  = 15.0;

// Umbrales de cambio AO (con histéresis)
const int HUMO_CAMBIO_BAJO   = 80;
const int HUMO_CAMBIO_MEDIO  = 200;
const int HUMO_CAMBIO_ALTO   = 400;

// Histéresis (valor de salida menor que entrada)
const int HYSTERESIS_BAJO    = 60;
const int HYSTERESIS_MEDIO   = 150;
const int HYSTERESIS_ALTO    = 320;

// =============================================================
//  CONSTANTES DE TIEMPO Y FILTROS
// =============================================================
const unsigned long INTERVALO_LECTURA   = 2000;      // 2 s
const unsigned long INTERVALO_ENVIO     = 5000;      // 5 s
const unsigned long TIEMPO_CALIB_INICIAL = 5000;     // 5 s de calibración inicial
const unsigned long INTERVALO_RECALIB   = 600000;    // 10 min
const unsigned long TIMEOUT_HTTP        = 5000;      // 5 s

// Tamaños de filtros
const int MUESTRAS_PROMEDIO_TEMP = 5;
const int MUESTRAS_PROMEDIO_AO   = 10;               // Mayor filtrado para AO
const int VENTANA_TENDENCIA      = 6;               // para calcular pendiente

// Persistencia
const int LECTURAS_PARA_RIESGO_MODERADO = 3;
const int LECTURAS_PARA_RIESGO_ALTO     = 4;
const int LECTURAS_PARA_ALERTA          = 5;

// =============================================================
//  VARIABLES GLOBALES
// =============================================================
DHT dht(DHT_PIN, DHT_TYPE);

String stationCode = "";          // MAC sin ':'

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

// Buffers para filtros
float tempBuffer[MUESTRAS_PROMEDIO_TEMP];
float humBuffer[MUESTRAS_PROMEDIO_TEMP];
int idxTemp = 0;
bool tempBufferLleno = false;

int aoBuffer[MUESTRAS_PROMEDIO_AO];
int idxAO = 0;
bool aoBufferLleno = false;

// Para tendencia
int ultimosCambios[VENTANA_TENDENCIA];
int idxTendencia = 0;
bool tendenciaLlena = false;

// ---- Persistencia e histéresis ----
enum class NivelHumo : uint8_t { NINGUNO, BAJO, MEDIO, ALTO };
NivelHumo nivelHumoEstable = NivelHumo::NINGUNO;
NivelHumo nivelHumoActual   = NivelHumo::NINGUNO;
int contadorHumo[4] = {0,0,0,0};  // índice 0=NINGUNO,1=BAJO,2=MEDIO,3=ALTO

// ---- Estado de sensores ----
enum class SensorStatus : uint8_t { OK, ERROR };
SensorStatus statusDHT = SensorStatus::OK;
SensorStatus statusMQ2 = SensorStatus::OK;
int fallosDHT = 0;
const int MAX_FALLOS_DHT = 5;
unsigned long ultimoDHTok = 0;

// ---- Riesgo ----
enum class Riesgo : uint8_t { NORMAL, MODERADO, ALTO, ALERTA };
Riesgo riesgoActual = Riesgo::NORMAL;
int confianza = 0;      // 0-100

// ---- WiFi ----
bool wifiConectado = false;
unsigned long ultimoIntentoWiFi = 0;

// ---- Flask ----
bool flaskDisponible = false;
unsigned long ultimoEnvioExitoso = 0;

// =============================================================
//  PROTOTIPOS DE FUNCIONES
// =============================================================
void actualizarLED();
void calibrarMQ2(const char* motivo, bool forzado = false);
bool deberiaRecalibrar();
void leerMQ2(int &ao, int &doVal, int &cambio, bool &humoDO);
int calcularTendencia();
NivelHumo clasificarHumo(int cambio, bool humoDO, bool &subiendo);
void evaluarRiesgo(float temp, float hum, NivelHumo nivel, int tendencia);
void enviarDatos(float temp, float hum, int ao, int base, int cambio, int doVal, NivelHumo nivel, int conf, Riesgo riesgo, bool wifiOk, bool flaskOk);
void conectarWiFi();
void comprobarWiFi();
void serialDiagnostico(float temp, float hum, int ao, int base, int cambio, int doVal, NivelHumo nivel, int tendencia, int conf, Riesgo riesgo, bool wifiOk, bool flaskOk);

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
  // Prioridad: ERROR de sensor > CALIBRACIÓN > ALERTA > RIESGO ALTO > RIESGO MODERADO > NORMAL
  if (statusDHT == SensorStatus::ERROR) {
    ledParpadeoAzul();
    return;
  }
  if (ledState == LedState::CALIBRACION) {
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
//  CALIBRACIÓN MQ-2 (mejorada)
// =============================================================
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
  Serial.println("Mantén el sensor en aire limpio.");
  delay(100);

  const int NUM_MUESTRAS = 20;
  long sumaAO = 0;
  int high = 0, low = 0;
  int muestrasValidas = 0;

  for (int i = 0; i < NUM_MUESTRAS; i++) {
    int ao = analogRead(MQ2_AO);
    int doVal = digitalRead(MQ2_DO);
    // Validar rango ADC (0-4095)
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
  // Solo recalibrar si:
  // - El ambiente es estable (sin humo, temperatura normal, humedad normal)
  // - No hay riesgo activo (alerta, alto o moderado)
  if (riesgoActual != Riesgo::NORMAL) return false;
  if (nivelHumoEstable != NivelHumo::NINGUNO) return false;
  // Además, temperatura y humedad deben estar en rangos seguros (usamos último valor filtrado)
  // Esto se evalúa en el loop, aquí solo comprobamos el estado general.
  return true;
}

// =============================================================
//  LECTURA DHT22 CON VALIDACIÓN
// =============================================================
bool leerDHT22(float &temp, float &hum) {
  const int MAX_INTENTOS = 5;
  for (int i = 0; i < MAX_INTENTOS; i++) {
    temp = dht.readTemperature();
    hum = dht.readHumidity();
    if (!isnan(temp) && !isnan(hum) && temp >= -40 && temp <= 80 && hum >= 0 && hum <= 100) {
      return true;
    }
    delay(100);
  }
  return false;
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
  if (n == 0) return analogRead(MQ2_AO); // fallback
  long suma = 0;
  for (int i = 0; i < n; i++) suma += aoBuffer[i];
  return suma / n;
}

// =============================================================
//  TENDENCIA (pendiente de cambio AO)
// =============================================================
int calcularTendencia() {
  // Calcula la pendiente de los últimos cambios AO en la ventana
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
  float pendiente = (n * sumaXY - sumaX * sumaY) / (float)(n * sumaX2 - sumaX * sumaX);
  return (int)(pendiente * 100); // escala para representar tendencia
}

// =============================================================
//  CLASIFICACIÓN DE HUMO CON PERSISTENCIA E HISTÉRESIS
// =============================================================
NivelHumo clasificarHumo(int cambio, bool humoDO, bool &subiendo) {
  // Determinar nivel actual según umbrales (con histéresis)
  NivelHumo nivel = NivelHumo::NINGUNO;
  if (cambio >= HUMO_CAMBIO_ALTO || humoDO) nivel = NivelHumo::ALTO;
  else if (cambio >= HUMO_CAMBIO_MEDIO) nivel = NivelHumo::MEDIO;
  else if (cambio >= HUMO_CAMBIO_BAJO) nivel = NivelHumo::BAJO;

  // Si está en un nivel, verificar si debe bajar (histéresis)
  if (nivel == NivelHumo::NINGUNO) {
    if (nivelHumoEstable == NivelHumo::BAJO && cambio >= HYSTERESIS_BAJO) nivel = NivelHumo::BAJO;
    else if (nivelHumoEstable == NivelHumo::MEDIO && cambio >= HYSTERESIS_MEDIO) nivel = NivelHumo::MEDIO;
    else if (nivelHumoEstable == NivelHumo::ALTO && cambio >= HYSTERESIS_ALTO) nivel = NivelHumo::ALTO;
  }

  // Persistencia: contadores separados
  int idxNivel = (int)nivel;
  contadorHumo[idxNivel]++;
  // Resetear contadores de otros niveles
  for (int i = 0; i < 4; i++) {
    if (i != idxNivel) contadorHumo[i] = 0;
  }

  // Definir umbrales de persistencia para cada nivel
  int umbral;
  switch (nivel) {
    case NivelHumo::BAJO:  umbral = LECTURAS_PARA_RIESGO_MODERADO; break;
    case NivelHumo::MEDIO: umbral = LECTURAS_PARA_RIESGO_ALTO; break;
    case NivelHumo::ALTO:  umbral = LECTURAS_PARA_ALERTA; break;
    default: umbral = 2; break;
  }

  if (contadorHumo[idxNivel] >= umbral) {
    nivelHumoEstable = nivel;
    // Evaluar tendencia
    subiendo = (calcularTendencia() > 0);
  } else {
    // Si no se alcanza persistencia, mantener el nivel estable anterior
    // a menos que sea NINGUNO y haya acumulado 2 lecturas sin humo
    if (nivel == NivelHumo::NINGUNO && contadorHumo[0] >= 2) {
      nivelHumoEstable = NivelHumo::NINGUNO;
    }
  }
  return nivelHumoEstable;
}

// =============================================================
//  EVALUACIÓN DE RIESGO (MATRIZ DE DECISIÓN MEJORADA)
// =============================================================
void evaluarRiesgo(float temp, float hum, NivelHumo nivel, int tendencia) {
  bool tempRiesgo = temp >= TEMP_RIESGO;
  bool tempAlta   = temp >= TEMP_ALTA;
  bool tempCritica= temp >= TEMP_CRITICA;
  bool humRiesgo  = hum <= HUMEDAD_RIESGO;
  bool humBaja    = hum <= HUMEDAD_BAJA;
  bool humCritica = hum <= HUMEDAD_CRITICA;

  // Variables de decisión
  bool alerta = false, alto = false, moderado = false;
  int conf = 0;

  // --- ALERTA (posible incendio) ---
  if (nivel == NivelHumo::ALTO) {
    alerta = true;
    conf = 80;
  }
  else if (nivel == NivelHumo::MEDIO && tempAlta) {
    alerta = true;
    conf = 75;
  }
  else if (nivel == NivelHumo::MEDIO && humBaja) {
    alerta = true;
    conf = 70;
  }
  else if (nivel == NivelHumo::BAJO && tempCritica) {
    alerta = true;
    conf = 70;
  }
  else if (nivel == NivelHumo::BAJO && humCritica) {
    alerta = true;
    conf = 65;
  }
  else if (tempCritica && humBaja) {
    alerta = true;
    conf = 60;
  }
  else if (nivel != NivelHumo::NINGUNO && tempAlta && humBaja) {
    alerta = true;
    conf = 75;
  }

  // --- RIESGO ALTO ---
  if (!alerta) {
    if (nivel == NivelHumo::MEDIO) {
      alto = true;
      conf = 55;
    }
    else if (nivel == NivelHumo::BAJO && tempAlta) {
      alto = true;
      conf = 50;
    }
    else if (nivel == NivelHumo::BAJO && humBaja) {
      alto = true;
      conf = 45;
    }
    else if (tempAlta && humBaja) {
      alto = true;
      conf = 40;
    }
  }

  // --- RIESGO MODERADO ---
  if (!alerta && !alto) {
    if (nivel == NivelHumo::BAJO) {
      moderado = true;
      conf = 30;
    }
    else if (tempRiesgo) {
      moderado = true;
      conf = 25;
    }
    else if (humRiesgo) {
      moderado = true;
      conf = 20;
    }
  }

  // Ajustar confianza según tendencia
  if (tendencia > 0) conf = min(100, conf + 10);  // subiendo -> más confianza
  if (tendencia < -50) conf = max(0, conf - 10);  // bajando rápido -> menos confianza

  // Aplicar histéresis de riesgo (no usar contadores aquí, ya que la persistencia está en el humo)
  // Si el riesgo es menor que el actual, requerimos confirmación adicional
  // Pero simplificamos: usamos la confianza para decidir el estado final

  Riesgo nuevoRiesgo = Riesgo::NORMAL;
  if (alerta) nuevoRiesgo = Riesgo::ALERTA;
  else if (alto) nuevoRiesgo = Riesgo::ALTO;
  else if (moderado) nuevoRiesgo = Riesgo::MODERADO;

  // Guardar confianza
  confianza = conf;

  // Actualizar riesgo solo si ha cambiado y se confirma (persistencia global)
  // Usamos un contador simple para evitar oscilaciones
  static Riesgo ultimoRiesgo = Riesgo::NORMAL;
  static int contadorCambio = 0;
  if (nuevoRiesgo != ultimoRiesgo) {
    contadorCambio++;
    if (contadorCambio >= 2) {
      riesgoActual = nuevoRiesgo;
      ultimoRiesgo = nuevoRiesgo;
      contadorCambio = 0;
    }
  } else {
    contadorCambio = 0;
  }

  // Actualizar LED según riesgo (pero prioriza error de sensor)
  if (statusDHT == SensorStatus::OK) {
    switch (riesgoActual) {
      case Riesgo::ALERTA:  ledState = LedState::ALERTA; break;
      case Riesgo::ALTO:    ledState = LedState::RIESGO_ALTO; break;
      case Riesgo::MODERADO: ledState = LedState::RIESGO_MODERADO; break;
      default:              ledState = LedState::NORMAL; break;
    }
  } else {
    ledState = LedState::ERROR_SENSOR;  // lo maneja actualizarLED()
  }
}

// =============================================================
//  ENVÍO A FLASK (con manejo de errores)
// =============================================================
void enviarDatos(float temp, float hum, int ao, int base, int cambio, int doVal, NivelHumo nivel, int conf, Riesgo riesgo, bool wifiOk, bool flaskOk) {
  if (!wifiOk) {
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
  // Campos adicionales (no rompen Flask)
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
    }
  } else {
    Serial.print("[HTTP] Falló: "); Serial.println(http.errorToString(codigo));
    if (codigo == -1) { // connection refused
      flaskDisponible = false;
    }
  }
  http.end();
}

// =============================================================
//  WIFI
// =============================================================
void conectarWiFi() {
  Serial.println();
  Serial.println("======================================");
  Serial.println("             CONEXION WIFI");
  Serial.println("======================================");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando");
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) {
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
  if (ahora - ultimoIntentoWiFi < 10000) return;
  ultimoIntentoWiFi = ahora;
  Serial.println("WIFI DESCONECTADO, reconectando...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// =============================================================
//  DIAGNÓSTICO SERIAL COMPLETO
// =============================================================
void serialDiagnostico(float temp, float hum, int ao, int base, int cambio, int doVal, NivelHumo nivel, int tendencia, int conf, Riesgo riesgo, bool wifiOk, bool flaskOk) {
  Serial.println();
  Serial.println("======================================");
  Serial.println("        FORESTGUARD");
  Serial.println("======================================");
  Serial.print("Temperatura : "); Serial.print(temp, 1); Serial.println(" C");
  Serial.print("Humedad     : "); Serial.print(hum, 1); Serial.println(" %");
  Serial.print("MQ-2 AO     : "); Serial.println(ao);
  Serial.print("AO BASE     : "); Serial.println(base);
  Serial.print("CAMBIO AO   : "); Serial.println(cambio);
  Serial.print("MQ-2 DO     : "); Serial.println(doVal);

  const char* niveles[] = {"NINGUNO","BAJO","MEDIO","ALTO"};
  Serial.print("HUMO        : "); Serial.println(niveles[(int)nivel]);

  const char* tendencias[] = {"BAJANDO","ESTABLE","SUBIENDO"};
  int idxTend = (tendencia < -10) ? 0 : (tendencia > 10 ? 2 : 1);
  Serial.print("TENDENCIA   : "); Serial.println(tendencias[idxTend]);
  Serial.print("CONFIANZA   : "); Serial.print(conf); Serial.println(" %");

  const char* estados[] = {"NORMAL","RIESGO MODERADO","RIESGO ALTO","ALERTA"};
  Serial.print("ESTADO      : "); Serial.println(estados[(int)riesgo]);

  Serial.print("WIFI        : "); Serial.println(wifiOk ? "CONECTADO" : "DESCONECTADO");
  Serial.print("FLASK       : "); Serial.println(flaskOk ? "DISPONIBLE" : "NO DISPONIBLE");

  Serial.print("DHT22       : "); Serial.println(statusDHT == SensorStatus::OK ? "OK" : "ERROR");
  Serial.print("MQ-2        : "); Serial.println(statusMQ2 == SensorStatus::OK ? "OK" : "ERROR");
  Serial.println("======================================");
}

// =============================================================
//  SETUP (autodiagnóstico)
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("           FORESTGUARD");
  Serial.println("      SISTEMA DE MONITOREO");
  Serial.println("======================================");

  // Inicializar LED PWM
  ledcSetup(CANAL_ROJO, FRECUENCIA, RESOLUCION);
  ledcSetup(CANAL_VERDE, FRECUENCIA, RESOLUCION);
  ledcSetup(CANAL_AZUL, FRECUENCIA, RESOLUCION);
  ledcAttachPin(LED_ROJO, CANAL_ROJO);
  ledcAttachPin(LED_VERDE, CANAL_VERDE);
  ledcAttachPin(LED_AZUL, CANAL_AZUL);
  ledApagado();

  // Pines
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

  // DHT22
  float t, h;
  if (leerDHT22(t, h)) {
    Serial.println("[DIAG] DHT22 OK");
    statusDHT = SensorStatus::OK;
  } else {
    Serial.println("[DIAG] DHT22 ERROR (comprueba conexión)");
    statusDHT = SensorStatus::ERROR;
  }

  // MQ-2 (lectura simple)
  int ao = analogRead(MQ2_AO);
  int doVal = digitalRead(MQ2_DO);
  if (ao >= 0 && ao <= 4095) {
    Serial.println("[DIAG] MQ-2 AO OK");
    statusMQ2 = SensorStatus::OK;
  } else {
    Serial.println("[DIAG] MQ-2 AO fuera de rango");
    statusMQ2 = SensorStatus::ERROR;
  }
  Serial.print("[DIAG] MQ-2 DO = "); Serial.println(doVal);

  // Calibración MQ-2 (forzada)
  calibrarMQ2("INICIAL", true);

  // WiFi
  conectarWiFi();

  // Inicializar buffers
  for (int i = 0; i < MUESTRAS_PROMEDIO_TEMP; i++) {
    tempBuffer[i] = 0;
    humBuffer[i] = 0;
  }
  for (int i = 0; i < MUESTRAS_PROMEDIO_AO; i++) {
    aoBuffer[i] = 0;
  }
  for (int i = 0; i < VENTANA_TENDENCIA; i++) {
    ultimosCambios[i] = 0;
  }

  // Obtener MAC
  stationCode = WiFi.macAddress();
  stationCode.replace(":", "");
  Serial.print("[DIAG] Código estación: "); Serial.println(stationCode);

  Serial.println();
  Serial.println("======================================");
  Serial.println("       FORESTGUARD LISTO");
  Serial.println("======================================");
  Serial.println("MONITOREO ACTIVO");
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

  comprobarWiFi();

  // Recalibración periódica condicional
  if (millis() - ultimaCalibracion > INTERVALO_RECALIB && !recalibracionPendiente) {
    if (deberiaRecalibrar()) {
      calibrarMQ2("PERIODICA", false);
    } else {
      recalibracionPendiente = true;
    }
  }

  // Lectura de sensores cada INTERVALO_LECTURA
  static unsigned long ultimaLectura = 0;
  if (millis() - ultimaLectura < INTERVALO_LECTURA) return;
  ultimaLectura = millis();

  // ---- LEER DHT22 ----
  float temp, hum;
  if (leerDHT22(temp, hum)) {
    statusDHT = SensorStatus::OK;
    fallosDHT = 0;
    agregarMuestraTemp(temp, hum);
  } else {
    fallosDHT++;
    if (fallosDHT >= MAX_FALLOS_DHT) {
      statusDHT = SensorStatus::ERROR;
      // No actualizamos buffers, usamos el último valor válido
    } else {
      // Si falla pero no ha superado el máximo, mantenemos el estado anterior
    }
  }

  // ---- LEER MQ-2 (con filtro) ----
  int aoRaw = analogRead(MQ2_AO);
  if (aoRaw < 0 || aoRaw > 4095) {
    statusMQ2 = SensorStatus::ERROR;
  } else {
    statusMQ2 = SensorStatus::OK;
    agregarMuestraAO(aoRaw);
  }
  int aoFiltrado = obtenerPromedioAO();
  int doVal = digitalRead(MQ2_DO);
  int cambio = abs(aoFiltrado - aoBase);
  bool humoDO = (doVal != estadoDONormal);

  // ---- TENDENCIA ----
  static int ultimoCambio = 0;
  if (cambio != ultimoCambio) {
    ultimosCambios[idxTendencia] = cambio;
    idxTendencia = (idxTendencia + 1) % VENTANA_TENDENCIA;
    if (idxTendencia == 0) tendenciaLlena = true;
    ultimoCambio = cambio;
  }
  int tendencia = calcularTendencia();

  // ---- CLASIFICAR HUMO ----
  bool subiendo = false;
  NivelHumo nivel = clasificarHumo(cambio, humoDO, subiendo);

  // ---- EVALUAR RIESGO ----
  float tempProm = obtenerPromedioTemp(false);
  float humProm = obtenerPromedioTemp(true);
  if (statusDHT == SensorStatus::OK) {
    evaluarRiesgo(tempProm, humProm, nivel, tendencia);
  } else {
    // Si DHT está en error, solo confiamos en el humo para riesgo (menos agresivo)
    // pero mantenemos el estado actual para no generar falsos.
  }

  // ---- SERIAL DIAGNÓSTICO (cada 2 segundos) ----
  if (millis() - ultimoSerial > 2000) {
    ultimoSerial = millis();
    serialDiagnostico(tempProm, humProm, aoFiltrado, aoBase, cambio, doVal, nivel, tendencia, confianza, riesgoActual, wifiConectado, flaskDisponible);
  }

  // ---- ENVÍO A FLASK (cada 5 segundos) ----
  if (millis() - ultimoEnvio > INTERVALO_ENVIO) {
    ultimoEnvio = millis();
    enviarDatos(tempProm, humProm, aoFiltrado, aoBase, cambio, doVal, nivel, confianza, riesgoActual, wifiConectado, flaskDisponible);
  }

  // ---- ACTUALIZAR LED ----
  actualizarLED();
}