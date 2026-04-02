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
#include <ElegantOTA.h>

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
AsyncWebSocket ws("/ws");
bool shouldStartPortal = false;
bool shouldRestart     = false;

// Configurações da Câmera
int camResol    = 5;      // SVGA (800x600) default
int camQuality  = 12;
int camBrightness = 0;
int camContrast   = 0;
int camSaturation = 0;
bool camVFlip     = false;
bool camHMirror   = false;

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

void applyCameraSettings() {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    s->set_framesize(s, (framesize_t)camResol);
    s->set_quality(s, camQuality);
    s->set_brightness(s, camBrightness);
    s->set_contrast(s, camContrast);
    s->set_saturation(s, camSaturation);
    s->set_vflip(s, camVFlip);
    s->set_hmirror(s, camHMirror);
    LOG("Configuracoes da camera aplicadas (Res: %d, Qual: %d)", camResol, camQuality);
}

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
    camResol       = prefs.getInt("cam_res", 5); // SVGA
    camQuality     = prefs.getInt("cam_qual", 12);
    camBrightness  = prefs.getInt("cam_brt", 0);
    camContrast    = prefs.getInt("cam_con", 0);
    camSaturation  = prefs.getInt("cam_sat", 0);
    camVFlip       = prefs.getBool("cam_vf", false);
    camHMirror     = prefs.getBool("cam_hm", false);

    Config cfg;
    cfg.setPins(esp32cam::pins::AiThinker);
    // XCLK ultra-estável (8MHz) para evitar falhas de sincronia em UXGA
    cfg.setResolution(Resolution::find(1600, 1200)); 
    cfg.setBufferCount(2); // Double-buffering ajuda na consistência da captura
    cfg.setJpeg(10);

    cameraReady = Camera.begin(cfg);
    if (cameraReady) {
        sensor_t *s = esp_camera_sensor_get();
        if (s) s->set_xclk(s, 0, 8); // 8MHz é o "porto seguro" para a ESP32-CAM
        
        applyCameraSettings(); 
        camWarmup(15); 
        
        // Pisca flash para indicar que iniciou
        setFlash(50);
        delay(100);
        setFlash(0);
        delay(50);
        setFlash(80);
        delay(100);
        setFlash(0);
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

    // Websocket handlers
    ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT) LOG("WS Cliente conectado");
    });
    server.addHandler(&ws);

    // OTA
    ElegantOTA.begin(&server);

    // Dashboard 2.0
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<!DOCTYPE html><html><head><title>ESP32-RemoteCam</title><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<link href='https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600&display=swap' rel='stylesheet'>";
        html += "<style>:root{--bg:#0a0a0c;--card:#16161a;--accent:#3d5afe;--text:#e0e0e0;--green:#00e676;--red:#ff1744;}";
        html += "body{font-family:'Outfit',sans-serif;background:var(--bg);color:var(--text);margin:0;display:flex;justify-content:center;align-items:center;min-height:100vh;}";
        html += ".container{width:90%;max-width:450px;animation:fadeIn 0.8s ease-out;} @keyframes fadeIn{from{opacity:0;transform:translateY(20px);}to{opacity:1;transform:translateY(0);}}";
        html += ".card{background:var(--card);border-radius:24px;padding:32px;box-shadow:0 10px 40px rgba(0,0,0,0.5);border:1px solid #222;position:relative;overflow:hidden;}";
        html += ".card::before{content:'';position:absolute;top:0;left:0;width:100%;height:4px;background:linear-gradient(90deg,transparent,var(--accent),transparent);}";
        html += "h1{margin:0 0 8px;font-weight:600;font-size:28px;background:linear-gradient(45deg,#fff,var(--accent));-webkit-background-clip:text;-webkit-text-fill-color:transparent;}";
        html += ".status-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:24px 0;}";
        html += ".stat-item{background:#1c1c22;padding:16px;border-radius:16px;border:1px solid #2a2a30;transition:0.3s;} .stat-item:hover{border-color:var(--accent);transform:translateY(-2px);}";
        html += ".stat-label{font-size:12px;color:#888;margin-bottom:4px;display:block;text-transform:uppercase;letter-spacing:1px;} .stat-val{font-size:18px;font-weight:600;}";
        html += ".btn-group{display:flex;flex-direction:column;gap:12px;margin-top:24px;}";
        html += ".btn{padding:14px;border-radius:14px;border:none;font-family:inherit;font-weight:600;font-size:15px;cursor:pointer;transition:0.3s;display:flex;align-items:center;justify-content:center;text-decoration:none;gap:8px;}";
        html += ".btn-primary{background:var(--accent);color:white;box-shadow:0 8px 20px rgba(61,90,254,0.3);} .btn-primary:hover{filter:brightness(1.2);transform:scale(1.02);}";
        html += ".btn-outline{background:transparent;border:1px solid #333;color:var(--text);} .btn-outline:hover{background:#222;border-color:var(--accent);}";
        html += ".led-ctrl{display:grid;grid-template-columns:1fr 1fr;gap:12px;} .btn-led-on{background:#ffab00;color:#000;} .btn-led-off{background:#333;color:#fff;}";
        html += "</style><script>";
        html += "const ws = new WebSocket('ws://' + location.host + '/ws');";
        html += "ws.onmessage = (e) => { const d = JSON.parse(e.data);";
        html += "  document.getElementById('rssi').innerText = d.wifi_pct + '%';";
        html += "  document.getElementById('ram').innerText = d.ram_pct + '%';";
        html += "  document.getElementById('temp').innerText = d.temp + '°C';";
        html += "  document.getElementById('upt').innerText = d.uptime + 's';";
        html += "};";
        html += "</script></head><body><div class='container'><div class='card'>";
        html += "<h1>RemoteCam</h1><p style='color:#666;font-size:14px;margin-bottom:0;'>" + String(mdnsName) + ".local</p>";
        html += "<div class='status-grid'>";
        html += "<div class='stat-item'><span class='stat-label'>WiFi</span><span class='stat-val' id='rssi'>...</span></div>";
        html += "<div class='stat-item'><span class='stat-label'>RAM</span><span class='stat-val' id='ram'>...</span></div>";
        html += "<div class='stat-item'><span class='stat-label'>Tempo</span><span class='stat-val' id='temp'>...</span></div>";
        html += "<div class='stat-item'><span class='stat-label'>Uptime</span><span class='stat-val' id='upt'>...</span></div>";
        html += "</div>";
        html += "<div class='btn-group'>";
        html += "<a href='/stream' class='btn btn-primary' target='_blank'>🎥 LIVE STREAM</a>";
        html += "<a href='/snapshot' class='btn btn-outline' target='_blank'>📸 CAPTURAR SNAPSHOT</a>";
        html += "<div class='led-ctrl'><button onclick=\"fetch('/led-on')\" class='btn btn-led-on'>💡 FLASH ON</button>";
        html += "<button onclick=\"fetch('/led-off')\" class='btn btn-led-off'>🌑 FLASH OFF</button></div>";
        html += "<a href='/settings' class='btn btn-outline' style='margin-top:10px;font-size:13px;opacity:0.7;'>⚙️ CONFIGURAÇÕES</a>";
        html += "<a href='/update' class='btn btn-outline' style='font-size:11px;opacity:0.5;border:none;'>OTA Update</a>";
        html += "</div></div></div></body></html>";
        request->send(200, "text/html", html);
    });

    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "application/json", "{\"status\":\"use ws\"}");
    });

    server.on("/raw", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!cameraReady) { request->send(503); return; }
        if (xSemaphoreTake(camLock, pdMS_TO_TICKS(500))) {
            esp32cam::asyncweb::handleStill(request);
            xSemaphoreGive(camLock);
        } else request->send(503);
    });

    server.on("/snapshot", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!cameraReady) { request->send(503); return; }
        if (xSemaphoreTake(camLock, pdMS_TO_TICKS(5000))) {
            if (useFlashAuto) {
                setFlash(flashIntensity);
                delay(500); // 500ms é o ideal para o sensor se ajustar totalmente à luz
            }

            // Descarta 5 frames. Isso "limpa" a fila do hardware e garante que 
            // a captura final ocorra após o sensor ter ajustado ganho e exposição.
            for(int i=0; i<5; i++) {
                camera_fb_t *fb_tmp = esp_camera_fb_get();
                if (fb_tmp) esp_camera_fb_return(fb_tmp);
                delay(10); // Pequena pausa entre descartes
            }
            
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                request->send(200, "image/jpeg", fb->buf, fb->len);
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
            });
            esp32cam::asyncweb::handleMjpeg(request);
        } else request->send(503);
    });

    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<!DOCTYPE html><html><head><title>Config</title><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif;background:#0a0a0c;color:#fff;padding:20px;display:flex;justify-content:center;}";
        html += ".card{background:#16161a;border-radius:20px;padding:25px;width:100%;max-width:400px;border:1px solid #333;}";
        html += "h3{margin:0 0 20px;color:#3d5afe;} .group{margin-bottom:15px;text-align:left;} label{display:block;font-size:12px;color:#888;margin-bottom:5px;}";
        html += "input[type=text], select{width:100%;padding:10px;background:#222;border:1px solid #444;color:#fff;border-radius:8px;box-sizing:border-box;}";
        html += "input[type=range]{width:100%;margin-top:5px;} .row{display:flex;gap:10px;} .row > div{flex:1;}";
        html += ".btn{background:#3d5afe;color:white;padding:12px;border-radius:10px;border:none;cursor:pointer;width:100%;font-weight:bold;margin-top:15px;}";
        html += ".btn-test{background:#666;} .modal{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.9);z-index:100;justify-content:center;align-items:center;flex-direction:column;}";
        html += "</style></head><body><div class='card'><h3>Configurações</h3><form action='/save-settings' method='POST'>";
        html += "<div class='group'><label>Nome mDNS</label><input type='text' name='mdns' value='" + String(mdnsName) + "'></div>";
        html += "<div class='group'><label>Resolução</label><select name='res'>";
        const char* resNames[] = {"QQVGA","QCIF","HQVGA","240x240","QVGA","CIF","HVGA","VGA","SVGA","XGA","HD","SXGA","UXGA"};
        for(int i=0; i<13; i++) {
            html += "<option value='" + String(i) + "'" + String(camResol==i?"selected":"") + ">" + String(resNames[i]) + "</option>";
        }
        html += "</select></div>";
        html += "<div class='row'><div class='group'><label>Qualidade (10-63)</label><input type='range' name='qual' min='10' max='63' value='" + String(camQuality) + "'></div>";
        html += "<div class='group'><label>Brilho</label><input type='range' name='brt' min='-2' max='2' value='" + String(camBrightness) + "'></div></div>";
        html += "<div class='row'><div class='group'><label>Contraste</label><input type='range' name='con' min='-2' max='2' value='" + String(camContrast) + "'></div>";
        html += "<div class='group'><label>Saturação</label><input type='range' name='sat' min='-2' max='2' value='" + String(camSaturation) + "'></div></div>";
        html += "<div class='row'><div class='group'><input type='checkbox' name='vf' id='vf' " + String(camVFlip?"checked":"") + "><label style='display:inline' for='vf'>V-Flip</label></div>";
        html += "<div class='group'><input type='checkbox' name='hm' id='hm' " + String(camHMirror?"checked":"") + "><label style='display:inline' for='hm'>H-Mirror</label></div></div>";
        html += "<div class='group'><input type='checkbox' name='fl' id='fl' " + String(useFlashAuto?"checked":"") + "><label style='display:inline' for='fl'>Flash Auto</label></div>";
        html += "<button type='submit' class='btn'>SALVAR E APLICAR</button></form>";
        html += "<button onclick=\"testPhoto()\" class='btn btn-test'>TESTAR FOTO</button>";
        html += "<a href='/' style='display:block;text-align:center;margin-top:15px;color:#888;text-decoration:none;font-size:13px;'>← VOLTAR</a>";
        html += "</div><div id='m' class='modal' onclick='this.style.display=\"none\"'><h3>Snapshot de Teste</h3><img id='mi' style='max-width:90%;border-radius:10px;'><p>Clique para fechar</p></div>";
        html += "<script>function testPhoto(){ document.getElementById('m').style.display='flex'; document.getElementById('mi').src='/snapshot?t='+Date.now(); }</script></body></html>";
        request->send(200, "text/html", html);
    });

    server.on("/save-settings", HTTP_POST, [](AsyncWebServerRequest *request){
        if (request->hasParam("mdns", true)) {
            String newName = request->getParam("mdns", true)->value();
            if (newName.length() > 0) prefs.putString("mdns", newName);
            
            camResol = request->getParam("res", true)->value().toInt();
            camQuality = request->getParam("qual", true)->value().toInt();
            camBrightness = request->getParam("brt", true)->value().toInt();
            camContrast = request->getParam("con", true)->value().toInt();
            camSaturation = request->getParam("sat", true)->value().toInt();
            camVFlip = request->hasParam("vf", true);
            camHMirror = request->hasParam("hm", true);
            useFlashAuto = request->hasParam("fl", true);

            prefs.putInt("cam_res", camResol);
            prefs.putInt("cam_qual", camQuality);
            prefs.putInt("cam_brt", camBrightness);
            prefs.putInt("cam_con", camContrast);
            prefs.putInt("cam_sat", camSaturation);
            prefs.putBool("cam_vf", camVFlip);
            prefs.putBool("cam_hm", camHMirror);
            prefs.putBool("flash_auto", useFlashAuto);

            applyCameraSettings();
            shouldRestart = (newName != String(mdnsName));
            request->redirect("/settings");
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

    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate > 2000) {
        lastUpdate = millis();
        int wifi_pct   = constrain(map(WiFi.RSSI(), -90, -40, 0, 100), 0, 100);
        uint32_t freeH = ESP.getFreeHeap();
        uint32_t totH  = ESP.getHeapSize();
        int ram_pct    = constrain((int)(((float)(totH - freeH) / (float)totH) * 100), 0, 100);
        float temp     = (temprature_sens_read() - 32) / 1.8;
        
        String json = "{\"wifi_pct\":" + String(wifi_pct)
                    + ",\"ram_pct\":"  + String(ram_pct)
                    + ",\"temp\":"     + String(temp, 1)
                    + ",\"uptime\":"   + String(millis() / 1000) + "}";
        ws.textAll(json);
    }

    ws.cleanupClients();

    static unsigned long lastWd = 0;
    if (millis() - lastWd > 30000) {
        lastWd = millis();
        if (streamActive) LOG("[WD] Stream ativo");
    }
}
