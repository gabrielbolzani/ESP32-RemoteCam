#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <esp32cam.h>
#include <esp32cam-asyncweb.h>
#include <ESPAsyncWebServer.h>
#include <esp_camera.h>
#include <Preferences.h>

// Pins for AI-Thinker ESP32-CAM
#define FLASH_LED_PIN 4
#define STATUS_LED_PIN 33
#define LEDC_CHANNEL 0
#define LEDC_FREQ 5000
#define LEDC_RES 10

extern "C" {
  uint8_t temprature_sens_read();
}

using namespace esp32cam;

char mdnsName[33] = "webcam"; 
bool useFlashAuto = false;
int flashIntensity = 255; 
bool cameraReady = false; 
Preferences prefs;

AsyncWebServer server(80);
bool shouldStartPortal = false;
bool shouldRestart = false;

// Resource Lock
SemaphoreHandle_t camLock = NULL;

Resolution maxRes = Resolution::find(1600, 1200);
Resolution streamRes = Resolution::find(640, 480);

void setFlash(int val) {
    ledcWrite(LEDC_CHANNEL, val);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n--- ESP32-REMOTECAM ---");

    camLock = xSemaphoreCreateMutex();

    ledcSetup(LEDC_CHANNEL, LEDC_FREQ, LEDC_RES);
    ledcAttachPin(FLASH_LED_PIN, LEDC_CHANNEL);
    setFlash(0);

    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, HIGH);

    prefs.begin("cam-pro", false);
    strncpy(mdnsName, prefs.getString("mdns", "webcam").c_str(), sizeof(mdnsName));
    useFlashAuto = prefs.getBool("flash_auto", false);
    flashIntensity = prefs.getInt("flash_br", 255);

    Config cfg;
    cfg.setPins(esp32cam::pins::AiThinker);
    cfg.setResolution(maxRes);
    cfg.setBufferCount(1); 
    cfg.setJpeg(85);

    cameraReady = Camera.begin(cfg);
    if (cameraReady) {
        Camera.changeResolution(streamRes);
    }

    WiFiManager wm;
    wm.setConnectTimeout(60);
    if (!wm.autoConnect("ESP32-RemoteCam-Config")) {
        ESP.restart();
    }

    if (MDNS.begin(mdnsName)) {
        Serial.printf("mDNS: http://%s.local\n", mdnsName);
    }

    // MAIN DASHBOARD
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<!DOCTYPE html><html><head><title>RemoteCam</title><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:'Segoe UI',Arial;text-align:center;background:#050505;color:#fff;margin:0;padding-top:40px;}";
        html += ".card{background:#151515;border-radius:25px;padding:30px;max-width:500px;margin:auto;box-shadow:0 20px 50px rgba(0,0,0,0.8);border:1px solid #222;}";
        html += ".stat-row{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;padding:12px;background:#1a1a1a;border-radius:12px;border:1px solid #333;}";
        html += ".prog-bg{background:#000;width:100px;height:10px;border-radius:5px;overflow:hidden;margin-left:10px; border:1px solid #444;}";
        html += ".prog-fill{height:100%;transition:0.5s;width:0%;}";
        html += ".btn{background:#3d5afe;color:white;padding:15px 25px;text-decoration:none;border-radius:12px;margin:8px;display:inline-block;font-weight:bold;border:none;cursor:pointer;transition:0.2s;text-transform:uppercase;font-size:14px;}";
        html += ".btn:hover{filter:brightness(1.5); transform:translateY(-2px);} .btn-led-on{background:#ffab00;color:#000;} .btn-stream{background:#00c853;color:#000;} .btn-config{background:#444;font-size:12px; opacity:0.8;}</style>";
        html += "<script>";
        html += "function fmtUp(s){let h=Math.floor(s/3600),m=Math.floor(s%3600/60),se=s%60; return (h?h+'h ':'')+(m?m+'m ':'')+se+'s';}";
        html += "function getStatus(){fetch('/status').then(r=>r.json()).then(d=>{";
        html += "  document.getElementById('rssi_val').innerText = d.wifi_pct + '%';";
        html += "  document.getElementById('rssi_bar').style.width = d.wifi_pct + '%';";
        html += "  document.getElementById('rssi_bar').style.background = d.wifi_pct > 60 ? '#00c853' : (d.wifi_pct > 30 ? '#ffab00' : '#d50000');";
        html += "  document.getElementById('ram_val').innerText = d.ram_pct + '%';";
        html += "  document.getElementById('ram_bar').style.width = d.ram_pct + '%';";
        html += "  document.getElementById('temp_val').innerHTML = d.temp + '&deg;C';";
        html += "  document.getElementById('up_val').innerText = fmtUp(d.uptime);";
        html += "});} setInterval(getStatus, 2000); getStatus();";
        html += "</script></head><body><div class='card'>";
        html += "<h2 style='margin-top:0;color:#3d5afe;'>RemoteCam</h2>";
        html += "<p style='margin-bottom:20px;color:#888;font-size:14px;'>" + String(mdnsName) + ".local</p>";
        
        html += "<div class='stat-row'><span>Sinal WiFi</span> <div style='display:flex;align-items:center;'><span id='rssi_val'>...</span><div class='prog-bg'><div id='rssi_bar' class='prog-fill'></div></div></div></div>";
        html += "<div class='stat-row'><span>Uso SRAM</span> <div style='display:flex;align-items:center;'><span id='ram_val'>...</span><div class='prog-bg'><div id='rssi_bar' class='prog-fill' style='background:#3d5afe;'></div></div></div></div>";
        html += "<div class='stat-row'><span>Temperatura</span> <span id='temp_val' style='font-weight:bold;color:#ffab00;'>...</span></div>";
        html += "<div class='stat-row'><span>Uptime</span> <span id='up_val'>...</span></div>";

        if(!cameraReady) html += "<div style='color:#d50000;margin:10px;font-weight:bold;'>ERRO NO SENSOR DA CÂMERA! Verifique os cabos.</div>";

        html += "<div style='margin-top:20px;'><a href='/stream' class='btn btn-stream' target='_blank'>stream</a>";
        html += "<a href='/snapshot' class='btn' target='_blank'>snapshot</a></div>";
        html += "<div style='margin-top:10px;'><button onclick=\"fetch('/led-on')\" class='btn btn-led-on'>FLASH ON</button>";
        html += "<button onclick=\"fetch('/led-off')\" class='btn' style='background:#333;'>FLASH OFF</button></div>";
        
        html += "<div style='margin-top:20px;'><button onclick=\"location.href='/settings'\" class='btn btn-config'>Configurações</button></div>";
        html += "</div></body></html>";
        request->send(200, "text/html", html);
    });

    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
        int wifi_pct = constrain(map(WiFi.RSSI(), -90, -40, 0, 100), 0, 100);
        int ram_pct = constrain((int)(((float)(320000 - ESP.getFreeHeap()) / 320000.0) * 100), 0, 100);
        float temp = (temprature_sens_read() - 32) / 1.8; 
        request->send(200, "application/json", "{\"wifi_pct\":"+String(wifi_pct)+",\"ram_pct\":"+String(ram_pct)+",\"temp\":"+String(temp,1)+",\"uptime\":"+String(millis()/1000)+"}");
    });

    // FAST RAW - Short lock
    server.on("/raw", HTTP_GET, [](AsyncWebServerRequest *request){
        if (xSemaphoreTake(camLock, pdMS_TO_TICKS(100))) {
            esp32cam::asyncweb::handleStill(request);
            xSemaphoreGive(camLock);
        } else { request->send(503, "text/plain", "Busy"); }
    });

    server.on("/snapshot", HTTP_GET, [](AsyncWebServerRequest *request){
        if (xSemaphoreTake(camLock, pdMS_TO_TICKS(3000))) {
            if (useFlashAuto) setFlash(flashIntensity);
            sensor_t * s = esp_camera_sensor_get();
            Camera.changeResolution(maxRes);
            if (s) s->set_quality(s, 10); 
            delay(400); 
            esp32cam::asyncweb::handleStill(request);
            Camera.changeResolution(streamRes);
            if (useFlashAuto) setFlash(0);
            xSemaphoreGive(camLock);
        } else { request->send(503); }
    });

    server.on("/stream", HTTP_GET, [](AsyncWebServerRequest *request){
        if (xSemaphoreTake(camLock, pdMS_TO_TICKS(3000))) {
            if (useFlashAuto) setFlash(flashIntensity);
            request->onDisconnect([](){
                if (useFlashAuto) setFlash(0);
                xSemaphoreGive(camLock); 
            });
            Camera.changeResolution(streamRes);
            esp32cam::asyncweb::handleMjpeg(request);
        } else { request->send(503); }
    });

    // SETTINGS
    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<!DOCTYPE html><html><head><title>Configurações</title><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:'Segoe UI',Arial;text-align:center;background:#050505;color:#fff;padding-top:40px;}.card{background:#151515;border-radius:20px;padding:30px;max-width:400px;margin:auto;border:1px solid #333;}input[type=text]{width:100%;padding:12px;margin:10px 0;background:#222;border:1px solid #444;color:#fff;border-radius:8px;}.btn{background:#3d5afe;color:white;padding:12px 20px;border-radius:8px;border:none;cursor:pointer;width:100%;font-weight:bold;margin-top:10px;} .range-val{font-weight:bold;color:#ffab00;}</style></head><body>";
        html += "<div class='card'><h3>RemoteCam</h3><form action='/save-settings' method='POST'><div style='text-align:left;font-size:12px;color:#888;'>NOME mDNS:</div><input type='text' name='mdns' value='" + String(mdnsName) + "' maxlength='32'>";
        html += "<div style='text-align:left;margin:15px 0;'><input type='checkbox' name='flash' id='flash' " + String(useFlashAuto ? "checked" : "") + "><label for='flash' style='margin-left:10px;'>Flash Automático</label></div>";
        html += "<div style='text-align:left;margin:15px 0;'><label>Intensidade: <span id='v' class='range-val'>" + String(map(flashIntensity,0,255,0,100)) + "</span>%</label><br><input type='range' name='br' min='0' max='255' value='" + String(flashIntensity) + "' style='width:100%' oninput='document.getElementById(\"v\").innerText=Math.round(this.value/2.55)'></div>";
        html += "<button type='submit' class='btn'>SALVAR</button></form><hr style='border:0;border-top:1px solid #333;margin:20px 0;'><button onclick=\"location.href='/wifi-reset'\" class='btn' style='background:#d50000;'>RESET WIFI</button><br><br><a href='/' style='color:#888;'>VOLTAR</a></div></body></html>";
        request->send(200, "text/html", html);
    });

    server.on("/save-settings", HTTP_POST, [](AsyncWebServerRequest *request){
        if (request->hasParam("mdns", true)) {
            String newName = request->getParam("mdns", true)->value();
            if (newName != String(mdnsName)) { prefs.putString("mdns", newName); shouldRestart = true; }
            useFlashAuto = request->hasParam("flash", true);
            prefs.putBool("flash_auto", useFlashAuto);
            if (request->hasParam("br", true)) { flashIntensity = request->getParam("br", true)->value().toInt(); prefs.putInt("flash_br", flashIntensity); setFlash(0); }
            request->send(200, "text/plain", "OK");
        } else { request->send(400); }
    });

    server.on("/led-on", HTTP_GET, [](AsyncWebServerRequest *request){ setFlash(flashIntensity); request->send(200); });
    server.on("/led-off", HTTP_GET, [](AsyncWebServerRequest *request){ setFlash(0); request->send(200); });
    server.on("/wifi-reset", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(200, "text/plain", "AP Mode..."); shouldStartPortal = true; });

    server.begin();
}

void loop() {
    if (shouldStartPortal) {
        WiFiManager *wm = new WiFiManager();
        wm->startConfigPortal("ESP32-RemoteCam-Config");
        delete wm;
        shouldStartPortal = false;
        ESP.restart();
    }
    if (shouldRestart) { delay(1000); ESP.restart(); }
}
