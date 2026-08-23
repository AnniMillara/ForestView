/*
  ForestGuard - Firmware ESP32
  Envía datos a Flask usando codigo en URL y API key en JSON
*/
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <ArduinoJson.h>

// ============ CONFIGURACIÓN ============
const char* WIFI_SSID     = "GameofThrones";
const char* WIFI_PASSWORD = "elsenordelosanillos";
const char* SERVER_HOST = "192.168.18.116";
const int   SERVER_PORT   = 5000;
const char* CODIGO_ESTACION = "A1B2C3D4";
const char* API_KEY         = "3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c";

String serverUrl() {
    return String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/api/estaciones/" + CODIGO_ESTACION + "/datos";
}

// ============ PINES ============
#define DHTPIN   13
#define DHTTYPE  DHT22
#define MQ2_AO   35
#define MQ2_DO   33
#define LED_R    26
#define LED_G    27
#define LED_B    25

DHT dht(DHTPIN, DHTTYPE);

// ============ PROMEDIO MÓVIL ============
const int NUM_LECTURAS = 10;
int lecturasAO[NUM_LECTURAS];
int indexLectura = 0;
long totalAO = 0;
int aoBase = 0;
const unsigned long INTERVALO_ENVIO = 5000;

void setup() {
    Serial.begin(115200);
    dht.begin();
    pinMode(MQ2_DO, INPUT);
    pinMode(LED_R, OUTPUT);
    pinMode(LED_G, OUTPUT);
    pinMode(LED_B, OUTPUT);

    // WiFi
    Serial.print("Conectando a WiFi");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi conectado");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Servidor: ");
    Serial.println(serverUrl());

    // Calibración MQ-2
    long sum = 0;
    for (int i = 0; i < 50; i++) {
        sum += analogRead(MQ2_AO);
        delay(50);
    }
    aoBase = sum / 50;
    Serial.print("AO Base: ");
    Serial.println(aoBase);

    for (int i = 0; i < NUM_LECTURAS; i++) lecturasAO[i] = aoBase;
    totalAO = (long)aoBase * NUM_LECTURAS;
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi desconectado, reconectando...");
        WiFi.reconnect();
        delay(2000);
        return;
    }

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    bool dhtOk = !isnan(t) && !isnan(h);

    int ao = analogRead(MQ2_AO);
    int doVal = digitalRead(MQ2_DO);

    // Promedio móvil
    totalAO = totalAO - lecturasAO[indexLectura];
    lecturasAO[indexLectura] = ao;
    totalAO = totalAO + ao;
    indexLectura = (indexLectura + 1) % NUM_LECTURAS;
    int aoPromedio = totalAO / NUM_LECTURAS;
    int cambioAO = aoPromedio - aoBase;

    // LED RGB
    if (!dhtOk || cambioAO < 0) {
        digitalWrite(LED_R, LOW); digitalWrite(LED_G, LOW); digitalWrite(LED_B, HIGH);  // Azul = error
    } else if (cambioAO >= 550 || (cambioAO >= 300 && t >= 35)) {
        digitalWrite(LED_R, HIGH); digitalWrite(LED_G, LOW); digitalWrite(LED_B, LOW);   // Rojo
    } else if (cambioAO >= 100 || t >= 32) {
        digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); digitalWrite(LED_B, LOW);  // Amarillo
    } else {
        digitalWrite(LED_R, LOW); digitalWrite(LED_G, HIGH); digitalWrite(LED_B, LOW);   // Verde
    }

    // Serial
    Serial.println("========== LECTURA ==========");
    Serial.print("DHT22 OK: "); Serial.println(dhtOk ? "SI" : "NO");
    if (dhtOk) {
        Serial.print("Temp: "); Serial.print(t); Serial.println(" C");
        Serial.print("Humedad: "); Serial.print(h); Serial.println(" %");
    }
    Serial.print("MQ2 AO: "); Serial.println(ao);
    Serial.print("AO Base: "); Serial.println(aoBase);
    Serial.print("Cambio AO: "); Serial.println(cambioAO);
    Serial.print("MQ2 DO: "); Serial.println(doVal);

    // Enviar a Flask
    HTTPClient http;
    http.begin(serverUrl());
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    StaticJsonDocument<256> doc;
    doc["api_key"] = API_KEY;
    if (dhtOk) {
        doc["temperatura"] = t;
        doc["humedad"] = h;
    } else {
        doc["temperatura"] = nullptr;
        doc["humedad"] = nullptr;
    }
    doc["mq2_ao"] = ao;
    doc["mq2_base"] = aoBase;
    doc["cambio_ao"] = cambioAO;
    doc["mq2_do"] = doVal;

    String payload;
    serializeJson(doc, payload);

    int httpCode = http.POST(payload);
    if (httpCode > 0) {
        String respuesta = http.getString();
        Serial.print("HTTP: "); Serial.println(httpCode);
        Serial.print("Respuesta: "); Serial.println(respuesta);
    } else {
        Serial.print("Error HTTP: ");
        Serial.println(http.errorToString(httpCode));
    }
    http.end();

    delay(INTERVALO_ENVIO);
}