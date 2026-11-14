# ESP32-RemoteCam

Firmware **0.2b** para **ESP32-CAM AI-Thinker com sensor OV2640 e PSRAM**, voltado ao monitoramento de impressoras 3D.

A versão vem do arquivo [VERSION](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/VERSION). O build usa esse valor no firmware, no painel e na galeria; não é necessário alterar números em vários arquivos.

> **Estado da entrega:** implementação compilada e verificada com testes locais. Desempenho, estabilidade prolongada, concorrência na ESP32, formatação e recuperação após falta de energia ainda exigem ensaio na placa. Os testes locais não fazem upload nem apagam o cartão da câmera.

## Recursos

- **Vídeo MJPEG e snapshot sob demanda**, nos endereços originais e sem login no painel.
- **Timelapse autônomo no SD**, com intervalo, resolução e qualidade JPEG, criando uma pasta exclusiva por sessão.
- **Galeria web**, com miniaturas, ampliação e download das fotos.
- **Gerenciador de arquivos:** criar pastas, renomear, excluir conteúdo e baixar pastas como ZIP.
- **FTP anônimo de leitura**, para acessar os arquivos em clientes como FileZilla ou WinSCP.
- **Controle do LED**, com intensidade PWM, botão liga/desliga e iluminação automática.
- **Telemetria:** sinal Wi-Fi em percentual estimado e RSSI, temperatura aproximada do chip, RAM utilizada/livre, PSRAM e indicadores da câmera.
- **Chaves de serviços persistentes** para FTP, acesso web ao SD, OTA e atualização automática do painel.

O SNR não é coletado nem exibido. O projeto não utiliza serviços de nuvem para vídeo ou armazenamento.

## Hardware e ferramentas

- ESP32-CAM AI-Thinker, OV2640 e PSRAM funcionando.
- Alimentação adequada à placa, especialmente ao usar câmera, Wi-Fi, SD e LED simultaneamente.
- Cartão com FAT suportado pelo firmware; a formatação oferecida pela interface é FAT32.
- Python e [PlatformIO](https://platformio.org/) para compilar/enviar.
- Node.js compatível com Playwright, somente para os testes de interface.

A plataforma de build e as bibliotecas estão fixadas na [configuração do PlatformIO](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/platformio.ini): Espressif32 6.9.0, Arduino ESP32 2.0.17, WiFiManager 2.0.17 e ArduinoJson 6.21.5. O mapa de pinos é específico da AI-Thinker.

## Início rápido

Execute no diretório clonado do projeto:

```sh
git clone https://github.com/gabrielbolzani/ESP32-RemoteCam.git
cd ESP32-RemoteCam
python controlar.py devices
python controlar.py compile
```

O [utilitário Python](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/controlar.py) também possui menu interativo:

```sh
python controlar.py
```

Para enviar à placa e acompanhar o serial, escolha a porta correta do seu computador:

```sh
python controlar.py upload --port COM5
python controlar.py monitor --port COM5
```

**Compilar não faz upload.** No menu, a opção de compilar e enviar só prossegue se a compilação terminar com sucesso. O utilitário procura PlatformIO no PATH, na instalação padrão do Windows ou no caminho definido por `PLATFORMIO_EXE`.

Se a placa não tiver programação automática, siga o procedimento de boot/upload do seu adaptador. Não use a porta de outro equipamento.

Na primeira conexão, o WiFiManager pode abrir o ponto de acesso **ESP32-RemoteCam-Config**. Depois de conectar à rede, o serial informa o IP. O nome mDNS padrão é **webcam.local**, alterável no painel.

## URLs e integração com a impressora

Painel, câmera e API HTTP usam **a mesma porta 80**:

| Endereço | Comportamento |
|---|---|
| `/` | Painel principal, sem login |
| `/stream` | MJPEG iniciado pela conexão do cliente |
| `/snapshot` | Uma foto nova, retornada diretamente como `image/jpeg` com tamanho informado |
| `/raw` | Captura direta na configuração atual |
| `/gallery` | Galeria e navegação pelo SD, quando o acesso web ao SD está habilitado |
| `/api/status` | Telemetria e estado das operações |
| `/api/services` | Consulta e configuração dos serviços opcionais |

**`/stream` e `/snapshot` não redirecionam para outras portas e não dependem de abrir o painel ou apertar um botão antes.** São permitidos até dois clientes de vídeo, para usar o monitor da impressora e o navegador. Fechar o vídeo no painel fecha apenas aquela conexão.

O servidor utiliza um worker por conexão, limitado a seis conexões HTTP ativas, para que um stream longo não bloqueie os controles. A tarefa de aquisição fica ociosa quando não há consumidor nem captura de timelapse agendada; o driver da câmera permanece inicializado. Não há suspensão automática da câmera nesta revisão.

HTTP/1.0 e HTTP/1.1 são aceitos. Requisições de entrada com corpo precisam informar `Content-Length`; `Transfer-Encoding: chunked` na entrada não é suportado. O painel usa consultas HTTP, sem depender de WebSocket.

## Serviços: ligar e desligar

As preferências são aplicadas em execução e salvas no NVS. A seção **Serviços** indica estados como Ligado, Em uso, Desativando e Desligado.

| Chave | Efeito |
|---|---|
| **FTP** | Fecha a escuta da porta 21, remove somente o anúncio mDNS de FTP e encerra sessões ociosas. Uma transferência já iniciada pode terminar antes da sessão sair. |
| **Acesso web ao SD** | Bloqueia novos acessos à galeria, arquivos, ZIP e gerenciamento. Pedidos/jobs já aceitos terminam. Não desmonta o cartão nem interrompe FTP ou timelapse. |
| **Atualização OTA** | Controla novos envios de firmware. Desligar a chave não cancela uma atualização em andamento. |
| **Atualização automática do painel** | Pausa as consultas periódicas ou usa intervalos de 2, 5 ou 10 segundos. **Atualizar agora** e consultas necessárias aos comandos continuam disponíveis. |

**Wi-Fi, servidor web, `/stream`, `/snapshot` e `/raw` não têm chave de desligamento.** O nome mDNS e o anúncio HTTP não são encerrados ao desligar FTP.

No primeiro uso, sem preferências anteriores, os quatro serviços começam habilitados e o painel atualiza a cada dois segundos, preservando o comportamento existente. Depois valem as escolhas salvas. Uma falha ao salvar não altera a configuração em uso.

Ao desligar um serviço ocupado, o firmware bloqueia novas operações e aguarda o término das já aceitas. O painel pode continuar consultando apenas o progresso dessa alteração, mesmo com a atualização dos demais indicadores pausada. Em abas pausadas, use **Atualizar agora** para receber mudanças feitas por outro cliente.

Desligar serviços não remove código da flash. O benefício vem de liberar recursos dinâmicos e evitar trabalho/transferências concorrentes; não há promessa de ganho de FPS sem medição na placa.

## Câmera, LED e telemetria

- Captura JPEG nativa, XCLK de 20 MHz, dois buffers do driver e seleção do frame mais recente.
- Um único proprietário coordena a câmera. Buffers compartilhados têm ciclo de vida controlado, sem devolução ao driver enquanto ainda estão em uso.
- Wi-Fi permanece habilitado e sem modem sleep para priorizar latência, com maior consumo de energia.
- Resolução e compressão são controles diferentes. Na escala JPEG nativa usada pelo painel, **número menor significa mais qualidade e normalmente arquivo maior**.
- A temperatura exibida é uma leitura interna aproximada do ESP32, não da impressora/ambiente nem uma medição calibrada. Valores inválidos aparecem como N/D.
- O cartão de memória do painel mostra **percentual de RAM interna/heap utilizada**, com a quantidade livre abaixo. PSRAM é mostrada separadamente.
- O LED inicia desligado. O botão preserva a última intensidade; o slider limita a frequência dos comandos e evita gravar preferências a cada movimento.

Intensidade PWM não é potência óptica calibrada. Verifique alimentação e aquecimento antes de manter o LED forte por longos períodos.

## Timelapse

1. Clique em **Iniciar timelapse**.
2. Escolha intervalo em segundos, resolução, qualidade JPEG e iluminação.
3. Selecione o comportamento do vídeo e confirme.
4. Use **Finalizar** para encerrar o agendamento e concluir a escrita pendente.

Cada início cria **uma nova pasta exclusiva dentro de `/timelapse` no cartão**. Todas as fotos daquela sessão ficam nela até finalizar, desligar a placa ou ocorrer erro de armazenamento. Fechar o navegador não interrompe a gravação.

- **Modo fluido:** fotos e vídeo compartilham o perfil escolhido.
- **Modo foto prioritária:** o vídeo pausa brevemente para usar outro perfil na foto; o intervalo mínimo é de três segundos.
- A primeira captura ocorre após a preparação. O relógio monotônico evita deriva acumulada e não depende de NTP.
- Se houver atraso, o firmware registra slots perdidos; não executa uma rajada de compensação.
- São gravados JPEGs numerados, índice CSV e manifestos. Arquivos temporários não são apresentados como fotos concluídas.
- Após reiniciar, a sessão anterior não é retomada. Quando possível, seu manifesto é marcado como interrompido. O próximo início cria outra pasta.

A saída é uma sequência de fotos, **não um MP4**.

## Cartão SD, galeria e pastas

O SDMMC opera em **um bit**, evitando disputar GPIO4 com o LED. A interface mostra capacidade/espaço e oferece:

- Navegação paginada, miniaturas carregadas gradualmente e ampliação das fotos.
- Download individual e **ZIP de uma pasta**.
- Criação e renomeação de pastas, sem sobrescrever um destino existente.
- Exclusão de pasta e conteúdo após digitar **EXCLUIR**, com progresso em segundo plano.
- Formatação FAT32 após confirmação explícita **FORMATAR**.

### Integridade e limites

- Não existe formatação automática ao montar nem exclusão automática de fotos antigas.
- A raiz do cartão, `/timelapse` e sessões ativas são protegidas. Mutações conflitantes com gravação, download, OTA ou manutenção são recusadas.
- Excluir uma pasta verifica a árvore antes de iniciar. Se houver falha no meio, informa a exclusão parcial; não promete desfazer o que já foi removido.
- Fechar a página não cancela uma exclusão já confirmada.
- Caminhos têm limite de 180 bytes e operações recursivas suportam até 16 subníveis.
- ZIP64 usa método STORE: mantém arquivos, subpastas vazias e nomes UTF-8, sem recomprimir JPEGs. Duas passagens permitem transmissão com memória limitada, sem criar o ZIP no cartão; CRCs/metadados são comparados antes de finalizar.
- Nunca remova o cartão durante escrita ou formatação. FAT não é imune à falta de energia, mesmo usando arquivos temporários e manifesto anterior.

## FTP

FTP é um serviço adicional de **leitura**, compartilhando o cartão já montado e suas proteções de acesso.

| Configuração do cliente | Valor |
|---|---|
| Host | Mesmo IP/nome mDNS da câmera; também aparece no painel |
| Protocolo | **FTP simples**, não SFTP nem FTPS |
| Porta de controle | **21** |
| Acesso | **Anônimo** (`anonymous` ou `ftp`), sem senha secreta |
| Modo de dados | **Passivo**, PASV/EPSV |
| Portas passivas | **50000–50001** |
| Concorrência recomendada | Uma transferência simultânea |

Há até duas conexões de controle, para navegação + transferência. É possível listar, navegar, consultar tamanho/data, baixar arquivos e retomar downloads binários por REST. Um cliente pode baixar uma pasta recursivamente, arquivo por arquivo.

FTP **não aceita upload, exclusão, renomeação ou formatação**. Essas alterações, quando disponíveis, permanecem na web. O ZIP é gerado pela interface web; FTP apresenta os arquivos efetivamente armazenados.

Durante timelapse, JPEGs concluídos da sessão ativa podem ser baixados, mas seus arquivos em escrita/metadados são recusados. Arquivos estáveis de outras pastas continuam acessíveis. Em caso de disputa com outro download, o servidor informa ocupado. FTP e HTTP não montam o cartão duas vezes.

Sessões ociosas expiram. Conexões de dados passivas precisam vir do mesmo IP da conexão de controle, e modo ativo PORT/EPRT é recusado. Limites de tamanho/seek de arquivos individuais continuam sujeitos ao driver FAT/VFS. Downloads compartilham a banda Wi-Fi com o vídeo.

## Segurança e OTA

**O painel não tem login e o FTP é anônimo. Use somente em rede local confiável/VPN; não exponha a câmera diretamente à internet.** HTTP e FTP simples não criptografam os dados. Verificação de origem de comandos não substitui autenticação.

OTA precisa estar habilitado em **Serviços** e não pode começar com gravação/download/manutenção conflitantes. O envio de firmware interrompe o vídeo e reinicia a placa quando concluído. Desligar a chave OTA não aborta um envio já admitido. Não desligue a alimentação durante a atualização.

## API principal

Consultas e mutações usam JSON, exceto o envio OTA binário. Não é necessário token de login.

| Método e rota | Finalidade |
|---|---|
| `GET /api/status` | Estado completo; aliases `/status` e `/api/timelapse/status` |
| `GET /api/health` | Diagnóstico compacto |
| `GET /api/services` | Preferências e estado dos serviços |
| `POST /api/services` | Campos opcionais `ftp`, `web_sd`, `ota`, `panel_refresh` e `refresh_seconds` |
| `POST /api/led` | `enabled`, `intensity_pct` e, opcionalmente, `auto_flash` |
| `POST /api/camera` | Configuração da imagem |
| `POST /api/timelapse/start` | Intervalo, perfil e modo da sessão |
| `POST /api/timelapse/stop` | Finalização coordenada |
| `GET /api/sd/list` | Listagem paginada por `path` e `offset` |
| `GET /api/sd/image` | JPEG para visualização |
| `GET /api/sd/download` | Download de arquivo por `path` |
| `GET /api/sd/archive` | Download ZIP por `path` |
| `POST /api/sd/directory` | Criar pasta |
| `POST /api/sd/rename` | `path` e `new_name` |
| `DELETE /api/sd/item` | `path`, `confirmation` e `recursive` quando necessário |
| `POST /api/sd/mount` | Detectar/remontar o cartão |
| `POST /api/sd/format/prepare` | Obter desafio temporário de confirmação |
| `POST /api/sd/format` | Confirmar formatação |
| `POST /api/system` | Nome mDNS e reinício |
| `POST /api/update` | Firmware binário com `Content-Length` |

Operações longas retornam aceitação e identificador; o resultado deve ser acompanhado no estado, por exemplo `sd.job`. Aceitação não significa conclusão. Com o acesso web ao SD desligado, `/gallery` e `/api/sd/*` ficam indisponíveis; `/api/status` e `/api/services` permanecem acessíveis.

## Estrutura do projeto

| Área | Arquivos |
|---|---|
| Firmware e transporte HTTP | [Principal](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/src/main.cpp), [transporte](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/src/http_transport.h) |
| Serviços | [Política](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/src/service_policy.h), [interface](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/web/services.js) |
| FTP e ZIP | [Protocolo FTP](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/src/ftp_protocol.h), [integração](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/src/ftp_service.h), [encoder ZIP](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/src/zip_store.h) |
| Interface web | [Painel](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/web/index.html), [galeria](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/web/gallery.html), [ações de pasta](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/web/folder_actions.js) |
| Build | [Gerador](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/scripts/embed_ui.py), [versão](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/VERSION), [configuração](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/platformio.ini) |
| Verificação | [Testes](https://github.com/gabrielbolzani/ESP32-RemoteCam/tree/main/tests), [ensaios pendentes na placa](https://github.com/gabrielbolzani/ESP32-RemoteCam/blob/main/NEXT_STEPS.md) |

Os headers da interface e da versão são gerados automaticamente antes da compilação. Não os edite manualmente. Artefatos do PlatformIO, dependências, logs, resultados de teste e configurações locais do editor não são versionados.

## Testes

### Python e código C portátil

```sh
python -m unittest discover -s tests -p "test_*.py" -v
```

Os testes C usam o mesmo encoder ZIP, protocolo FTP e política de serviços do firmware. Em Windows, precisam do Tiny C Compiler e dos headers WinAPI. Os pacotes oficiais estão na [página de releases do TinyCC](https://download-mirror.savannah.gnu.org/releases/tinycc/): compilador Windows 64 bits 0.9.27 e pacote WinAPI correspondente.

É possível extrair esses pacotes para a pasta local de verificação ou definir `TCC_EXE` e `TCC_WINAPI`. Na ausência dessas ferramentas, os testes nativos são marcados como **ignorados**, não como validados.

FTP é exercitado em loopback com uma pasta temporária do PC, nunca no cartão da câmera. O ZIP é conferido por um leitor independente, incluindo nomes UTF-8, pastas vazias e contagens ZIP64. O teste de serviços verifica configurações salvas/recarregadas e desligamento cooperativo.

### Interface em navegador

```sh
npm ci
npx playwright install chromium
npm run test:ui
```

Playwright é somente uma dependência de desenvolvimento. `PLAYWRIGHT_MODULE` permite apontar para uma instalação externa, se necessário. Os testes da interface utilizam um backend simulado e não comandam a placa.

### Verificação desta entrega

- Compilação para o ambiente `esp32cam` concluída.
- 30 testes locais Python/C aprovados, sem skips no ambiente usado.
- Duas suítes de interface aprovadas, incluindo rotas originais, galeria, operações de pasta, memória e chaves de serviços.
- Recursos web gerados conferidos contra as fontes e o binário.

Esses resultados **não substituem ensaios físicos** de FPS/latência, estabilidade prolongada, diferentes cartões, interrupção de energia, OTA e integração com o monitor da impressora. A formatação deve ser validada apenas em cartão explicitamente autorizado para apagar.
