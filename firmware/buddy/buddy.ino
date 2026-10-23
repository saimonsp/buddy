#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
extern "C" {
#include <user_interface.h>
}
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include "config.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

#define SERIAL_BAUD 115200
#define MAX_TOOLS 4
#define PAGE_MS 4500
#define LINE_MAX 1024
#define STALE_MS 90000
#define WIFI_TIMEOUT_MS 20000

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiClient wifiClient;

struct Tool {
  char id[16];
  char name[16];
  uint64_t total;
  uint64_t today;
  int8_t pct;       // % do limite, -1 = sem limite conhecido
  uint32_t resetAt; // unix ts do reset da janela
};

enum Transport : uint8_t { T_NONE, T_WIFI, T_USB };

Tool tools[MAX_TOOLS];
uint8_t toolCount = 0;
uint64_t grandTotal = 0;
uint64_t grandToday = 0;
bool hasData = false;
uint32_t lastDataMs = 0;
Transport lastTransport = T_NONE;

char lineBuf[LINE_MAX];
uint16_t lineIdx = 0;

uint32_t pageStartMs = 0;
uint8_t pageIndex = 0;
bool displayDirty = true;

uint32_t nextBlinkMs = 2000;
bool blinking = false;
uint32_t blinkEndMs = 0;

bool wifiConnected = false;
uint32_t nextPollMs = 0;

// IP do agent em runtime - o agent anuncia via broadcast UDP quando muda
char agentHost[16] = AGENT_HOST;
uint16_t agentPort = AGENT_PORT;
WiFiUDP udp;

// ----------------------------------------------------------------- alertas
// faces: 0 neutro, 1 feliz, 2 uau, 3 descolado, 4 estrela, 5 triste,
//        6 preocupado, 7 panico
bool popupActive = false;
uint32_t popupEndMs = 0;
uint8_t popupFace = 1;
char popupL1[26];
char popupL2[26];
uint32_t lastAlertTs = 0;

// sequenciador de bipes (pares freq,dur - freq 0 = silencio)
const uint16_t BP_LIMIT80[] = {880, 120, 0, 80, 880, 120};
const uint16_t BP_LIMIT90[] = {1046, 80, 0, 60, 1046, 80, 0, 60, 1046, 80};
const uint16_t BP_LIMIT100[] = {1318, 120, 987, 120, 1318, 120, 987, 120,
                                1318, 120, 987, 120, 1318, 300};
const uint16_t BP_DONE[] = {784, 110, 988, 110, 1318, 240};
const uint16_t BP_ATTENTION[] = {1046, 70, 0, 60, 1046, 70, 0, 160,
                                 1046, 70, 0, 60, 1046, 70, 0, 160, 1046, 70, 0, 60, 1046, 70};

const uint16_t* bzSeq = nullptr;
uint8_t bzLen = 0;
uint8_t bzIdx = 0;
uint32_t bzPhaseEnd = 0;

void startBeep(const uint16_t* seq, uint8_t pairs) {
  bzSeq = seq;
  bzLen = pairs * 2;
  bzIdx = 0;
  bzPhaseEnd = 0;
}

void tickBeep() {
  if (!bzSeq) return;
  if ((int32_t)(millis() - bzPhaseEnd) < 0) return;
  if (bzIdx >= bzLen) {
    noTone(BUZZER_PIN);
    bzSeq = nullptr;
    return;
  }
  uint16_t freq = bzSeq[bzIdx];
  uint16_t dur = bzSeq[bzIdx + 1];
  if (freq) {
    tone(BUZZER_PIN, freq);
  } else {
    noTone(BUZZER_PIN);
  }
  bzPhaseEnd = millis() + dur;
  bzIdx += 2;
}

String fmtTokens(uint64_t n) {
  char buf[16];
  if (n >= 1000000000ULL) {
    snprintf(buf, sizeof(buf), "%.2fB", (double)n / 1e9);
  } else if (n >= 1000000ULL) {
    snprintf(buf, sizeof(buf), "%.1fM", (double)n / 1e6);
  } else if (n >= 1000ULL) {
    snprintf(buf, sizeof(buf), "%.1fK", (double)n / 1e3);
  } else {
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)n);
  }
  return String(buf);
}

// ------------------------------------------------------------- icones

void drawWifiIcon(int16_t x0, int16_t y0) {
  int16_t cx = x0 + 6;
  int16_t cy = y0 + 10;
  display.fillCircle(cx, cy, 1, SSD1306_WHITE);
  for (uint8_t ri = 0; ri < 3; ri++) {
    int8_t r = 3 + ri * 3;
    for (int8_t a = -50; a <= 50; a += 5) {
      float rad = a * DEG_TO_RAD;
      int16_t px = cx + (int16_t)lroundf(r * sinf(rad));
      int16_t py = cy - (int16_t)lroundf(r * cosf(rad));
      display.drawPixel(px, py, SSD1306_WHITE);
    }
  }
}

void drawUsbIcon(int16_t x0, int16_t y0) {
  // tridente USB: conector central, haste e dois galhos com pontas
  display.fillRect(x0 + 4, y0 + 1, 3, 3, SSD1306_WHITE);
  display.drawFastVLine(x0 + 5, y0 + 4, 7, SSD1306_WHITE);
  display.drawLine(x0 + 5, y0 + 7, x0 + 2, y0 + 5, SSD1306_WHITE);
  display.fillRect(x0, y0 + 3, 3, 3, SSD1306_WHITE);
  display.drawLine(x0 + 5, y0 + 7, x0 + 8, y0 + 5, SSD1306_WHITE);
  display.fillRect(x0 + 8, y0 + 3, 3, 3, SSD1306_WHITE);
}

bool useWifiIcon() {
  if (!wifiConnected) return false;
  if (lastTransport == T_USB) return false;
  return true;
}

void drawTransportIcon() {
  int16_t x0 = SCREEN_WIDTH - 13;
  if (useWifiIcon()) {
    drawWifiIcon(x0, 1);
  } else {
    drawUsbIcon(x0, 1);
  }
}

// ------------------------------------------------------------- mascote

void drawMascot(int16_t x, int16_t y, uint8_t face, bool blink) {
  display.drawLine(x + 14, y + 4, x + 14, y + 8, SSD1306_WHITE);
  display.fillCircle(x + 14, y + 2, 2, SSD1306_WHITE);
  display.drawRoundRect(x + 2, y + 8, 26, 20, 7, SSD1306_WHITE);
  display.fillRect(x, y + 14, 2, 7, SSD1306_WHITE);
  display.fillRect(x + 28, y + 14, 2, 7, SSD1306_WHITE);

  int16_t eyeL = x + 9;
  int16_t eyeR = x + 21;
  int16_t eyeY = y + 15;

  if (blink) {
    display.drawFastHLine(eyeL - 2, eyeY, 5, SSD1306_WHITE);
    display.drawFastHLine(eyeR - 2, eyeY, 5, SSD1306_WHITE);
  } else {
    switch (face) {
      case 1:
        display.drawCircle(eyeL, eyeY + 2, 3, SSD1306_WHITE);
        display.drawCircle(eyeR, eyeY + 2, 3, SSD1306_WHITE);
        display.fillRect(eyeL - 4, eyeY + 2, 8, 3, SSD1306_BLACK);
        display.fillRect(eyeR - 4, eyeY + 2, 8, 3, SSD1306_BLACK);
        break;
      case 2:
        display.fillCircle(eyeL, eyeY, 3, SSD1306_WHITE);
        display.fillCircle(eyeR, eyeY, 3, SSD1306_WHITE);
        display.fillCircle(eyeL + 1, eyeY - 1, 1, SSD1306_BLACK);
        display.fillCircle(eyeR + 1, eyeY - 1, 1, SSD1306_BLACK);
        break;
      case 3:
        display.fillCircle(eyeL, eyeY, 3, SSD1306_WHITE);
        display.fillCircle(eyeR, eyeY, 3, SSD1306_WHITE);
        display.fillRect(eyeL - 4, eyeY - 4, 8, 3, SSD1306_BLACK);
        display.fillRect(eyeR - 4, eyeY - 4, 8, 3, SSD1306_BLACK);
        break;
      case 4:
        display.drawPixel(eyeL, eyeY - 2, SSD1306_WHITE);
        display.drawPixel(eyeL, eyeY + 2, SSD1306_WHITE);
        display.drawPixel(eyeL - 2, eyeY, SSD1306_WHITE);
        display.drawPixel(eyeL + 2, eyeY, SSD1306_WHITE);
        display.fillCircle(eyeL, eyeY, 1, SSD1306_WHITE);
        display.drawPixel(eyeR, eyeY - 2, SSD1306_WHITE);
        display.drawPixel(eyeR, eyeY + 2, SSD1306_WHITE);
        display.drawPixel(eyeR - 2, eyeY, SSD1306_WHITE);
        display.drawPixel(eyeR + 2, eyeY, SSD1306_WHITE);
        display.fillCircle(eyeR, eyeY, 1, SSD1306_WHITE);
        break;
      case 5: // triste - olhos semicerrados olhando pra baixo
        display.fillCircle(eyeL, eyeY, 3, SSD1306_WHITE);
        display.fillCircle(eyeR, eyeY, 3, SSD1306_WHITE);
        display.fillRect(eyeL - 4, eyeY - 4, 8, 3, SSD1306_BLACK);
        display.fillRect(eyeR - 4, eyeY - 4, 8, 3, SSD1306_BLACK);
        break;
      case 6: // preocupado - olhinhos + sobrancelhas inclinadas
        display.fillCircle(eyeL, eyeY, 2, SSD1306_WHITE);
        display.fillCircle(eyeR, eyeY, 2, SSD1306_WHITE);
        display.drawLine(x + 7, y + 10, x + 12, y + 12, SSD1306_WHITE);
        display.drawLine(x + 23, y + 10, x + 18, y + 12, SSD1306_WHITE);
        break;
      case 7: // panico - olhos arregalados + gota de suor
        display.fillCircle(eyeL, eyeY, 4, SSD1306_WHITE);
        display.fillCircle(eyeR, eyeY, 4, SSD1306_WHITE);
        display.fillCircle(eyeL, eyeY, 1, SSD1306_BLACK);
        display.fillCircle(eyeR, eyeY, 1, SSD1306_BLACK);
        display.drawPixel(x + 31, y + 11, SSD1306_WHITE);
        display.drawPixel(x + 31, y + 12, SSD1306_WHITE);
        display.drawPixel(x + 30, y + 13, SSD1306_WHITE);
        display.drawPixel(x + 32, y + 13, SSD1306_WHITE);
        break;
      default:
        display.fillCircle(eyeL, eyeY, 2, SSD1306_WHITE);
        display.fillCircle(eyeR, eyeY, 2, SSD1306_WHITE);
        break;
    }
  }

  int16_t mouthY = y + 23;
  switch (face) {
    case 1:
      display.drawCircle(x + 15, mouthY - 2, 5, SSD1306_WHITE);
      display.drawFastHLine(x + 10, mouthY - 2, 11, SSD1306_BLACK);
      break;
    case 2:
      display.fillCircle(x + 15, mouthY, 3, SSD1306_WHITE);
      break;
    case 3:
      display.drawFastHLine(x + 11, mouthY, 8, SSD1306_WHITE);
      display.drawPixel(x + 20, mouthY - 1, SSD1306_WHITE);
      break;
    case 4:
      display.drawCircle(x + 15, mouthY - 2, 5, SSD1306_WHITE);
      display.drawFastHLine(x + 10, mouthY - 2, 11, SSD1306_BLACK);
      display.drawFastHLine(x + 11, mouthY, 9, SSD1306_WHITE);
      break;
    case 5: // boca virada pra baixo
      display.drawCircle(x + 15, mouthY, 5, SSD1306_WHITE);
      display.fillRect(x + 10, mouthY, 11, 6, SSD1306_BLACK);
      break;
    case 6: // boca tremida
      display.drawPixel(x + 11, mouthY, SSD1306_WHITE);
      display.drawPixel(x + 12, mouthY - 1, SSD1306_WHITE);
      display.drawPixel(x + 13, mouthY, SSD1306_WHITE);
      display.drawPixel(x + 14, mouthY + 1, SSD1306_WHITE);
      display.drawPixel(x + 15, mouthY, SSD1306_WHITE);
      display.drawPixel(x + 16, mouthY - 1, SSD1306_WHITE);
      display.drawPixel(x + 17, mouthY, SSD1306_WHITE);
      break;
    case 7: // boca escancarada
      display.fillRect(x + 12, mouthY - 2, 7, 4, SSD1306_WHITE);
      break;
    default:
      display.drawFastHLine(x + 11, mouthY, 9, SSD1306_WHITE);
      break;
  }
}

void drawDots(int16_t cx, int16_t y, uint8_t total, uint8_t active) {
  if (total == 0) return;
  int16_t spacing = 8;
  int16_t w = (total - 1) * spacing;
  int16_t x0 = cx - w / 2;
  for (uint8_t i = 0; i < total; i++) {
    if (i == active) {
      display.fillCircle(x0 + i * spacing, y, 2, SSD1306_WHITE);
    } else {
      display.drawCircle(x0 + i * spacing, y, 2, SSD1306_WHITE);
    }
  }
}

// ------------------------------------------------------------- telas

void drawWaiting() {
  display.clearDisplay();
  bool sadNow = !wifiConnected || (hasData && (millis() - lastDataMs) >= STALE_MS);
  drawMascot(6, 14, sadNow ? 5 : 1, blinking);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(44, 12);
  display.print("Buddy AI");
  if (wifiConnected) {
    display.setCursor(44, 30);
    display.print("aguardando");
    display.setCursor(44, 42);
    display.print("agent...");
  } else {
    display.setCursor(44, 30);
    display.print("sem WiFi");
    display.setCursor(44, 42);
    display.print("agent no cabo?");
  }
  drawTransportIcon();
  drawDots(SCREEN_WIDTH / 2, 61, 1, 0);
  display.display();
}

void drawToolPage(uint8_t idx) {
  display.clearDisplay();
  const char* name;
  uint64_t total, today;
  uint8_t face;
  int8_t pct = -1;

  if (idx == 0) {
    name = "TOTAL";
    total = grandTotal;
    today = grandToday;
    face = 1;
  } else {
    Tool& t = tools[idx - 1];
    name = t.name;
    total = t.total;
    today = t.today;
    pct = t.pct;
    if (strcmp(t.id, "codex") == 0) face = 3;
    else if (strcmp(t.id, "claude") == 0) face = 4;
    else if (strcmp(t.id, "opencode") == 0) face = 2;
    else face = 0;
  }

  // status tem prioridade sobre a identidade da pagina
  bool stale = (millis() - lastDataMs) >= STALE_MS;
  if (stale) {
    face = 5;
  } else if (pct >= 90) {
    face = 7;
  } else if (pct >= 80) {
    face = 6;
  }

  drawMascot(4, 10, face, blinking);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(40, 8);
  display.print(name);

  display.setTextSize(2);
  display.setCursor(40, 22);
  display.print(fmtTokens(total));

  display.setTextSize(1);
  display.setCursor(40, 44);
  display.print("hoje ");
  display.print(fmtTokens(today));

  if (stale) {
    display.setCursor(40, 54);
    display.print("agent off?");
    drawDots(SCREEN_WIDTH / 2, 60, toolCount + 1, pageIndex);
  } else if (pct >= 0) {
    int16_t bx = 40, by = 56, bw = 60, bh = 6;
    display.drawRect(bx, by, bw, bh, SSD1306_WHITE);
    int16_t fw = (int16_t)((int32_t)(bw - 2) * pct / 100);
    if (pct > 0 && fw < 1) fw = 1;
    if (fw > bw - 2) fw = bw - 2;
    if (fw > 0) display.fillRect(bx + 1, by + 1, fw, bh - 2, SSD1306_WHITE);
    display.setCursor(104, 56);
    display.print(pct);
    display.print('%');
  } else {
    drawDots(SCREEN_WIDTH / 2, 60, toolCount + 1, pageIndex);
  }

  drawTransportIcon();
  display.display();
}

// ------------------------------------------------------------- alertas UI

const char* toolLabel(const char* id) {
  if (strcmp(id, "codex") == 0) return "Codex";
  if (strcmp(id, "claude") == 0) return "Claude";
  if (strcmp(id, "opencode") == 0) return "OpenCode";
  return "?";
}

void showAlert(JsonObject a) {
  uint32_t ts = a["ts"] | (uint32_t)0;
  if (ts <= lastAlertTs) return;
  lastAlertTs = ts;

  const char* k = a["k"] | "";
  const char* tool = toolLabel(a["tool"] | "");
  popupActive = true;
  popupEndMs = millis() + 6000;

  if (strcmp(k, "limit") == 0) {
    int band = a["band"] | 0;
    int p = a["pct"] | 0;
    popupFace = band >= 90 ? 7 : 6;
    snprintf(popupL1, sizeof(popupL1), "%s %d%%!", tool, p);
    snprintf(popupL2, sizeof(popupL2), "limite %d%%", band);
    if (band >= 100) {
      startBeep(BP_LIMIT100, 7);
    } else if (band >= 90) {
      startBeep(BP_LIMIT90, 3);
    } else {
      startBeep(BP_LIMIT80, 2);
    }
  } else if (strcmp(k, "done") == 0) {
    popupFace = 1;
    snprintf(popupL1, sizeof(popupL1), "%s terminou!", tool);
    snprintf(popupL2, sizeof(popupL2), "tarefa pronta :)");
    startBeep(BP_DONE, 3);
  } else {
    popupFace = 2;
    snprintf(popupL1, sizeof(popupL1), "%s precisa", tool);
    snprintf(popupL2, sizeof(popupL2), "de voce!");
    startBeep(BP_ATTENTION, 6);
  }
  displayDirty = true;
  Serial.printf("alerta: %s | %s\n", k, popupL1);
}

void drawPopup() {
  display.clearDisplay();
  drawMascot(49, 6, popupFace, blinking);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int16_t w1 = (int16_t)strlen(popupL1) * 6;
  display.setCursor((SCREEN_WIDTH - w1) / 2, 42);
  display.print(popupL1);
  int16_t w2 = (int16_t)strlen(popupL2) * 6;
  display.setCursor((SCREEN_WIDTH - w2) / 2, 54);
  display.print(popupL2);
  display.display();
}

void redraw() {
  if (popupActive) {
    drawPopup();
  } else if (!hasData) {
    drawWaiting();
  } else {
    drawToolPage(pageIndex);
  }
}

// ------------------------------------------------------------- dados

void applyPayload(const char* json, Transport origin) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  const char* cmd = doc["cmd"] | "";
  if (strcmp(cmd, "data") != 0) return;

  toolCount = 0;
  grandTotal = doc["total"] | 0ULL;
  grandToday = doc["today"] | 0ULL;

  JsonArray arr = doc["tools"].as<JsonArray>();
  for (JsonObject o : arr) {
    if (toolCount >= MAX_TOOLS) break;
    Tool& t = tools[toolCount];
    strlcpy(t.id, o["id"] | "", sizeof(t.id));
    strlcpy(t.name, o["name"] | "", sizeof(t.name));
    t.total = o["total"] | 0ULL;
    t.today = o["today"] | 0ULL;
    int p = o["pct"] | -1;
    if (p > 100) p = 100;
    if (p < -1) p = -1;
    t.pct = (int8_t)p;
    t.resetAt = o["resetAt"] | (uint32_t)0;
    toolCount++;
  }

  JsonArray alerts = doc["alerts"].as<JsonArray>();
  for (JsonObject a : alerts) {
    showAlert(a);
  }

  lastTransport = origin;
  hasData = true;
  lastDataMs = millis();
  if (pageIndex > toolCount) pageIndex = 0;
  pageStartMs = millis();
  displayDirty = true;
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      lineBuf[lineIdx] = '\0';
      if (lineIdx > 0) applyPayload(lineBuf, T_USB);
      lineIdx = 0;
    } else if (c != '\r') {
      if (lineIdx < LINE_MAX - 1) {
        lineBuf[lineIdx++] = c;
      } else {
        lineIdx = 0;
      }
    }
  }
}

// ------------------------------------------------------------- wifi

void startWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  // reduz potencia de TX (13 dBm): menos pico de corrente no barramento USB,
  // evita derrubar adaptadores WiFi vizinhos
  system_phy_set_max_tpw(26);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected) {
    Serial.print(F("WiFi ok, IP: "));
    Serial.println(WiFi.localIP());
    udp.begin(8788);
  } else {
    Serial.println(F("WiFi falhou - modo serial ativo"));
  }
}

void listenUdp() {
  if (!wifiConnected) return;
  int n = udp.parsePacket();
  if (!n) return;
  char buf[64];
  int len = udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';
  char host[16] = {0};
  int port = 0;
  if (sscanf(buf, "BUDDY %15s %d", host, &port) == 2 && port > 0) {
    if (strcmp(host, agentHost) != 0 || (uint16_t)port != agentPort) {
      strncpy(agentHost, host, sizeof(agentHost) - 1);
      agentHost[sizeof(agentHost) - 1] = '\0';
      agentPort = (uint16_t)port;
      nextPollMs = millis();
      Serial.printf("agent descoberto: %s:%u\n", agentHost, agentPort);
    }
  }
}

void pollAgent() {
  if (!wifiConnected) return;
  String url = String("http://") + agentHost + ":" + String(agentPort) + "/data";
  HTTPClient http;
  if (!http.begin(wifiClient, url)) return;
  http.setTimeout(3000);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    applyPayload(body.c_str(), T_WIFI);
    Serial.println(F("agent ok"));
  } else if (code > 0) {
    Serial.printf("agent HTTP %d\n", code);
  } else {
    Serial.printf("agent inacessivel: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// ------------------------------------------------------------- main

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(100);

  // cabeadinho ligado invertido: SDA=D1(GPIO5), SCL=D2(GPIO4)
  Wire.begin(5, 4);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("{\"cmd\":\"oled_fail\"}"));
    for (;;) delay(1000);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(16, 22);
  display.print("Buddy AI");
  display.setCursor(16, 38);
  display.print("conectando WiFi");
  display.display();

  startWifi();

  Serial.println(F("{\"cmd\":\"hello\"}"));
  pageStartMs = millis();
  nextBlinkMs = millis() + 2500;
  nextPollMs = millis();
}

void loop() {
  handleSerial();
  listenUdp();

  bool status = WiFi.status() == WL_CONNECTED;
  if (status != wifiConnected) {
    wifiConnected = status;
    displayDirty = true;
    if (wifiConnected) {
      nextPollMs = millis();
      udp.begin(8788);
    }
    Serial.println(wifiConnected ? F("WiFi reconectado") : F("WiFi caiu"));
  }

  uint32_t now = millis();

  tickBeep();

  if (popupActive && (int32_t)(now - popupEndMs) >= 0) {
    popupActive = false;
    displayDirty = true;
  }

  if (wifiConnected && (int32_t)(now - nextPollMs) >= 0) {
    pollAgent();
    nextPollMs = now + POLL_MS;
  }

  if (now >= nextBlinkMs && !blinking) {
    blinking = true;
    blinkEndMs = now + 140;
    displayDirty = true;
  }
  if (blinking && now >= blinkEndMs) {
    blinking = false;
    nextBlinkMs = now + 2000 + random(1500);
    displayDirty = true;
  }

  if (hasData && now - pageStartMs >= PAGE_MS) {
    pageIndex = (pageIndex + 1) % (toolCount + 1);
    pageStartMs = now;
    displayDirty = true;
  }

  if (displayDirty) {
    redraw();
    displayDirty = false;
  }

  yield();
}
