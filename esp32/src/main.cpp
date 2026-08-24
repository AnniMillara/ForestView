// =============================================================
//               FORESTGUARD ESP32 - FIRMWARE v2.0
//               Robusto, con calibración y filtrado
// =============================================================
//  Hardware: ESP32 + DHT11 + MQ-2 + LED RGB
//  Backend: Flask + MySQL
// =============================================================
//  Pines:
//    DHT11    -> GPIO 13
//    MQ-2 AO  -> GPIO 35   (analógico, opcional)
//    MQ-2 DO  -> GPIO 27   (digital, obligatorio)
//    LED ROJO -> GPIO 25
//    LED VERDE-> GPIO 26
//    LED AZUL -> GPIO 33
// =============================================================
//  Estados:
//    🔵 AZUL FIJO      = Calentamiento / Calibración
//    🟢 VERDE          = NORMAL
//    🟡 AMARILLO       = RIESGO MODERADO
//    🔴 ROJO           = ALERTA MÁXIMA
//    🔵 AZUL PARPADEO  = ERROR DE SENSOR
// =============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// =============================================================
//  PINES (MANTENER)
// =============================================================
#define DHT_PIN     13
#define DHT_TYPE    DHT22          // Cambiado a DHT11
#define MQ2_AO      35
#define MQ2_DO      27
#define LED_ROJO    25
#define LED_VERDE   26
#define LED_AZUL    33

// =============================================================
//  WIFI Y SERVIDOR
// =============================================================
const char* WIFI_SSID       = "b3ar";
const char* WIFI_PASSWORD   = "papyrusB3st";
const char* SERVER_IP       = "172.16.50.57";
const int   SERVER_PORT     = 5000;
const char* DEFAULT_API_KEY = "3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c";

// =============================================================
//  PWM LED (canales y resolución)
// =============================================================
const int FRECUENCIA  = 5000;
const int RESOLUCION  = 8;
const int CANAL_ROJO  = 0;
const int CANAL_VERDE = 1;
const int CANAL_AZUL  = 2;

// =============================================================
//  CONSTANTES DE CALIBRACIÓN Y FILTRADO
// =============================================================
const unsigned long TIEMPO_CALENTAMIENTO = 60000;   // 60 segundos de calentamiento MQ-2
const unsigned long INTERVALO_LECTURA   = 2000;     // 2 segundos
const unsigned long INTERVALO_ENVIO     = 5000;     // 5 segundos
const unsigned long TIMEOUT_HTTP        = 5000;

// Filtros
const int MUESTRAS_PROMEDIO_TEMP = 5;                // Para DHT11
const int MUESTRAS_PROMEDIO_AO   = 10;               // Para MQ-2 AO

// Persistencia de estados
const int LECTURAS_PARA_RIESGO_MODERADO = 3;
const int LECTURAS_PARA_ALERTA_MAXIMA   = 3;
const int LECTURAS_PARA_VOLVER_NORMAL   = 5;        // Histéresis

// Umbrales de riesgo (adaptados al DHT11)
const float TEMP_RIESGO      = 35.0;
const float TEMP_CRITICA     = 40.0;
const float HUMEDAD_RIESGO   = 35.0;
const float HUMEDAD_CRITICA  = 30.0;

// Para el MQ-2: usaremos porcentaje de cambio respecto a la base
const float PORCENTAJE_HUMO_LEVE   = 0.15;   // 15% de cambio
const float PORCENTAJE_HUMO_MODERADO = 0.30; // 30%
const float PORCENTAJE_HUMO_FUERTE  = 0.50;  // 50%

// Histéresis para porcentajes (evita oscilaciones)
const float HISTERESIS_PORCENTAJE = 0.05;    // 5%

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
bool aoDisponible = true;           // Si el AO está conectado y tiene señal
unsigned long tiempoInicioCalentamiento = 0;
bool calentamientoCompletado = false;

// Buffers para filtros
float tempBuffer[MUESTRAS_PROMEDIO_TEMP];
float humBuffer[MUESTRAS_PROMEDIO_TEMP];
int idxTemp = 0;
bool tempBufferLleno = false;

int aoBuffer[MUESTRAS_PROMEDIO_AO];
int idxAO = 0;
bool aoBufferLleno = false;

// ---- Estado actual de los sensores (filtrados) ----
float temperatura = 0.0;
float humedad = 0.0;
int mq2AO = 0;
int mq2DO = HIGH;
int cambioAO = 0;
float porcentajeCambio = 0.0;
bool humoDigital = false;

// ---- Estados de riesgo con persistencia ----
enum class Riesgo : uint8_t { NORMAL, MODERADO, ALERTA };
Riesgo riesgoActual = Riesgo::NORMAL;
int contadorRiesgoModerado = 0;
int contadorAlertaMaxima = 0;
int contadorNormal = 0;

// ---- Estado de sensores ----
bool dhtOK = true;
bool mq2OK = true;
int fallosDHT = 0;
const int MAX_FALLOS_DHT = 3;

// ---- WiFi y Flask ----
bool wifiConectado = false;
bool flaskDisponible = false;
unsigned long ultimoIntentoWiFi = 0;
unsigned long ultimoEnvioExitoso = 0;

// =============================================================
//  PROTOTIPOS
// =============================================================
void actualizarLED();
void calibrarMQ2();
void leerSensores();
void filtrarValores();
void evaluarRiesgo();
void enviarDatos();
void conectarWiFi();
void comprobarWiFi();
void serialDiagnostico();

// =============================================================
//  FUNCIONES LED (CORREGIDAS PARA ESP32 CORE 3.x)
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
  ledcWrite(LED_VERDE, 70);
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
    ledParpadeoAzul();          // Error de sensor
    return;
  }
  if (!calentamientoCompletado) {
    ledAzul();                  // Calentando
    return;
  }
  switch (riesgoActual) {
    case Riesgo::ALERTA:   ledRojo(); break;
    case Riesgo::MODERADO: ledAmarillo(); break;
    default:               ledVerde(); break;
  }
}

// =============================================================
//  CALIBRACIÓN MQ-2 (con calentamiento de 60 segundos)
// =============================================================
void calibrarMQ2() {
  Serial.println();
  Serial.println("======================================");
  Serial.println("   CALENTANDO MQ-2 (60 segundos)");
  Serial.println("======================================");
  Serial.println("Espera a que el sensor se estabilice.");
  Serial.println("NO acerques humo ni gases durante este tiempo.");
  Serial.println();

  ledState = LedState::CALENTANDO;
  actualizarLED();

  const int MUESTRAS_CALIB = 20;
  long sumaAO = 0;
  int muestrasValidas = 0;
  int high = 0, low = 0;

  unsigned long inicio = millis();
  while (millis() - inicio < TIEMPO_CALENTAMIENTO) {
    // Leer AO y DO cada 500 ms para ver evolución
    int ao = analogRead(MQ2_AO);
    int doVal = digitalRead(MQ2_DO);
    
    // Mostrar progreso cada 5 segundos
    if ((millis() - inicio) % 5000 < 500) {
      Serial.print(".");
    }
    
    // Acumular solo al final del calentamiento (últimos 10 segundos)
    if (millis() - inicio > TIEMPO_CALENTAMIENTO - 10000) {
      if (ao >= 0 && ao <= 4095) {
        sumaAO += ao;
        muestrasValidas++;
      }
      if (doVal == HIGH) high++; else low++;
    }
    delay(500);
  }
  Serial.println();

  if (muestrasValidas == 0) {
    Serial.println("⚠️  No se pudieron leer muestras válidas del AO.");
    aoDisponible = false;
    aoBase = 0;
  } else {
    aoBase = sumaAO / muestrasValidas;
    aoDisponible = true;
    Serial.print("AO BASE = "); Serial.println(aoBase);
  }

  estadoDONormal = (high >= low) ? HIGH : LOW;
  Serial.print("DO NORMAL = "); Serial.println(estadoDONormal);

  // Si el AO no varía o está en 0, probablemente no está conectado
  if (aoDisponible && aoBase < 50) {
    Serial.println("⚠️  El valor AO es muy bajo. Posiblemente el pin no está conectado.");
    aoDisponible = false;
  }

  calentamientoCompletado = true;
  ledState = LedState::NORMAL;
  actualizarLED();
  Serial.println("CALIBRACION TERMINADA");
  Serial.println();
}

// =============================================================
//  LECTURA DHT11 CON VALIDACIÓN
// =============================================================
bool leerDHT11(float &temp, float &hum) {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  
  // Validar rango realista para DHT11
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
//  LECTURA Y FILTRADO DE SENSORES
// =============================================================
void leerSensores() {
  // ---- DHT11 ----
  float t, h;
  if (leerDHT11(t, h)) {
    dhtOK = true;
    fallosDHT = 0;
    agregarMuestraTemp(t, h);
  } else {
    fallosDHT++;
    if (fallosDHT >= MAX_FALLOS_DHT) {
      dhtOK = false;
      Serial.println("⚠️  DHT11 sin respuesta. Usando último valor válido.");
    }
  }

  // ---- MQ-2 ----
  int aoRaw = analogRead(MQ2_AO);
  int doVal = digitalRead(MQ2_DO);

  if (aoRaw >= 0 && aoRaw <= 4095) {
    mq2OK = true;
    agregarMuestraAO(aoRaw);
  } else {
    mq2OK = false;
    Serial.println("⚠️  MQ-2 AO fuera de rango.");
  }

  mq2AO = obtenerPromedioAO();
  mq2DO = doVal;

  // Si el AO no está disponible, usamos solo el DO
  if (!aoDisponible) {
    cambioAO = 0;
    porcentajeCambio = 0.0;
  } else if (aoBase > 0) {
    cambioAO = abs(mq2AO - aoBase);
    porcentajeCambio = (float)cambioAO / aoBase;
  } else {
    cambioAO = 0;
    porcentajeCambio = 0.0;
  }

  humoDigital = (mq2DO != estadoDONormal);

  // ---- Temperatura y humedad filtradas ----
  if (dhtOK) {
    temperatura = obtenerPromedioTemp(false);
    humedad = obtenerPromedioTemp(true);
  }
}

// =============================================================
//  EVALUACIÓN DE RIESGO CON PERSISTENCIA E HISTÉRESIS
// =============================================================
void evaluarRiesgo() {
  // Condiciones booleanas
  bool tempRiesgo = (temperatura >= TEMP_RIESGO);
  bool tempCritica = (temperatura >= TEMP_CRITICA);
  bool humRiesgo = (humedad <= HUMEDAD_RIESGO);
  bool humCritica = (humedad <= HUMEDAD_CRITICA);

  // Determinar nivel de humo
  bool humoLeve = false;
  bool humoModerado = false;
  bool humoFuerte = false;
  if (aoDisponible && aoBase > 0) {
    float umbralLeve = PORCENTAJE_HUMO_LEVE;
    float umbralModerado = PORCENTAJE_HUMO_MODERADO;
    float umbralFuerte = PORCENTAJE_HUMO_FUERTE;
    // Aplicar histéresis: para salir de un nivel, el porcentaje debe bajar más de lo que subió
    if (porcentajeCambio >= umbralFuerte) humoFuerte = true;
    else if (porcentajeCambio >= umbralModerado) humoModerado = true;
    else if (porcentajeCambio >= umbralLeve) humoLeve = true;
    // Si el DO digital está activo, consideramos humo como mínimo moderado
    if (humoDigital) humoModerado = true;
  } else {
    // Si no hay AO, usamos solo el DO
    if (humoDigital) humoModerado = true;
  }

  // ---- DECISIÓN DE RIESGO ----
  bool condicionModerado = false;
  bool condicionAlerta = false;

  // Riesgo moderado: temperatura alta O humedad baja O humo leve
  if (tempRiesgo || humRiesgo || humoLeve) {
    condicionModerado = true;
  }

  // Alerta máxima: (temperatura crítica Y humedad crítica) Y (humo moderado o DO activo)
  // O también si humo fuerte independientemente
  if (humoFuerte || (tempCritica && humCritica && (humoModerado || humoDigital))) {
    condicionAlerta = true;
  }

  // ---- PERSISTENCIA ----
  Riesgo nuevoRiesgo = Riesgo::NORMAL;

  if (condicionAlerta) {
    contadorAlertaMaxima++;
    contadorRiesgoModerado = 0;
    if (contadorAlertaMaxima >= LECTURAS_PARA_ALERTA_MAXIMA) {
      nuevoRiesgo = Riesgo::ALERTA;
    } else {
      // Mientras no se confirme, mantenemos el estado anterior si era ALERTA
      nuevoRiesgo = (riesgoActual == Riesgo::ALERTA) ? Riesgo::ALERTA : Riesgo::MODERADO;
    }
  }
  else if (condicionModerado) {
    contadorRiesgoModerado++;
    contadorAlertaMaxima = 0;
    if (contadorRiesgoModerado >= LECTURAS_PARA_RIESGO_MODERADO) {
      nuevoRiesgo = Riesgo::MODERADO;
    } else {
      nuevoRiesgo = (riesgoActual == Riesgo::ALERTA) ? Riesgo::ALERTA : Riesgo::MODERADO;
    }
  }
  else {
    // Condiciones normales: necesitamos varias lecturas para volver a NORMAL (histéresis)
    contadorNormal++;
    if (contadorNormal >= LECTURAS_PARA_VOLVER_NORMAL) {
      nuevoRiesgo = Riesgo::NORMAL;
      contadorRiesgoModerado = 0;
      contadorAlertaMaxima = 0;
    } else {
      // Mantener el estado actual mientras no se confirme la vuelta
      nuevoRiesgo = riesgoActual;
    }
  }

  // Actualizar solo si ha cambiado y se ha confirmado
  if (nuevoRiesgo != riesgoActual) {
    // Si el nuevo riesgo es menor (ej. ALERTA→MODERADO), también necesitamos confirmación
    // pero ya lo manejamos con los contadores.
    riesgoActual = nuevoRiesgo;
    // Resetear contadores de confirmación para evitar cambios bruscos
    if (nuevoRiesgo == Riesgo::NORMAL) {
      contadorRiesgoModerado = 0;
      contadorAlertaMaxima = 0;
    }
    if (nuevoRiesgo == Riesgo::MODERADO) {
      contadorAlertaMaxima = 0;
    }
    contadorNormal = 0;
  }

  // Si no se cumplen condiciones, pero los contadores no han llegado al umbral,
  // el estado se mantiene (eso ya está implementado con las condiciones else).
}

// =============================================================
//  DIAGNÓSTICO SERIAL
// =============================================================
void serialDiagnostico() {
  Serial.println();
  Serial.println("================================================");
  Serial.println("              FORESTGUARD");
  Serial.println("================================================");

  // Temperatura y humedad
  if (dhtOK) {
    Serial.print("🌡️  Temperatura : "); Serial.print(temperatura, 1); Serial.println(" °C");
    Serial.print("💧  Humedad     : "); Serial.print(humedad, 1); Serial.println(" %");
  } else {
    Serial.println("⚠️  DHT11: SIN DATOS");
  }

  // MQ-2
  Serial.print("📡  MQ-2 AO     : "); Serial.println(mq2AO);
  if (aoDisponible) {
    Serial.print("🔵  AO BASE     : "); Serial.println(aoBase);
    Serial.print("📊  Cambio AO   : "); Serial.println(cambioAO);
    Serial.print("📈  Porcentaje  : "); Serial.print(porcentajeCambio * 100, 1); Serial.println(" %");
  } else {
    Serial.println("⚠️  AO no disponible (usando DO)");
  }
  Serial.print("🔴  MQ-2 DO     : "); Serial.println(mq2DO);
  Serial.print("🔴  Humo DO     : "); Serial.println(humoDigital ? "SI" : "NO");

  // Estado del sistema
  const char* estadoTexto;
  switch (riesgoActual) {
    case Riesgo::NORMAL:   estadoTexto = "🟢 NORMAL"; break;
    case Riesgo::MODERADO: estadoTexto = "🟡 RIESGO MODERADO"; break;
    case Riesgo::ALERTA:   estadoTexto = "🔴 ALERTA MÁXIMA"; break;
    default: estadoTexto = "❓ DESCONOCIDO";
  }
  Serial.print("🚨  Estado      : "); Serial.println(estadoTexto);

  // Confirmaciones (debug)
  Serial.print("   Conf. MOD   : "); Serial.print(contadorRiesgoModerado);
  Serial.print("/"); Serial.print(LECTURAS_PARA_RIESGO_MODERADO);
  Serial.print("   ALERTA: "); Serial.print(contadorAlertaMaxima);
  Serial.print("/"); Serial.println(LECTURAS_PARA_ALERTA_MAXIMA);

  // WiFi y Flask
  Serial.print("📶  WiFi        : "); Serial.println(wifiConectado ? "CONECTADO" : "DESCONECTADO");
  Serial.print("🌐  Flask       : "); Serial.println(flaskDisponible ? "DISPONIBLE" : "NO DISPONIBLE");

  Serial.println("================================================");
}

// =============================================================
//  ENVÍO A FLASK (con manejo de errores)
// =============================================================
void enviarDatos() {
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
  doc["temperatura"] = temperatura;
  doc["humedad"] = humedad;
  doc["mq2_ao"] = mq2AO;
  doc["mq2_base"] = aoBase;
  doc["cambio_ao"] = cambioAO;
  doc["mq2_do"] = mq2DO;
  doc["nivel_humo"] = (int)riesgoActual; // 0=NORMAL, 1=MODERADO, 2=ALERTA
  doc["mac"] = stationCode;
  doc["porcentaje_cambio"] = porcentajeCambio;

  String payload;
  serializeJson(doc, payload);

  int codigo = http.POST(payload);
  if (codigo > 0) {
    Serial.print("[HTTP] Código: "); Serial.println(codigo);
    if (codigo == 200 || codigo == 201) {
      flaskDisponible = true;
      ultimoEnvioExitoso = millis();
    } else {
      Serial.println("[HTTP] Error del servidor");
    }
  } else {
    Serial.print("[HTTP] Falló: "); Serial.println(http.errorToString(codigo));
    if (codigo == -1) flaskDisponible = false;
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
//  SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("           FORESTGUARD v2.0");
  Serial.println("      SISTEMA DE MONITOREO");
  Serial.println("======================================");

  // LED PWM (CORREGIDO PARA ESP32 CORE 3.x)
  ledcAttach(LED_ROJO, FRECUENCIA, RESOLUCION);
  ledcAttach(LED_VERDE, FRECUENCIA, RESOLUCION);
  ledcAttach(LED_AZUL, FRECUENCIA, RESOLUCION);
  ledApagado();

  // Pines
  pinMode(MQ2_AO, INPUT);
  pinMode(MQ2_DO, INPUT);
  dht.begin();

  // Secuencia de diagnóstico LED
  ledRojo(); delay(300);
  ledVerde(); delay(300);
  ledAzul(); delay(300);
  ledAmarillo(); delay(300);
  ledApagado();

  // Calentamiento y calibración MQ-2 (60 segundos)
  calibrarMQ2();

  // Conectar WiFi (después de la calibración para no interferir)
  conectarWiFi();

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
  Serial.println("ENVIO A FLASK: CADA 5 SEGUNDOS");
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

  // Recalibración periódica (cada 10 minutos) si el sistema está estable
  // (esto se podría añadir más adelante)

  // Lectura de sensores cada INTERVALO_LECTURA
  if (millis() - ultimaLectura >= INTERVALO_LECTURA) {
    ultimaLectura = millis();
    leerSensores();
    evaluarRiesgo();
    actualizarLED();
  }

  // Monitor serie cada 2 segundos
  if (millis() - ultimoSerial >= 2000) {
    ultimoSerial = millis();
    serialDiagnostico();
  }

  // Envío a Flask cada 5 segundos
  if (millis() - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = millis();
    enviarDatos();
  }

  // Pequeña pausa para no saturar el bucle
  delay(50);
}