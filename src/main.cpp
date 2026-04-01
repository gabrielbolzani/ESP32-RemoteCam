#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <esp32cam.h>
#include <esp32cam-asyncweb.h>
#include <ESPAsyncWebServer.h>
#include <esp_camera.h>
#include <esp_system.h>
#include <Preferences.h>

// Pins for AI-Thinker ESP32-CAM
#define FLASH_LED_PIN   4
#define STATUS_LED_PIN  33
#define LEDC_CHANNEL    0
#define LEDC_FREQ       5000
#define LEDC_RES        10

// Warmup frames (não bloqueia WDT pois fb_get espera o hardware)
#define CAM_WARMUP_FRAMES 5

extern "C" {
  uint8_t temprature_sens_read();
}

using namespace esp32cam;

char mdnsName[33]   = "webcam";
bool useFlashAuto   = false;
int  flashIntensity = 255;
bool cameraReady    = false;
String lastRestartReason = "Desconhecido";
Preferences prefs;

AsyncWebServer server(80);
bool shouldStartPortal = false;
bool shouldRestart     = false;

SemaphoreHandle_t camLock = NULL;
volatile bool streamActive = false;

// RESOLUCOES:
// Padronizado tudo em 640x480 para evitar falhas de buffer e estabilidade total
Resolution snapshotRes = Resolution::find(640, 480);
Resolution streamRes   = Resolution::find(640, 480);

// -----------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------
void setFlash(int val) {
    ledcWrite(LEDC_CHANNEL, val);
}

#define LOG(fmt, ...) Serial.printf("[%6lus | %4uKB] " fmt "\n", \
    millis()/1000, ESP.getFreeHeap()/1024, ##__VA_ARGS__)

void camWarmup(int frames = CAM_WARMUP_FRAMES) {
    for (int i = 0; i < frames; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
    }
}

String getRestartReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "Power-on";
        case ESP_RST_SW:        return "Software";
        case ESP_RST_PANIC:     return "Panic/Crash";
        case ESP_RST_INT_WDT:   return "Watchdog (Int)";
        case ESP_RST_TASK_WDT:  return "Watchdog (Task)";
        case ESP_RST_WDT:       return "Watchdog";
        case ESP_RST_BROWNOUT:  return "Brownout";
        default:                return "Outro";
    }
}

// -----------------------------------------------------------------
// Setup
// -----------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);

    lastRestartReason = getRestartReason();

    Serial.println("\n\n========================================");
    Serial.println("       ESP32-REMOTECAM INICIANDO        ");
    Serial.println("========================================");
    Serial.printf("  Motivo do reinicio: %s\n", lastRestartReason.c_str());

    camLock = xSemaphoreCreateMutex();
    if (camLock == NULL) ESP.restart();

    ledcSetup(LEDC_CHANNEL, LEDC_FREQ, LEDC_RES);
    ledcAttachPin(FLASH_LED_PIN, LEDC_CHANNEL);
    setFlash(0);

    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, HIGH);

    prefs.begin("cam-pro", false);
    String pName = prefs.getString("mdns", "webcam");
    strncpy(mdnsName, pName.c_str(), sizeof(mdnsName)-1);
    mdnsName[sizeof(mdnsName)-1] = '\0';
    useFlashAuto   = prefs.getBool("flash_auto", false);
    flashIntensity = prefs.getInt("flash_br", 255);

    Config cfg;
    cfg.setPins(esp32cam::pins::AiThinker);
    cfg.setResolution(snapshotRes); 
    cfg.setBufferCount(2);
    cfg.setJpeg(80);

    cameraReady = Camera.begin(cfg);
    if (cameraReady) {
        LOG("Camera inicializada em 640x480");
        camWarmup(CAM_WARMUP_FRAMES);
    } else {
        LOG("ERRO: Sensor da camera nao encontrado!");
    }

    WiFiManager wm;
    wm.setConnectTimeout(60);
    if (!wm.autoConnect("ESP32-RemoteCam-Config")) ESP.restart();
    
    LOG("IP: %s | RSSI: %d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());

    if (MDNS.begin(mdnsName)) {
        LOG("mDNS: http://%s.local", mdnsName);
    }

    // Dashboard
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        LOG(">> Dash de %s", request->client()->remoteIP().toString().c_str());
        String html = "<!DOCTYPE html><html><head><title>RemoteCam</title><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:'Segoe UI',Arial;text-align:center;background:#050505;color:#fff;margin:0;padding-top:40px;}";
        html += ".card{background:#151515;border-radius:25px;padding:30px;max-width:500px;margin:auto;box-shadow:0 20px 50px rgba(0,0,0,0.8);border:1px solid #222;}";
        html += ".stat-row{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;padding:12px;background:#1a1a1a;border-radius:12px;border:1px solid #333;}";
        html += ".prog-bg{background:#000;width:100px;height:10px;border-radius:5px;overflow:hidden;margin-left:10px;border:1px solid #444;}";
        html += ".prog-fill{height:100%;transition:0.5s;width:0%;}";
        html += ".btn{background:#3d5afe;color:white;padding:15px 25px;text-decoration:none;border-radius:12px;margin:8px;display:inline-block;font-weight:bold;border:none;cursor:pointer;transition:0.2s;text-transform:uppercase;font-size:14px;}";
        html += ".btn:hover{filter:brightness(1.5);transform:translateY(-2px);}.btn-led-on{background:#ffab00;color:#000;}.btn-stream{background:#00c853;color:#000;}.btn-config{background:#444;font-size:12px;opacity:0.8;}</style>";
        html += "<script>";
        html += "function fmtUp(s){let h=Math.floor(s/3600),m=Math.floor(s%3600/60),se=s%60;return (h?h+'h ':'')+(m?m+'m ':'')+se+'s';}";
        html += "function getStatus(){fetch('/status').then(r=>r.json()).then(d=>{";
        html += "  document.getElementById('rssi_val').innerText=d.wifi_pct+'%';";
        html += "  document.getElementById('rssi_bar').style.width=d.wifi_pct+'%';";
        html += "  document.getElementById('rssi_bar').style.background=d.wifi_pct>60?'#00c853':(d.wifi_pct>30?'#ffab00':'#d50000');";
        html += "  document.getElementById('ram_val').innerText=d.ram_pct+'%';";
        html += "  document.getElementById('ram_bar').style.width=d.ram_pct+'%';";
        html += "  document.getElementById('temp_val').innerHTML=d.temp+'&deg;C';";
        html += "  document.getElementById('up_val').innerText=fmtUp(d.uptime);";
        html += "  document.getElementById('rst_val').innerText=d.restart;";
        html += "});} setInterval(getStatus,3000); getStatus();";
        html += "</script></head><body><div class='card'>";
        html += "<h2 style='margin-top:0;color:#3d5afe;'>RemoteCam</h2>";
        html += "<p style='margin-bottom:20px;color:#888;font-size:14px;'>" + String(mdnsName) + ".local</p>";
        html += "<div class='stat-row'><span>Sinal WiFi</span><div style='display:flex;align-items:center;'><span id='rssi_val'>...</span><div class='prog-bg'><div id='rssi_bar' class='prog-fill'></div></div></div></div>";
        html += "<div class='stat-row'><span>Uso SRAM</span><div style='display:flex;align-items:center;'><span id='ram_val'>...</span><div class='prog-bg'><div id='ram_bar' class='prog-fill' style='background:#3d5afe;'></div></div></div></div>";
        html += "<div class='stat-row'><span>Temperatura</span><span id='temp_val' style='font-weight:bold;color:#ffab00;'>...</span></div>";
        html += "<div class='stat-row'><span>Uptime</span><span id='up_val'>...</span></div>";
        html += "<div class='stat-row'><span>Reiniciado por</span><span id='rst_val' style='font-size:13px;color:#ff6b6b;'>...</span></div>";
        if (!cameraReady) html += "<div style='color:#d50000;margin:10px;font-weight:bold;'>ERRO NO SENSOR DA CAMERA!</div>";
        html += "<div style='margin-top:20px;'><a href='/stream' class='btn btn-stream' target='_blank'>stream</a>";
        html += "<a href='/snapshot' class='btn' target='_blank'>snapshot</a></div>";
        html += "<div style='margin-top:10px;'><button onclick=\"fetch('/led-on')\" class='btn btn-led-on'>FLASH ON</button>";
        html += "<button onclick=\"fetch('/led-off')\" class='btn' style='background:#333;'>FLASH OFF</button></div>";
        html += "<div style='margin-top:20px;'><button onclick=\"location.href='/settings'\" class='btn btn-config'>Configuracoes</button></div>";
        html += "</div></body></html>";
        request->send(200, "text/html", html);
    });

    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
        int wifi_pct   = constrain(map(WiFi.RSSI(), -90, -40, 0, 100), 0, 100);
        uint32_t freeH = ESP.getFreeHeap();
        uint32_t totH  = ESP.getHeapSize();
        int ram_pct    = constrain((int)(((float)(totH - freeH) / (float)totH) * 100), 0, 100);
        float temp     = (temprature_sens_read() - 32) / 1.8;
        String json    = "{\"wifi_pct\":" + String(wifi_pct)
                       + ",\"ram_pct\":"  + String(ram_pct)
                       + ",\"temp\":"     + String(temp, 1)
                       + ",\"uptime\":"   + String(millis() / 1000)
                       + ",\"restart\":\"" + lastRestartReason + "\"}";
        request->send(200, "application/json", json);
    });

    server.on("/raw", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!cameraReady) { request->send(503); return; }
        if (xSemaphoreTake(camLock, pdMS_TO_TICKS(500))) {
            esp32cam::asyncweb::handleStill(request);
            xSemaphoreGive(camLock);
        } else request->send(503);
    });

    server.on("/snapshot", HTTP_GET, [](AsyncWebServerRequest *request){
        LOG(">> /snapshot | cliente: %s", request->client()->remoteIP().toString().c_str());
        if (!cameraReady) { request->send(503); return; }
        if (xSemaphoreTake(camLock, pdMS_TO_TICKS(5000))) {
            if (useFlashAuto) setFlash(flashIntensity);
            
            sensor_t *s = esp_camera_sensor_get();
            if (s) s->set_quality(s, 10);
            
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                LOG("   Snap OK: %u bytes", fb->len);
                request->send_P(200, "image/jpeg", fb->buf, fb->len);
                esp_camera_fb_return(fb);
            } else request->send(500, "text/plain", "Capture failed");

            if (useFlashAuto) setFlash(0);
            xSemaphoreGive(camLock);
        } else request->send(503);
    });

    server.on("/stream", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!cameraReady || streamActive) { request->send(503); return; }
        if (xSemaphoreTake(camLock, pdMS_TO_TICKS(3000))) {
            streamActive = true;
            if (useFlashAuto) setFlash(flashIntensity);
            request->onDisconnect([](){
                if (useFlashAuto) setFlash(0);
                streamActive = false;
                xSemaphoreGive(camLock);
                LOG("<< Stream encerrado");
            });
            esp32cam::asyncweb::handleMjpeg(request);
            LOG(">> Stream iniciado");
        } else request->send(503);
    });

    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<!DOCTYPE html><html><head><title>Config</title><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:'Segoe UI',Arial;text-align:center;background:#050505;color:#fff;padding-top:40px;}.card{background:#151515;border-radius:20px;padding:30px;max-width:400px;margin:auto;border:1px solid #333;}input[type=text]{width:100%;padding:12px;margin:10px 0;background:#222;border:1px solid #444;color:#fff;border-radius:8px;box-sizing:border-box;}.btn{background:#3d5afe;color:white;padding:12px 20px;border-radius:8px;border:none;cursor:pointer;width:100%;font-weight:bold;margin-top:10px;}.range-val{font-weight:bold;color:#ffab00;}</style></head><body>";
        html += "<div class='card'><h3>RemoteCam</h3><form action='/save-settings' method='POST'>";
        html += "<div style='text-align:left;font-size:12px;color:#888;'>mDNS:</div><input type='text' name='mdns' value='" + String(mdnsName) + "'>";
        html += "<div style='text-align:left;margin:15px 0;'><input type='checkbox' name='flash' id='flash' " + String(useFlashAuto ? "checked" : "") + "><label for='flash'>Flash Auto</label></div>";
        html += "<div style='text-align:left;margin:15px 0;'><label>Brilho: <span id='v' class='range-val'>" + String(map(flashIntensity, 0, 255, 0, 100)) + "</span>%</label><br><input type='range' name='br' min='0' max='255' value='" + String(flashIntensity) + "' style='width:100%' oninput='document.getElementById(\"v\").innerText=Math.round(this.value/2.55)'></div>";
        html += "<button type='submit' class='btn'>SALVAR</button></form><br><button onclick=\"location.href='/wifi-reset'\" class='btn' style='background:#d50000;'>RESET WIFI</button><br><br><a href='/' style='color:#888;'>VOLTAR</a></div></body></html>";
        request->send(200, "text/html", html);
    });

    server.on("/save-settings", HTTP_POST, [](AsyncWebServerRequest *request){
        if (request->hasParam("mdns", true)) {
            String newName = request->getParam("mdns", true)->value();
            if (newName.length() > 0) prefs.putString("mdns", newName);
            useFlashAuto = request->hasParam("flash", true);
            prefs.putBool("flash_auto", useFlashAuto);
            if (request->hasParam("br", true)) {
                flashIntensity = request->getParam("br", true)->value().toInt();
                prefs.putInt("flash_br", flashIntensity);
            }
            shouldRestart = (newName != String(mdnsName));
            request->redirect("/");
        } else request->send(400);
    });

    server.on("/led-on", HTTP_GET, [](AsyncWebServerRequest *request){ setFlash(flashIntensity); request->send(200); });
    server.on("/led-off", HTTP_GET, [](AsyncWebServerRequest *request){ setFlash(0); request->send(200); });
    server.on("/wifi-reset", HTTP_GET, [](AsyncWebServerRequest *request){ shouldStartPortal = true; request->send(200, "text/plain", "AP Mode..."); });

    server.begin();
    LOG("Servidor iniciado!");
}

void loop() {
    if (shouldStartPortal) {
        WiFiManager wm;
        wm.startConfigPortal("ESP32-RemoteCam-Config");
        ESP.restart();
    }
    if (shouldRestart) { delay(1000); ESP.restart(); }
    static unsigned long lastWd = 0;
    if (millis() - lastWd > 30000) {
        lastWd = millis();
        if (streamActive) LOG("[WD] Stream ativo");
    }
}
