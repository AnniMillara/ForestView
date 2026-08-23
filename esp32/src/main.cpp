#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <ESPmDNS.h>

// ============================================================
//                     FORESTGUARD ESP32
//              SISTEMA DE MONITOREO (VERSION MEJORADA)
// ============================================================
// 🔵 AZUL     = CALIBRACIÓN INICIAL / RECALIBRACIÓN
// 🟢 VERDE    = NORMAL
// 🟡 AMARILLO = RIESGO MODERADO
// 🟠 NARANJO  = RIESGO ALTO
// 🔴 ROJO     = ALERTA / POSIBLE INCENDIO
// 💡 PARPADEO AZUL = ERROR DHT22
// ============================================================

// ============================================================
//                         PINES
// ============================================================
#define DHT_PIN 13
#define DHT_TYPE DHT22
#define MQ2_AO 35
#define MQ2_DO 33
#define LED_ROJO  26
#define LED_VERDE 27
#define LED_AZUL  25

// ============================================================
//                    CONFIGURACIÓN WIFI
// ============================================================
const char* WIFI_SSID = "GameofThrones";
const char* WIFI_PASSWORD = "elsenordelosanillos";

// ============================================================
//              CONFIGURACIÓN DEL SERVIDOR (mDNS + FALLBACK)
// ============================================================
const char* SERVER_HOSTNAME = "forestguard";   // nombre mDNS: forestguard.local
const char* SERVER_HOST_FALLBACK = "192.168.18.116";  // IP fija por si mDNS falla
const int SERVER_PORT = 5000;

// ============================================================
//                  IDENTIFICACIÓN ESTACIÓN (MAC)
// ============================================================
String stationCode = "";
const char* DEFAULT_API_KEY = "3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c";
String serverIP = "";

// ============================================================
//                         DHT22
// ============================================================
DHT dht(DHT_PIN, DHT_TYPE);

// ============================================================
//                  CONFIGURACIÓN PWM
// ============================================================
const int FRECUENCIA = 5000;
const int RESOLUCION = 8;
const int CANAL_ROJO  = 0;
const int CANAL_VERDE = 1;
const int CANAL_AZUL  = 2;

// ============================================================
//              UMBRALES (ajustables)
// ============================================================
const float TEMP_RIESGO  = 30.0;
const float TEMP_ALTA    = 35.0;
const float TEMP_CRITICA = 40.0;
const float HUMEDAD_RIESGO  = 45.0;
const float HUMEDAD_BAJA    = 30.0;
const float HUMEDAD_CRITICA = 15.0;

// Umbrales de cambio AO (se aplican sobre la base dinámica)
const int HUMO_CAMBIO_BAJO  = 50;   // Reducido para mayor sensibilidad pero con filtro
const int HUMO_CAMBIO_MEDIO = 150;
const int HUMO_CAMBIO_ALTO  = 300;

// ============================================================
//                    VARIABLES DE CALIBRACIÓN
// ============================================================
int aoBase = 0;
int estadoDONormal = HIGH;
unsigned long ultimaCalibracion = 0;
const unsigned long INTERVALO_RECALIBRACION = 600000; // 10 minutos

// ============================================================
//                    FILTRO DE LECTURAS
// ============================================================
const int MUESTRAS_PROMEDIO = 5;
float tempBuffer[MUESTRAS_PROMEDIO];
float humBuffer[MUESTRAS_PROMEDIO];
int indiceBuffer = 0;
bool bufferLleno = false;

// ============================================================
//               PERSISTENCIA PARA DETECCIÓN DE HUMO
// ============================================================
const int LECTURAS_PARA_ALERTA = 3;  // Número de lecturas consecutivas para activar alerta
int contadorHumoBajo = 0;
int contadorHumoMedio = 0;
int contadorHumoAlto = 0;

// ============================================================
//                       TIEMPOS
// ============================================================
const unsigned long INTERVALO_LECTURA = 2000;   // 2 segundos entre lecturas
const unsigned long INTERVALO_ENVIO = 5000;     // 5 segundos entre envíos
const unsigned long TIEMPO_CALIBRACION = 30000; // 30 segundos de calibración inicial

unsigned long ultimaLectura = 0;
unsigned long ultimoEnvio = 0;
unsigned long inicioCalibracion = 0;

// ============================================================
//                    DECLARACIONES DE FUNCIONES
// ============================================================
void ledApagado();
void ledVerde();
void ledRojo();
void ledAzul();
void ledAmarillo();
void ledNaranjo();
void ledParpadeoAzul();
void calibrarMQ2(const char* motivo);
bool leerDHT22(float &temp, float &hum);
void agregarMuestra(float temp, float hum);
float obtenerPromedio(float* buffer);
int clasificarHumoConPersistencia(int cambioAO, bool humoDO);
void evaluarForestGuard(float temp, float hum, int nivelHumo);
void conectarWiFi();
void resolverServidor();
void comprobarWiFi();
void enviarDatosFlask(float temperatura, float humedad, int mq2AO, int mq2Base, int cambioAO, int mq2DO, int nivelHumo);

// ============================================================
//                    FUNCIONES LED
// ============================================================
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
  static unsigned long ultimoParpadeo = 0;
  if (millis() - ultimoParpadeo > 500) {
    ultimoParpadeo = millis();
    static bool estado = false;
    estado = !estado;
    if (estado) ledAzul(); else ledApagado();
  }
}

// ============================================================
//                 CALIBRACIÓN MQ-2 (dinámica)
// ============================================================
void calibrarMQ2(const char* motivo) {
  Serial.println();
  Serial.println("======================================");
  Serial.print("   CALIBRANDO MQ-2: ");
  Serial.println(motivo);
  Serial.println("======================================");
  Serial.println("Mantén el sensor en aire limpio.");
  ledAzul();

  unsigned long inicio = millis();
  long sumaAO = 0;
  int cantidad = 0;
  int high = 0, low = 0;

  // Tomar 20 muestras en 5 segundos
  while (millis() - inicio < 5000) {
    int ao = analogRead(MQ2_AO);
    int estadoDO = digitalRead(MQ2_DO);
    sumaAO += ao;
    cantidad++;
    if (estadoDO == HIGH) high++; else low++;
    delay(250);
  }

  if (cantidad > 0) aoBase = sumaAO / cantidad;
  estadoDONormal = (high >= low) ? HIGH : LOW;

  Serial.println("--------------------------------------");
  Serial.print("AO BASE     = "); Serial.println(aoBase);
  Serial.print("DO NORMAL   = "); Serial.println(estadoDONormal);
  Serial.println("--------------------------------------");
  Serial.println("CALIBRACION TERMINADA");
  ledVerde();
  ultimaCalibracion = millis();
}

// ============================================================
//                  LECTURA DHT22 CON REINTENTOS
// ============================================================
bool leerDHT22(float &temp, float &hum) {
  const int MAX_INTENTOS = 5;
  for (int i = 0; i < MAX_INTENTOS; i++) {
    temp = dht.readTemperature();
    hum = dht.readHumidity();
    if (!isnan(temp) && !isnan(hum)) {
      return true;
    }
    delay(100);
  }
  return false;
}

// ============================================================
//                    FILTRO DE PROMEDIO MÓVIL
// ============================================================
void agregarMuestra(float temp, float hum) {
  tempBuffer[indiceBuffer] = temp;
  humBuffer[indiceBuffer] = hum;
  indiceBuffer++;
  if (indiceBuffer >= MUESTRAS_PROMEDIO) {
    indiceBuffer = 0;
    bufferLleno = true;
  }
}

float obtenerPromedio(float* buffer) {
  if (!bufferLleno) return buffer[0];
  float suma = 0;
  for (int i = 0; i < MUESTRAS_PROMEDIO; i++) {
    suma += buffer[i];
  }
  return suma / MUESTRAS_PROMEDIO;
}

// ============================================================
//                  CLASIFICAR HUMO CON PERSISTENCIA
// ============================================================
int clasificarHumoConPersistencia(int cambioAO, bool humoDO) {
  // Primero determinamos el nivel actual sin persistencia
  int nivelActual = 0;
  if (cambioAO >= HUMO_CAMBIO_ALTO || humoDO) nivelActual = 3;
  else if (cambioAO >= HUMO_CAMBIO_MEDIO) nivelActual = 2;
  else if (cambioAO >= HUMO_CAMBIO_BAJO) nivelActual = 1;
  else nivelActual = 0;

  // Lógica de persistencia: solo cambiamos de nivel si se mantiene varias veces
  static int ultimoNivel = 0;
  static int contador = 0;

  if (nivelActual == ultimoNivel) {
    contador++;
  } else {
    contador = 1;
    ultimoNivel = nivelActual;
  }

  if (contador >= LECTURAS_PARA_ALERTA) {
    return nivelActual;
  } else {
    // Si no hemos alcanzado el número de lecturas, devolvemos el nivel anterior
    // pero solo si ya teníamos un nivel establecido (evita falsos al inicio)
    static int nivelEstable = 0;
    if (contador == 1 && ultimoNivel == 0) {
      // Reinicio: volvemos a 0 si es la primera lectura
      nivelEstable = 0;
    } else if (contador >= LECTURAS_PARA_ALERTA) {
      nivelEstable = nivelActual;
    }
    return nivelEstable;
  }
}

// ============================================================
//                  EVALUAR FORESTGUARD
// ============================================================
void evaluarForestGuard(float temp, float hum, int nivelHumo) {
  bool tempRiesgo = temp >= TEMP_RIESGO;
  bool tempAlta = temp >= TEMP_ALTA;
  bool tempCritica = temp >= TEMP_CRITICA;
  bool humRiesgo = hum <= HUMEDAD_RIESGO;
  bool humBaja = hum <= HUMEDAD_BAJA;
  bool humCritica = hum <= HUMEDAD_CRITICA;

  bool alertaMaxima = false;
  bool riesgoAlto = false;
  bool riesgoModerado = false;

  // ALERTA MÁXIMA
  if (nivelHumo == 3) alertaMaxima = true;
  else if (nivelHumo == 2 && tempAlta) alertaMaxima = true;
  else if (nivelHumo == 2 && humBaja) alertaMaxima = true;
  else if (nivelHumo == 1 && tempCritica) alertaMaxima = true;
  else if (nivelHumo == 1 && humCritica) alertaMaxima = true;
  else if (tempCritica && humBaja) alertaMaxima = true;
  else if (nivelHumo >= 1 && tempAlta && humBaja) alertaMaxima = true;

  // RIESGO ALTO
  if (!alertaMaxima) {
    if (nivelHumo == 2) riesgoAlto = true;
    if (nivelHumo == 1 && tempAlta) riesgoAlto = true;
    if (nivelHumo == 1 && humBaja) riesgoAlto = true;
    if (tempAlta && humBaja) riesgoAlto = true;
  }

  // RIESGO MODERADO
  if (!alertaMaxima && !riesgoAlto) {
    if (nivelHumo == 1) riesgoModerado = true;
    if (tempRiesgo) riesgoModerado = true;
    if (humRiesgo) riesgoModerado = true;
  }

  // LED + ESTADO
  if (alertaMaxima) {
    ledRojo();
    Serial.println();
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("       ALERTA FORESTGUARD");
    Serial.println("          POSIBLE INCENDIO");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  } else if (riesgoAlto) {
    ledNaranjo();
    Serial.println();
    Serial.println("======================================");
    Serial.println("       RIESGO ALTO");
    Serial.println("        FORESTGUARD");
    Serial.println("======================================");
  } else if (riesgoModerado) {
    ledAmarillo();
    Serial.println();
    Serial.println("======================================");
    Serial.println("       RIESGO MODERADO");
    Serial.println("        FORESTGUARD");
    Serial.println("======================================");
  } else {
    ledVerde();
    Serial.println();
    Serial.println("======================================");
    Serial.println("       FORESTGUARD NORMAL");
    Serial.println("======================================");
    Serial.println("SIN HUMO SIGNIFICATIVO");
    Serial.println("TEMPERATURA SEGURA");
    Serial.println("HUMEDAD SEGURA");
    Serial.println("MONITOREO ACTIVO");
  }
}

// ============================================================
//                       WIFI + mDNS
// ============================================================
void resolverServidor() {
  // Intentar resolver por mDNS
  IPAddress ip;
  if (MDNS.queryHost(SERVER_HOSTNAME, ip)) {
    serverIP = ip.toString();
    Serial.print("Servidor encontrado por mDNS: ");
    Serial.println(serverIP);
  } else {
    Serial.println("No se pudo resolver por mDNS, usando IP fija de fallback.");
    serverIP = SERVER_HOST_FALLBACK;
  }
}

void conectarWiFi() {
  Serial.println();
  Serial.println("======================================");
  Serial.println("             CONEXION WIFI");
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
    stationCode = WiFi.macAddress();
    stationCode.replace(":", "");
    Serial.print("Código estación (MAC): "); Serial.println(stationCode);

    // Iniciar mDNS
    if (MDNS.begin("forestguard-esp")) {
      Serial.println("mDNS iniciado como forestguard-esp.local");
    } else {
      Serial.println("Error al iniciar mDNS");
    }

    // Resolver el servidor por mDNS
    resolverServidor();
  } else {
    Serial.println("WIFI -> ERROR");
    Serial.println("El monitoreo local continuará.");
    serverIP = SERVER_HOST_FALLBACK; // fallback
  }
}

void comprobarWiFi() {
  static unsigned long ultimaComprobacion = 0;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - ultimaComprobacion < 10000) return;
  ultimaComprobacion = millis();
  Serial.println();
  Serial.println("WIFI DESCONECTADO");
  Serial.println("Intentando reconectar...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Si se reconecta, volver a resolver el servidor
  if (WiFi.status() == WL_CONNECTED) {
    resolverServidor();
  }
}

// ============================================================
//                 ENVIAR DATOS A FLASK
// ============================================================
void enviarDatosFlask(float temperatura, float humedad, int mq2AO, int mq2Base, int cambioAO, int mq2DO, int nivelHumo) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No se envia: WiFi desconectado");
    return;
  }

  if (serverIP.length() == 0) {
    Serial.println("No se envia: IP del servidor desconocida");
    resolverServidor();
    return;
  }

  HTTPClient http;
  String url = "http://" + serverIP + ":" + String(SERVER_PORT) +
               "/api/estaciones/" + stationCode + "/datos";
  Serial.println();
  Serial.println("------ ENVIO A FLASK ------");
  Serial.print("URL: "); Serial.println(url);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  JsonDocument doc;
  doc["api_key"] = DEFAULT_API_KEY;
  doc["temperatura"] = temperatura;
  doc["humedad"] = humedad;
  doc["mq2_ao"] = mq2AO;
  doc["mq2_base"] = mq2Base;
  doc["cambio_ao"] = cambioAO;
  doc["mq2_do"] = mq2DO;
  doc["nivel_humo"] = nivelHumo;
  doc["mac"] = stationCode;

  String payload;
  serializeJson(doc, payload);
  Serial.print("JSON: "); Serial.println(payload);

  int codigoHTTP = http.POST(payload);
  if (codigoHTTP > 0) {
    Serial.print("HTTP: "); Serial.println(codigoHTTP);
    String respuesta = http.getString();
    Serial.print("Respuesta Flask: "); Serial.println(respuesta);
  } else {
    Serial.print("ERROR HTTP: "); Serial.println(http.errorToString(codigoHTTP));
  }
  http.end();
}

// ============================================================
//                         SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("           FORESTGUARD");
  Serial.println("      SISTEMA DE MONITOREO");
  Serial.println("======================================");

  // LED RGB
  ledcSetup(CANAL_ROJO, FRECUENCIA, RESOLUCION);
  ledcSetup(CANAL_VERDE, FRECUENCIA, RESOLUCION);
  ledcSetup(CANAL_AZUL, FRECUENCIA, RESOLUCION);
  ledcAttachPin(LED_ROJO, CANAL_ROJO);
  ledcAttachPin(LED_VERDE, CANAL_VERDE);
  ledcAttachPin(LED_AZUL, CANAL_AZUL);
  ledApagado();

  // MQ-2
  pinMode(MQ2_AO, INPUT);
  pinMode(MQ2_DO, INPUT);
  // DHT22
  dht.begin();

  // Secuencia de inicio
  ledRojo(); delay(300);
  ledVerde(); delay(300);
  ledAzul(); delay(300);
  ledAmarillo(); delay(300);
  ledNaranjo(); delay(300);
  ledApagado();

  // Calibración inicial MQ-2 (30 segundos)
  calibrarMQ2("INICIAL");

  // WiFi
  conectarWiFi();

  // Inicializar buffers
  for (int i = 0; i < MUESTRAS_PROMEDIO; i++) {
    tempBuffer[i] = 0;
    humBuffer[i] = 0;
  }

  Serial.println();
  Serial.println("======================================");
  Serial.println("       FORESTGUARD LISTO");
  Serial.println("======================================");
  Serial.println("MONITOREO ACTIVO");
  Serial.println("ENVIO A FLASK: CADA 5 SEGUNDOS");
  Serial.println("RECALIBRACION MQ-2: CADA 10 MINUTOS");
  Serial.println();
}

// ============================================================
//                          LOOP
// ============================================================
void loop() {
  comprobarWiFi();

  // Recalibración periódica del MQ-2 (cada 10 minutos)
  if (millis() - ultimaCalibracion > INTERVALO_RECALIBRACION) {
    calibrarMQ2("RECALIBRACION PERIODICA");
  }

  unsigned long tiempoActual = millis();
  if (tiempoActual - ultimaLectura < INTERVALO_LECTURA) return;
  ultimaLectura = tiempoActual;

  // Lectura del DHT22 con reintentos
  float temp, hum;
  bool dhtOK = leerDHT22(temp, hum);

  if (!dhtOK) {
    Serial.println();
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("          ERROR DHT22");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ledParpadeoAzul();  // Parpadeo azul
    return;
  }

  // Agregar al filtro de promedio móvil
  agregarMuestra(temp, hum);
  float tempFiltrada = obtenerPromedio(tempBuffer);
  float humFiltrada = obtenerPromedio(humBuffer);

  // Lectura MQ-2
  int aoActual = analogRead(MQ2_AO);
  int doActual = digitalRead(MQ2_DO);

  int deltaAO = aoActual - aoBase;
  int cambioAO = abs(deltaAO);
  bool humoDO = (doActual != estadoDONormal);

  // Clasificar humo con persistencia (evita falsas alarmas)
  int nivelHumo = clasificarHumoConPersistencia(cambioAO, humoDO);

  // Mostrar en serial
  Serial.println();
  Serial.println("--------------------------------------");
  Serial.print("Temperatura : "); Serial.print(tempFiltrada, 1); Serial.println(" C");
  Serial.print("Humedad     : "); Serial.print(humFiltrada, 1); Serial.println(" %");
  Serial.print("MQ-2 AO     : "); Serial.println(aoActual);
  Serial.print("AO BASE     : "); Serial.println(aoBase);
  Serial.print("CAMBIO AO   : "); Serial.println(cambioAO);
  Serial.print("MQ-2 DO     : "); Serial.println(doActual);
  Serial.print("NIVEL HUMO  : ");
  if (nivelHumo == 0) Serial.println("NINGUNO");
  else if (nivelHumo == 1) Serial.println("BAJO");
  else if (nivelHumo == 2) Serial.println("MEDIO");
  else Serial.println("ALTO");

  // Evaluar riesgo y actualizar LED
  evaluarForestGuard(tempFiltrada, humFiltrada, nivelHumo);

  // Enviar a Flask cada 5 segundos
  if (tiempoActual - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = tiempoActual;
    enviarDatosFlask(tempFiltrada, humFiltrada, aoActual, aoBase, cambioAO, doActual, nivelHumo);
  }

  Serial.println("--------------------------------------");
}