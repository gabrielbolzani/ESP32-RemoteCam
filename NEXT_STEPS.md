# Próximos Passos - ESP32-RemoteCam Pro 🚀

Documento de planejamento para futuras melhorias e expansões do projeto.
---

## 🛠️ Interface de Configuração (Dashboard 2.0)
Atualmente a página é funcional, mas pode ser expandida para um "Centro de Controle":

- [x] **Configurações de Câmera em Tempo Real**:
    - Seleção de Resolução (desde QQVGA até UXGA).
    - Ajuste de Qualidade JPEG (10-63).
    - Controles de Imagem (V-Flip, H-Mirror, Brilho, Contraste, Saturação).
- [x] **Modo de Teste**:
    - Botão "Testar Foto" dentro da tela de configurações que abre em um modal/iframe para validar os ajustes sem sair da página.
- [ ] **Configuração de Rede**:
    - Opção para trocar o IP Estático / Gateway.
    - Login de proteção para a página de configurações.

## 📦 Infraestrutura e Código
- [x] **Atualização Over-the-Air (OTA)**:
    - Permitir atualizar o firmware via navegador (porta 80/update) sem cabos.
- [x] **Websockets**:
    - Trocar o polling `/status` (JS `setInterval`) por Websockets para telemetria em "tempo real" real.
- [x] **Documentação da API**:
    - Documentar as rotas (`/raw`, `/snapshot`, `/status`) para permitir integração com Home Assistant ou scripts externos.

---
*Sugestão do Antigravity (IA):* Como a ESP32-CAM é sensível a energia, uma melhoria física recomendada é adicionar um capacitor de 100uF a 1000uF entre os pinos 5V e GND para estabilizar os picos de consumo da câmera e do Wi-Fi, reduzindo Brownouts.
