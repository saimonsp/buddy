# Buddy AI

Mascote no ESP8266 (OLED SSD1306 128x64 I2C) que mostra, rotacionando, os tokens gastos com IAs instaladas no PC: **Codex**, **Claude Code** e **OpenCode**.

```
┌────────────────────────────┐
│  ( mascote )   Codex       │
│   (^_^)/       627.1M      │
│               hoje 0       │
│            · ● · ·         │
└────────────────────────────┘
```

- Página inicial: **TOTAL** (soma de todas as IAs)
- Depois, uma página por CLI instalado (só aparece quem estiver instalado)
- Tokens de **hoje** aparecem junto do total
- **Ícone de WiFi** no canto quando os dados vêm pela rede; **ícone de USB** quando vêm pelo cabo (ou sem WiFi)

### Caretas do mascote

| Careta | Quando aparece |
|---|---|
| 😊 feliz (olhos arco) | página TOTAL / tarefa terminou / boot |
| 😎 descolado (olhos semicerrados) | página do **Codex** |
| ✨ estrela (olhos brilhando) | página do **Claude** |
| 😲 uau (olhos grandes) | página do **OpenCode** / alerta "precisa de você" |
| 😞 triste | agent parado, sem WiFi ou aguardando dados demais |
| 😟 preocupado | algum limite ≥ **80%** |
| 😱 pânico (+ gota de suor) | algum limite ≥ **90%** |

Regra: **status > identidade** — se houver aviso (limite/stale), a careta vira status; senão, cada página mantém sua personalidade. O pisca é animação, sempre.

### Barra de limite

Páginas com limite conhecido (hoje: **Codex**, via `rate_limits` dos logs) mostram
barra de progresso + % no rodapé. Claude/OpenCode: sem limite até configurar.

### Buzzer (alertas sonoros)

| Pino | Uso |
|---|---|
| **D5 (GPIO14)** | buzzer **+** (já ligado aqui) |
| **GND** | buzzer **−** |

> Pinos evitados: D1/D2 (OLED), D3/D4/D8 (arranque do ESP8266).

| Evento | Som |
|---|---|
| Limite ≥ 80% | 2 bipes de 880Hz |
| Limite ≥ 90% | 3 bipes rápidos |
| Limite = 100% | alarme alternado |
| Agente terminou tarefa | chime ascendente |
| Claude precisa de você (hook `Notification`) | 3 bipes duplos |
| Claude terminou de responder (hook `Stop`) | chime |

Alertas aparecem como **pop-up** na tela (~6s) com a careta de status.

## Hardware

| ESP8266MOD (NodeMCU) | SSD1306 I2C |
|---|---|
| 3V3 | VCC |
| GND | GND |
| **D1 (GPIO5)** | SDA |
| **D2 (GPIO4)** | SCL |

> Atenção: neste projeto o barramento é `Wire.begin(5, 4)` — SDA em D1 e SCL em D2
> (o inverso do padrão NodeMCU, é assim que o display está ligado aqui).
> Se você ligar do jeito padrão (SDA=D2, SCL=D1), troque para `Wire.begin(4, 5)` no firmware.

Endereço I2C: `0x3C`.

## Como funciona

1. **Firmware** (`firmware/buddy/buddy.ino`) roda no ESP: conecta no WiFi, desenha o mascote e rotaciona as páginas (~4.5s cada).
2. **Agente** (`agent/buddy_agent.py`) roda no PC: detecta os CLIs instalados, lê os tokens dos logs locais de cada um e sirve em `http://<ip-do-pc>:8000/data`.
3. O ESP **busca** esse endereço a cada 10s (é cliente HTTP, não abre servidor).

Para o IP do PC não ser problema quando o DHCP mudar, o agente também faz **broadcast UDP** na porta 8788 (`BUDDY <ip> <porta>`) a cada 5s — o ESP escuta e se reencontra sozinho, sem reflashear.

O agente também:
- lê o **rate limit** do Codex (`used_percent`/`resets_at`) e envia `pct`/`resetAt`
- detecta **atividade** dos CLIs (mtime dos logs) e dispara alerta "terminou" na transição ativo→parado
- expõe `POST /alert` usado pelos **hooks do Claude Code** (`Notification`/`Stop` em `~/.claude/settings.json`, backup `.bak-buddy`)

> Porta 8000 de propósito: o firewalld desta máquina está em zona `drop`
> e só permite 8000/8001/5000/8080/8443 — a 8787 ficava bloqueada.
> Credenciais de WiFi ficam em `firmware/buddy/config.h` (fora do git).

Onde o agente lê os tokens:

| CLI | Fonte |
|---|---|
| Codex | `~/.codex/sessions/**/*.jsonl` (eventos `token_count`) |
| Claude Code | `~/.claude/projects/**/*.jsonl` (campo `usage` das respostas) |
| OpenCode | `~/.local/share/opencode/opencode.db` (tabela `message`) |

A coleta é incremental (estado em `agent/.buddy_state.json`) — após a primeira varredura, atualiza em ~0.2s.

## Uso

### 1. Firmware (já feito neste PC, mas para referência)

```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 firmware/buddy
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp8266:esp8266:nodemcuv2 firmware/buddy
```

### 2. Agente

```bash
pip install -r agent/requirements.txt   # só pyserial (opcional, pro modo --serial)
python3 agent/buddy_agent.py            # servidor HTTP na porta 8000 + broadcast
python3 agent/buddy_agent.py --dry-run  # so imprime o payload
python3 agent/buddy_agent.py --serial   # em vez de so servir, empurra pelo cabo USB
```

Deixe o agente rodando enquanto quiser ver o display atualizado:

```bash
nohup python3 agent/buddy_agent.py > /tmp/buddy.log 2>&1 &
```

### Formato do payload (`GET /data`, JSON)

```json
{"cmd":"data","total":4734566502,"today":1040018,"tools":[{"id":"codex","name":"Codex","total":627178067,"today":0}, ...]}
```

No boot o ESP também manda `{"cmd":"hello"}` pela serial (debug). Enquanto o agente não responde, o display mostra `agent off?`.

## Ajustes finos

- Intervalo de atualização do display: `--interval 30` (padrão 15s)
- Frequência de busca do ESP: constante `POLL_MS` no firmware (padrão 10000ms)
- Tempo por página: constante `PAGE_MS` no firmware (padrão 4500ms)
- Endereço do OLED: `OLED_ADDR` no firmware (`0x3C` ou `0x3D`)
- WiFi/IP/porta do agent: `firmware/buddy/config.h` (broadcast UDP corrige o IP sozinho)
