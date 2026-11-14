# Validação física pendente — RemoteCam 0.2b

A implementação passou por compilação e testes locais, mas requer os seguintes ensaios após o upload pelo operador:

- Vídeo em `/stream` direto, com monitor da impressora e painel simultâneos; fechar o painel não interrompe o outro cliente.
- JPEG direto em `/snapshot`, inclusive sem stream previamente aberto.
- Conexões liberadas após desconectar; o driver da câmera continua inicializado.
- Sinal Wi-Fi, RAM utilizada/livre e temperatura interna aproximada coerentes com a execução.
- LED, alimentação e aquecimento durante uso prolongado.
- Galeria, ZIP, renomeação e exclusão de pastas, incluindo erros de cartão.
- Nova pasta por sessão de timelapse; gravação independente do navegador; finalização coordenada e ausência de retomada automática após reiniciar.
- FTP anônimo de leitura com os clientes usados na rede, incluindo retomada e concorrência com vídeo/timelapse.
- Preferências dos serviços restauradas após reiniciar; desligamento cooperativo com transferências e manutenção em andamento.
- Wi-Fi, painel, `/stream` e `/snapshot` acessíveis independentemente das chaves opcionais.
- OTA válido/inválido e recuperação de falhas, com procedimento de recuperação serial disponível.
- Formatação e interrupção de energia **somente em cartão de teste autorizado**, nunca com dados únicos.

Não há promessa de FPS ou prontidão para produção sem essas medições. Consulte a documentação principal para limites e precauções.
