#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ============================================================
//                     FORESTGUARD
//              SISTEMA DE MONITOREO
// ============================================================
// DHT22 -> Temperatura + Humedad
// MQ-2  -> Detección de humo/gases
// RGB   -> Estado del sistema
//
// 🔵 AZUL     = CALIBRACIÓN INICIAL (60 segundos)
// 🟢 VERDE    = NORMAL
// 🟡 AMARILLO = RIESGO MODERADO
// 🟠 NARANJO  = RIESGO ALTO
// 🔴 ROJO     = ALERTA / POSIBLE INCENDIO
// 🔵 AZUL     = ERROR DHT22 (parpadeo)
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
//                  CONFIGURACIÓN FLASK (IP FIJA)
// ============================================================
const char* SERVER_HOST = "192.168.18.116";
const int SERVER_PORT = 5000;

// ============================================================
//                  IDENTIFICACIÓN ESTACIÓN (MAC)
// ============================================================
String stationCode = "";
const char* DEFAULT_API_KEY = "3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c";

// ============================================================
//                  URL DEL SERVIDOR
// ============================================================
String serverUrl() {
  return String("http://") + SERVER_HOST + ":" + SERVER_PORT +
         "/api/estaciones/" + stationCode + "/datos";
}

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
//              UMBRALES (iguales para la ESP32 y Flask)
// ============================================================
const float TEMP_RIESGO  = 30.0;
const float TEMP_ALTA    = 35.0;
const float TEMP_CRITICA = 40.0;
const float HUMEDAD_RIESGO  = 45.0;
const float HUMEDAD_BAJA    = 30.0;
const float HUMEDAD_CRITICA = 15.0;
const int HUMO_BAJO  = 100;
const int HUMO_MEDIO = 300;
const int HUMO_ALTO  = 500;

int estadoDONormal = HIGH;
int aoBase = 0;

// ============================================================
//                       TIEMPOS
// ============================================================
const unsigned long INTERVALO_LECTURA = 2000;
const unsigned long INTERVALO_ENVIO = 5000;
const unsigned long TIEMPO_CALIBRACION = 60000;  // 60 segundos

unsigned long ultimaLectura = 0;
unsigned long ultimoEnvio = 0;

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

// ============================================================
//                 SECUENCIA DE ENCENDIDO
// ============================================================
void secuenciaInicio() {
  Serial.println();
  Serial.println("======================================");
  Serial.println("           FORESTGUARD");
  Serial.println("       INICIANDO SISTEMA");
  Serial.println("======================================");
  ledRojo(); delay(300);
  ledVerde(); delay(300);
  ledAzul(); delay(300);
  ledAmarillo(); delay(300);
  ledNaranjo(); delay(300);
  ledApagado();
  Serial.println("LED RGB -> OK");
}

// ============================================================
//                    PRUEBAS
// ============================================================
void pruebaLED() {
  Serial.println();
  Serial.println("------ PRUEBA LED RGB ------");
  ledRojo(); Serial.println("ROJO"); delay(800);
  ledVerde(); Serial.println("VERDE"); delay(800);
  ledAzul(); Serial.println("AZUL"); delay(800);
  ledAmarillo(); Serial.println("AMARILLO"); delay(800);
  ledNaranjo(); Serial.println("NARANJO"); delay(800);
  ledApagado(); Serial.println("APAGADO"); delay(500);
  Serial.println("LED RGB -> OK");
}

bool pruebaDHT22() {
  Serial.println();
  Serial.println("------ PRUEBA DHT22 ------");
  delay(2000);
  float temperatura = dht.readTemperature();
  float humedad = dht.readHumidity();
  if (isnan(temperatura) || isnan(humedad)) {
    Serial.println("DHT22 -> ERROR");
    return false;
  }
  Serial.println("DHT22 -> OK");
  Serial.print("Temperatura: "); Serial.print(temperatura, 1); Serial.println(" C");
  Serial.print("Humedad: "); Serial.print(humedad, 1); Serial.println(" %");
  return true;
}

void pruebaMQ2() {
  Serial.println();
  Serial.println("------ PRUEBA MQ-2 ------");
  int ao = analogRead(MQ2_AO);
  int estadoDO = digitalRead(MQ2_DO);
  Serial.print("AO = "); Serial.println(ao);
  Serial.print("DO = "); Serial.println(estadoDO);
  Serial.println("MQ-2 -> SEÑAL RECIBIDA");
}

// ============================================================
//                 CALIBRACIÓN MQ-2 (60 segundos, LED AZUL)
// ============================================================
void calibrarMQ2() {
  Serial.println();
  Serial.println("======================================");
  Serial.println("          CALIBRANDO MQ-2");
  Serial.println("======================================");
  Serial.println("NO acerques humo.");
  Serial.println("NO acerques cigarros.");
  Serial.println("NO acerques fuego.");
  Serial.println("Midiendo ambiente normal durante 60 segundos...");
  Serial.println();

  ledAzul();  // 🔵 AZUL durante calibración
  unsigned long inicio = millis();
  long sumaAO = 0;
  int cantidad = 0;
  int high = 0, low = 0;

  while (millis() - inicio < TIEMPO_CALIBRACION) {
    int ao = analogRead(MQ2_AO);
    int estadoDO = digitalRead(MQ2_DO);
    sumaAO += ao;
    cantidad++;
    if (estadoDO == HIGH) high++; else low++;
    delay(250);
    // Mostrar progreso cada 5 segundos
    if ((millis() - inicio) % 5000 < 250) {
      Serial.print(".");
    }
  }
  Serial.println();

  if (cantidad > 0) aoBase = sumaAO / cantidad;
  estadoDONormal = (high >= low) ? HIGH : LOW;

  Serial.println();
  Serial.println("--------------------------------------");
  Serial.print("AO BASE     = "); Serial.println(aoBase);
  Serial.print("DO NORMAL   = "); Serial.println(estadoDONormal);
  Serial.println("--------------------------------------");
  Serial.println("CALIBRACION TERMINADA");
  ledVerde();  // Volver a verde
}

// ============================================================
//                  CLASIFICAR HUMO
// ============================================================
int obtenerNivelHumo(int cambioAO, bool humoDO) {
  if (cambioAO >= HUMO_ALTO) return 3;
  if (cambioAO >= HUMO_MEDIO) return 2;
  if (cambioAO >= HUMO_BAJO) return 1;
  if (humoDO) return 1;
  return 0;
}

void imprimirNivelHumo(int nivelHumo) {
  Serial.print("NIVEL HUMO  : ");
  if (nivelHumo == 0) Serial.println("NINGUNO");
  else if (nivelHumo == 1) Serial.println("BAJO");
  else if (nivelHumo == 2) Serial.println("MEDIO");
  else Serial.println("ALTO");
}

// ============================================================
//              MOSTRAR CONDICIONES
// ============================================================
void mostrarCondiciones(float temp, float hum, int nivelHumo, bool humoDO) {
  Serial.println();
  Serial.println("-------- CONDICIONES --------");
  if (temp >= TEMP_CRITICA) Serial.println("TEMPERATURA CRITICA");
  else if (temp >= TEMP_ALTA) Serial.println("TEMPERATURA ALTA");
  else if (temp >= TEMP_RIESGO) Serial.println("TEMPERATURA ELEVADA");

  if (hum <= HUMEDAD_CRITICA) Serial.println("HUMEDAD CRITICAMENTE BAJA");
  else if (hum <= HUMEDAD_BAJA) Serial.println("HUMEDAD BAJA");
  else if (hum <= HUMEDAD_RIESGO) Serial.println("HUMEDAD REDUCIDA");

  if (nivelHumo == 3) Serial.println("HUMO ALTO DETECTADO");
  else if (nivelHumo == 2) Serial.println("HUMO MEDIO DETECTADO");
  else if (nivelHumo == 1) Serial.println("HUMO BAJO DETECTADO");

  if (humoDO) Serial.println("MQ-2 DIGITAL: DETECCION");
}

// ============================================================
//                  EVALUAR FORESTGUARD
// ============================================================
void evaluarForestGuard(float temp, float hum, int nivelHumo, int cambioAO, bool humoDO) {
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
//                       WIFI
// ============================================================
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
    Serial.print("Servidor Flask: "); Serial.println(serverUrl());
  } else {
    Serial.println("WIFI -> ERROR");
    Serial.println("El monitoreo local continuará.");
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
}

// ============================================================
//                 ENVIAR DATOS A FLASK
// ============================================================
void enviarDatosFlask(float temperatura, float humedad, int mq2AO, int mq2Base, int cambioAO, int mq2DO, int nivelHumo) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No se envia: WiFi desconectado");
    return;
  }

  HTTPClient http;
  String url = serverUrl();
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
  doc["mac"] = stationCode;   // Enviamos la MAC para verificar

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
//                       AUTOTEST
// ============================================================
void autoTest() {
  Serial.println();
  Serial.println("######################################");
  Serial.println("#          FORESTGUARD ESP32         #");
  Serial.println("#             AUTOTEST               #");
  Serial.println("######################################");

  Serial.println(); Serial.println("[1/3] PRUEBA LED RGB");
  pruebaLED();

  Serial.println(); Serial.println("[2/3] PRUEBA DHT22");
  bool dhtOK = pruebaDHT22();

  Serial.println(); Serial.println("[3/3] PRUEBA MQ-2");
  pruebaMQ2();

  Serial.println(); Serial.println("CALIBRACION MQ-2 (60 segundos con LED AZUL)");
  calibrarMQ2();

  Serial.println();
  Serial.println("======================================");
  Serial.println("          AUTOTEST TERMINADO");
  Serial.println("======================================");
  if (dhtOK) Serial.println("DHT22 -> OK");
  else Serial.println("DHT22 -> ERROR");
  Serial.println("MQ-2 AO -> SEÑAL RECIBIDA");
  Serial.println("MQ-2 DO -> SEÑAL RECIBIDA");
  Serial.println();
  ledVerde();
  delay(1000);
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

  Serial.println();
  Serial.println("DHT22 OUT -> GPIO 13");
  Serial.println("MQ-2 AO   -> GPIO 35");
  Serial.println("MQ-2 DO   -> GPIO 33");
  Serial.println("LED ROJO  -> GPIO 26");
  Serial.println("LED VERDE -> GPIO 27");
  Serial.println("LED AZUL  -> GPIO 25");

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

  // INICIO
  secuenciaInicio();
  delay(1000);

  // AUTOTEST (incluye calibración de 60 segundos con LED azul)
  autoTest();

  // WIFI
  conectarWiFi();

  Serial.println();
  Serial.println("======================================");
  Serial.println("       FORESTGUARD LISTO");
  Serial.println("======================================");
  Serial.println("MONITOREO ACTIVO");
  Serial.println("ENVIO A FLASK: CADA 5 SEGUNDOS");
  Serial.println();

  ledVerde();
}

// ============================================================
//                          LOOP
// ============================================================
void loop() {
  comprobarWiFi();

  unsigned long tiempoActual = millis();
  if (tiempoActual - ultimaLectura < INTERVALO_LECTURA) return;
  ultimaLectura = tiempoActual;

  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  int aoActual = analogRead(MQ2_AO);
  int doActual = digitalRead(MQ2_DO);

  if (isnan(temp) || isnan(hum)) {
    Serial.println();
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("          ERROR DHT22");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ledAzul();
    return;
  }

  int deltaAO = aoActual - aoBase;
  int cambioAO = abs(deltaAO);
  bool humoDO = (doActual != estadoDONormal);
  int nivelHumo = obtenerNivelHumo(cambioAO, humoDO);

  Serial.println();
  Serial.println("--------------------------------------");
  Serial.print("Temperatura : "); Serial.print(temp, 1); Serial.println(" C");
  Serial.print("Humedad     : "); Serial.print(hum, 1); Serial.println(" %");
  Serial.print("MQ-2 AO     : "); Serial.println(aoActual);
  Serial.print("AO BASE     : "); Serial.println(aoBase);
  Serial.print("CAMBIO AO   : "); Serial.println(cambioAO);
  Serial.print("MQ-2 DO     : "); Serial.println(doActual);

  imprimirNivelHumo(nivelHumo);
  mostrarCondiciones(temp, hum, nivelHumo, humoDO);
  evaluarForestGuard(temp, hum, nivelHumo, cambioAO, humoDO);

  if (tiempoActual - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = tiempoActual;
    enviarDatosFlask(temp, hum, aoActual, aoBase, cambioAO, doActual, nivelHumo);
  }

  Serial.println("--------------------------------------");
}