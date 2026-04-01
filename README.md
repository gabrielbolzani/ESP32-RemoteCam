# ESP32-CAM Master Pro Ultimate 🚀📸

Firmware profissional de alto desempenho para ESP32-CAM, focado em monitoramento de alta qualidade, estabilidade de recursos e interface de usuário premium.

## ✨ Recursos Principais

- **📸 Snapshot Ultra HD**: Capturas em resolução **UXGA (1600x1200)** com controle fino de qualidade JPEG.
- **🎥 Stream Assíncrono**: Streaming MJPEG otimizado para baixa latência em resolução VGA.
- **⚡ Rota /raw**: Endpoint de captura ultra-rápida sem troca de resolução, ideal para integração com Python, Home Assistant ou Node-RED.
- **🎨 Dashboard Premium**: Painel de controle em Dark Mode com telemetria em tempo real:
  - Percentual de sinal WiFi (RSSI).
  - Uso de memória SRAM.
  - Temperatura interna do dispositivo.
  - Uptime formatado (h/m/s).
- **💡 Controle de Flash via PWM**: Slider de intensidade (0-100%) para o Flash LED, permitindo iluminação suave ou total.
- **🛠️ Configuração Persistente**:
  - Nome de rede customizado (**mDNS**) salvo na memória (ex: `http://nome.local`).
  - Portal de configuração WiFi (WiFiManager) integrado.
  - Opção de Flash Automático persistente.
- **🛡️ Segurança de Hardware**: Sistema de **Mutex (Semáforo)** para evitar crashes em acessos simultâneos ao sensor da câmera.

## 🚀 Como Usar

### Pré-requisitos
- [PlatformIO IDE](https://platformio.org/) (VS Code).
- Hardware: ESP32-CAM (Modelo AI-Thinker).

### Instalação
1. Clone este repositório.
2. Abra a pasta no VS Code com PlatformIO.
3. Conecte sua ESP32-CAM (Lembre-se de aterrar o GPIO 0 para modo de gravação).
4. Utilize o script `controlar.py` ou os comandos do PlatformIO:
   ```bash
   pio run -t upload
   ```
5. Abra o Monitor Serial para pegar o IP inicial ou acesse via `http://webcam.local` após a configuração do WiFi.

## 🔗 Endpoints Disponíveis

| Rota | Descrição |
| --- | --- |
| `/` | Dashboard principal (Interface Web) |
| `/stream` | Streaming de vídeo MJPEG |
| `/snapshot` | Foto em Resolução Máxima (Ultra HD) |
| `/raw` | Foto instantânea (Resolução atual do sensor) |
| `/status` | JSON com telemetria (WiFi, RAM, Temp, Uptime) |
| `/settings` | Painel de configurações (mDNS, Flash, Brilho) |
| `/led-on` | Liga o Flash manualmente (Intensidade salva) |
| `/led-off` | Desliga o Flash |

## 🛠️ Tecnologias Utilizadas
- **Framework**: Arduino ESP32.
- **Web**: ESPAsyncWebServer (Assíncrono).
- **Câmera**: Biblioteca `esp32cam` & `esp_camera`.
- **Configuração**: WiFiManager & Preferences.

---
Desenvolvido para ser o firmware definitivo para projetos de monitoramento e IoT com ESP32-CAM.
