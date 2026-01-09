#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// --- CONFIGURAÇÕES DE HARDWARE ---
#define PIN_BATTERY      0
#define PIN_USB_DETECT   6 
#define LED_PIN_R        3
#define LED_PIN_G        4
#define LED_PIN_B        5 
#define LOW_BAT_THRESHOLD 20

// --- ESTRUTURAS ---
struct Config {
    bool apModeOnly;
    String currentSSID;
};

// --- GLOBAIS ---
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
QueueHandle_t gameQueue;
Config sysConfig = {false, ""}; // Padrão

// Voláteis para Tasks
volatile int globalBatteryPct = 100;
volatile bool isCharging = false;

// --- GERENCIAMENTO DE ARQUIVOS (JSON) ---
// Carrega configurações globais
void loadConfig() {
    if (!SPIFFS.exists("/config.json")) return;
    File file = SPIFFS.open("/config.json", "r");
    StaticJsonDocument<512> doc;
    deserializeJson(doc, file);
    sysConfig.apModeOnly = doc["apOnly"] | false;
    sysConfig.currentSSID = doc["lastSSID"] | "";
    file.close();
}

void saveConfig() {
    File file = SPIFFS.open("/config.json", "w");
    StaticJsonDocument<512> doc;
    doc["apOnly"] = sysConfig.apModeOnly;
    doc["lastSSID"] = sysConfig.currentSSID;
    serializeJson(doc, file);
    file.close();
}

// Tenta conectar no WiFi salvo
void connectToWiFi() {
    if (sysConfig.apModeOnly) {
        WiFi.mode(WIFI_AP);
        Serial.println("Modo Apenas AP ativado.");
        return;
    }

    WiFi.mode(WIFI_AP_STA);
    
    if (sysConfig.currentSSID != "") {
        // Busca a senha no arquivo de redes
        if (SPIFFS.exists("/networks.json")) {
            File file = SPIFFS.open("/networks.json", "r");
            DynamicJsonDocument doc(2048);
            deserializeJson(doc, file);
            file.close();
            
            JsonArray networks = doc.as<JsonArray>();
            for (JsonObject net : networks) {
                if (net["ssid"] == sysConfig.currentSSID) {
                    const char* pass = net["pass"];
                    WiFi.begin(sysConfig.currentSSID.c_str(), pass);
                    Serial.print("Conectando a: "); Serial.println(sysConfig.currentSSID);
                    return;
                }
            }
        }
    }
}

// --- FUNÇÕES DE LED (Mantidas) ---
void setLedColor(bool r, bool g, bool b) {
    digitalWrite(LED_PIN_R, r); digitalWrite(LED_PIN_G, g); digitalWrite(LED_PIN_B, b);
}

void ledTask(void *parameter) {
    pinMode(LED_PIN_R, OUTPUT); pinMode(LED_PIN_G, OUTPUT); pinMode(LED_PIN_B, OUTPUT);
    setLedColor(0, 0, 0);
    while (true) {
        int pct = globalBatteryPct;
        bool charging = isCharging;
        bool r=0, g=0;

        if (pct < 20) { r=1; g=0; }
        else if (pct >= 20 && pct < 60) { r=1; g=1; }
        else { r=0; g=1; }

        if (charging) {
            if (pct >= 99) { setLedColor(0, 1, 0); vTaskDelay(pdMS_TO_TICKS(500)); }
            else {
                setLedColor(r, g, 0); vTaskDelay(pdMS_TO_TICKS(500));
                setLedColor(0, 0, 0); vTaskDelay(pdMS_TO_TICKS(500));
            }
        } else {
            setLedColor(r, g, 0); vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

// --- TASK BATERIA ---
void batteryTask(void *parameter) {
    pinMode(PIN_USB_DETECT, INPUT);
    while (true) {
        uint32_t raw = analogRead(PIN_BATTERY);
        float voltage = (raw / 4095.0) * 3.3 * 2.0; 
        int pct = (int)((voltage - 3.2) * 100.0 / (4.2 - 3.2));
        if (pct > 100) pct = 100; if (pct < 0) pct = 0;
        
        globalBatteryPct = pct;
        isCharging = digitalRead(PIN_USB_DETECT); // Use false se não tiver o circuito

        if (ws.count() > 0) {
            String json = "{\"type\":\"battery\",\"val\":" + String(pct) + "}";
            ws.textAll(json);
        }

        if (pct < LOW_BAT_THRESHOLD && !isCharging) {
            setLedColor(1, 0, 0); vTaskDelay(2000);
            ws.closeAll(); WiFi.mode(WIFI_OFF); esp_deep_sleep_start();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// --- TASK JOGO ---
typedef struct { uint32_t client_id; String payload; } GameMessage;
void gameLogicTask(void *parameter) {
    GameMessage msg;
    while (true) {
        if (xQueueReceive(gameQueue, &msg, portMAX_DELAY) == pdTRUE) {
            if (msg.payload.indexOf("join") >= 0) {
                String welcome = "{\"type\":\"welcome\",\"id\":" + String(msg.client_id) + ",\"bat\":" + String(globalBatteryPct) + "}";
                ws.text(msg.client_id, welcome);
            } else {
                String broadcastMsg = "{\"id\":" + String(msg.client_id) + "," + msg.payload.substring(1); 
                ws.textAll(broadcastMsg);
            }
        }
    }
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        GameMessage msg = {client->id(), "{\"type\":\"join\"}"};
        xQueueSend(gameQueue, &msg, portMAX_DELAY);
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            data[len] = 0;
            GameMessage msg = {client->id(), String((char*)data)};
            xQueueSend(gameQueue, &msg, 0);
        }
    }
}

// --- CONFIGURAÇÃO WEB SERVER & API ---
// --- CONFIGURAÇÃO WEB SERVER & API ---
void setupServer() {
    // 1. API: Listar Redes Salvas
    server.on("/api/networks", HTTP_GET, [](AsyncWebServerRequest *request){
        if (SPIFFS.exists("/networks.json")) {
            request->send(SPIFFS, "/networks.json", "application/json");
        } else {
            request->send(200, "application/json", "[]");
        }
    });

    // 2. API: Adicionar Rede
    server.on("/api/add_network", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, data);
        String newSSID = doc["ssid"];
        String newPass = doc["pass"];

        DynamicJsonDocument netDoc(2048);
        if(SPIFFS.exists("/networks.json")) {
            File r = SPIFFS.open("/networks.json", "r");
            deserializeJson(netDoc, r);
            r.close();
        }
        
        JsonArray arr = netDoc.to<JsonArray>();
        for (int i=0; i<arr.size(); i++) {
            if (arr[i]["ssid"] == newSSID) { arr.remove(i); break; }
        }
        
        JsonObject obj = arr.createNestedObject();
        obj["ssid"] = newSSID;
        obj["pass"] = newPass;

        File w = SPIFFS.open("/networks.json", "w");
        serializeJson(netDoc, w);
        w.close();
        request->send(200, "text/plain", "Salvo");
    });

    // 3. API: Deletar Rede
    server.on("/api/delete_network", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        DynamicJsonDocument doc(512);
        deserializeJson(doc, data);
        String targetSSID = doc["ssid"];

        File r = SPIFFS.open("/networks.json", "r");
        DynamicJsonDocument netDoc(2048);
        deserializeJson(netDoc, r);
        r.close();

        JsonArray arr = netDoc.as<JsonArray>();
        for (int i=0; i<arr.size(); i++) {
            if (arr[i]["ssid"] == targetSSID) {
                arr.remove(i);
                File w = SPIFFS.open("/networks.json", "w");
                serializeJson(netDoc, w);
                w.close();
                request->send(200, "text/plain", "Deletado");
                return;
            }
        }
        request->send(404, "text/plain", "Nao encontrado");
    });

    // 4. API: Conectar / Mudar Modo
    server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        DynamicJsonDocument doc(512);
        deserializeJson(doc, data);
        if (doc.containsKey("apOnly")) sysConfig.apModeOnly = doc["apOnly"];
        if (doc.containsKey("connectSSID")) {
            sysConfig.currentSSID = doc["connectSSID"].as<String>();
            sysConfig.apModeOnly = false;
        }
        saveConfig();
        request->send(200, "text/plain", "Configurado.");
        vTaskDelay(100);
        connectToWiFi();
    });

    // 5. API: Status
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{";
        json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"apMode\":" + String(sysConfig.apModeOnly ? "true" : "false") + ",";
        json += "\"currentSSID\":\"" + sysConfig.currentSSID + "\"";
        json += "}";
        request->send(200, "application/json", json);
    });

    // 6. API: Iniciar Scan (NOVO)
    server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *request){
        // Scan async = true (não trava o servidor)
        WiFi.scanNetworks(true);
        request->send(200, "text/plain", "Scan Iniciado");
    });

    // 7. API: Pegar Resultados (NOVO)
    server.on("/api/scan_results", HTTP_GET, [](AsyncWebServerRequest *request){
        int n = WiFi.scanComplete();
        if(n == -2) {
            WiFi.scanNetworks(true); // Falhou? tenta de novo
            request->send(202, "text/plain", "Reiniciando scan...");
        } else if(n == -1) {
            request->send(202, "text/plain", "Escaneando..."); // Ainda rodando
        } else {
            String json = "[";
            for (int i = 0; i < n; ++i) {
                if(i) json += ",";
                json += "{";
                json += "\"ssid\":\"" + WiFi.SSID(i) + "\",";
                json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
                // encryptionType 0 é OPEN
                json += "\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false");
                json += "}";
            }
            json += "]";
            WiFi.scanDelete(); // Limpa memória do scan
            request->send(200, "application/json", json);
        }
    });

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
    server.begin();
}

// --- SETUP ---
void setup() {
    Serial.begin(115200);
    analogReadResolution(12);

    if (!SPIFFS.begin(true)) return;
    loadConfig();

    gameQueue = xQueueCreate(20, sizeof(GameMessage));

    // WiFi Setup Inicial
    WiFi.softAP("ARCADE_SETUP", ""); // AP sempre existe para fallback
    connectToWiFi(); // Tenta conectar no salvo

    if (MDNS.begin("arcade")) Serial.println("mDNS OK");

    setupServer();

    xTaskCreatePinnedToCore(gameLogicTask, "GameTask", 4096, NULL, 1, NULL, 1);
    xTaskCreate(batteryTask, "BatTask", 2048, NULL, 1, NULL);
    xTaskCreate(ledTask, "LedTask", 2048, NULL, 1, NULL);
}

void loop() { vTaskDelay(1000); }