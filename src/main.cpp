#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <qrcode.h> // Instalar "QRCode" por ricmoo

// --- CONFIGURAÇÕES OLED ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- PINOS SD CARD (SPI) ---
#define SD_SCK  4
#define SD_MISO 5
#define SD_MOSI 6
#define SD_CS   7

// --- CONFIGURAÇÕES DE REDE ---
IPAddress staticIP(192, 168, 0, 33);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
const char* hostname = "arcade"; // Acessar via http://arcade.local

AsyncWebServer server(80);

// --- FUNÇÃO PARA DESENHAR QR CODE NO OLED ---
void drawQRCode(String data) {
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcodeData, 3, 0, data.c_str());

    display.clearDisplay();
    // Centraliza o QR Code
    int offset_x = (SCREEN_WIDTH - qrcode.size * 2) / 2;
    int offset_y = 10;

    for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
            if (qrcode_getModule(&qrcode, x, y)) {
                display.fillRect(x * 2 + offset_x, y * 2 + offset_y, 2, 2, SSD1306_WHITE);
            }
        }
    }
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("ESCANEIE PARA JOGAR");
    display.display();
}

// --- ADICIONE ESTAS VARIÁVEIS NO TOPO ---
unsigned long displayTimer = 0;
const long displayTimeout = 60000; // 60 segundos
bool displayOn = true;
#define BAT_PIN 0 // Pino onde o divisor de tensão está ligado

// --- FUNÇÃO PARA LER BATERIA ---
float getBatteryVoltage() {
    int raw = analogRead(BAT_PIN);
    // 3.1V é a ref do ADC do ESP32, 2.0 é o fator do divisor 10k/10k
    float voltage = (raw / 4095.0) * 3.1 * 2.0; 
    return map(voltage * 100, 300, 420, 0, 100) / 100.0; // Retorna em Volts
}

String listGames() {
  String html = "<!DOCTYPE html><html lang='pt-br'><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>ESP32 Arcade Lobby</title>";
  
  // CSS Moderno e Responsivo
  html += "<style>";
  html += "  @import url('https://fonts.googleapis.com/css2?family=Press+Start+2P&family=Rajdhani:wght@500;700&display=swap');";
  html += "  :root { --primary: #f1c40f; --accent: #2ecc71; --bg: #0f0f1b; --card-bg: #1e1e2f; }";
  html += "  body { background: var(--bg); color: #fff; font-family: 'Rajdhani', sans-serif; margin: 0; padding: 20px; display: flex; flex-direction: column; align-items: center; min-height: 100vh; }";
  html += "  h1 { font-family: 'Press Start 2P', cursive; font-size: 18px; color: var(--primary); text-shadow: 3px 3px #c0392b; margin-bottom: 40px; text-align: center; line-height: 1.5; }";
  
  // Container dos Cards
  html += "  .lobby { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 20px; width: 100%; max-width: 600px; }";
  
  // Estilo do Botão/Card de Jogo
  html += "  .game-card { background: var(--card-bg); border: 2px solid #3d3d5c; border-radius: 15px; padding: 25px 15px; text-decoration: none; color: #fff; ";
  html += "               display: flex; flex-direction: column; align-items: center; justify-content: center; transition: all 0.3s cubic-bezier(0.175, 0.885, 0.32, 1.275); ";
  html += "               box-shadow: 0 10px 20px rgba(0,0,0,0.5); position: relative; overflow: hidden; }";
  
  // Efeito de Hover (Passar o mouse ou toque)
  html += "  .game-card:active, .game-card:hover { transform: scale(1.05); border-color: var(--accent); box-shadow: 0 0 15px var(--accent); background: #252545; }";
  
  // Ícone Genérico (Emoji)
  html += "  .game-card i { font-size: 40px; margin-bottom: 15px; }";
  html += "  .game-name { font-weight: 700; font-size: 16px; text-transform: uppercase; letter-spacing: 1px; text-align: center; }";
  
  // Footer informando sobre o sistema
  html += "  .footer { margin-top: auto; padding-top: 40px; font-size: 12px; color: #5d5d7d; text-transform: uppercase; letter-spacing: 2px; }";
  html += "  .pulse { animation: pulse-animation 2s infinite; }";
  html += "  @keyframes pulse-animation { 0% { opacity: 1; } 50% { opacity: 0.4; } 100% { opacity: 1; } }";
  html += "</style></head><body>";

  html += "<h1>🕹️ ARCADE<br>SYSTEM</h1>";
  html += "<div class='lobby'>";

  File root = SD.open("/");
  if (!root) {
    html += "<p>Erro: Cartão SD não montado</p>";
  } else {
    File file = root.openNextFile();
    while (file) {
      String fileName = file.name();
      if (!file.isDirectory() && fileName.endsWith(".html") && !fileName.startsWith(".")) {
        String displayName = fileName;
        displayName.replace("/", "");
        displayName.replace(".html", "");
        displayName.toUpperCase();

        html += "<a href='" + fileName + "' class='game-card'>";
        html += "  <i>🎮</i>";
        html += "  <span class='game-name'>" + displayName + "</span>";
        html += "</a>";
      }
      file = root.openNextFile();
    }
  }

  html += "</div>";
html += "<div class='footer pulse'>Sistema Online - 192.168.0.33 | Bateria: " + String(getBatteryVoltage(), 1) + "V</div>";
  html += "</body></html>";
  return html;
}

void setup() {
    Serial.begin(115200);

    // 1. INICIALIZA OLED
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
        Serial.println("Erro OLED");
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,10);
    display.println("INICIALIZANDO...");
    display.display();

    // 2. INICIALIZA SD
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS)) {
        display.println("ERRO SD CARD!");
        display.display();
    }

    // 3. WIFI MANAGER
    WiFiManager wm;
    display.println("AGUARDANDO WIFI...");
    display.display();

    wm.setSTAStaticIPConfig(staticIP, gateway, subnet, IPAddress(8,8,8,8));
    
    // Se não conectar, cria o AP "ESP-ARCADE-SETUP"
    if(!wm.autoConnect("ESP-ARCADE-SETUP")) {
        display.clearDisplay();
        display.setCursor(0,0);
        display.println("CONECTE NO WIFI:");
        display.println("ESP-ARCADE-SETUP");
        display.display();
    }

    // 4. mDNS
    MDNS.begin(hostname);

    // 5. SERVIDOR E OTA
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/html", listGames());
    });
    server.serveStatic("/", SD, "/");
    
    ArduinoOTA.setHostname("esp-arcade");
    ArduinoOTA.begin();
    server.begin();

    // 6. MOSTRA QR CODE FINAL
    // Gera QR Code para o IP fixo
    drawQRCode("http://192.168.0.33");
}

void loop() {
    ArduinoOTA.handle();
}