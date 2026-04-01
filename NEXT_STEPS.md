# Próximos Passos - ESP32-RemoteCam Pro 🚀

Documento de planejamento para futuras melhorias e expansões do projeto.

---

## 🛠️ Interface de Configuração (Dashboard 2.0)
Atualmente a página é funcional, mas pode ser expandida para um "Centro de Controle":

- [ ] **Configurações de Câmera em Tempo Real**:
    - Seleção de Resolução (desde QQVGA até UXGA).
    - Ajuste de Qualidade JPEG (10-63).
    - Controles de Imagem (V-Flip, H-Mirror, Brilho, Contraste, Saturação).
- [ ] **Modo de Teste**:
    - Botão "Testar Foto" dentro da tela de configurações que abre em um modal/iframe para validar os ajustes sem sair da página.
- [ ] **Configuração de Rede**:
    - Opção para trocar o IP Estático / Gateway.
    - Login de proteção para a página de configurações.

## 🌡️ Sensores e Telemetria
Expandir a capacidade de monitoramento do dispositivo:

- [ ] **Sensor Externo (DS18B20 / DHT22)**:
    - Leitura de temperatura/umidade ambiente real (superior ao sensor interno que sofre calor da CPU).
- [ ] **Monitoramento de Bateria**:
    - Se usado com LiPo, adicionar divisor de tensão para mostrar percentual de carga.
- [ ] **Alertas**:
    - Notificação via Webhook (Discord/Slack/Telegram) se a temperatura ultrapassar um limite.

## 🎥 Melhorias no Stream/Snapshot
- [ ] **Gravação em SD Card**:
    - Salvar snapshots periodicamente ou por detecção de movimento no cartão SD (se disponível).
- [ ] **Stream Multi-Cliente**:
    - Otimizar o semáforo para permitir que um cliente veja o monitoramento enquanto outros puxam telemetria (status).
- [ ] **Filtro de Noite**:
    - Ajuste automático de ganho/exposição quando a luz estiver baixa.

## 📦 Infraestrutura e Código
- [ ] **Atualização Over-the-Air (OTA)**:
    - Permitir atualizar o firmware via navegador (porta 80/update) sem cabos.
- [ ] **Websockets**:
    - Trocar o polling `/status` (JS `setInterval`) por Websockets para telemetria em "tempo real" real.
- [ ] **Documentação da API**:
    - Documentar as rotas (`/raw`, `/snapshot`, `/status`) para permitir integração com Home Assistant ou scripts externos.

---
*Sugestão do Antigravity (IA):* Como a ESP32-CAM é sensível a energia, uma melhoria física recomendada é adicionar um capacitor de 100uF a 1000uF entre os pinos 5V e GND para estabilizar os picos de consumo da câmera e do Wi-Fi, reduzindo Brownouts.
