#pragma once

// Credenciais WiFi 2.4GHz (copie para config.h e ajuste)
#define WIFI_SSID "SEU_WIFI"
#define WIFI_PASS "SUA_SENHA"

// Agent no PC (o ESP busca esse endereco via HTTP)
#define AGENT_HOST "192.168.1.112"
#define AGENT_PORT 8000

// Intervalo de busca dos dados (ms)
#define POLL_MS 5000

// Buzzer piezo: + em D5 (GPIO14), - em GND
// (D5 e D6 sao os pinos livres seguros: D1/D2 com OLED, D3/D4/D8 sao de boot)
#define BUZZER_PIN 14
