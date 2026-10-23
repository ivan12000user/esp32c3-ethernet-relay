#include <Arduino.h>
#include <ETH.h>
#include <SPI.h>
#include <WiFi.h>
#include <Network.h>
#include <NetworkClient.h>
#include <NetworkServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <Update.h>
#include "PingWatchdog.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ESP32-C3 SuperMini / W5500 / 4 relays, Arduino-ESP32 3.x
// v1.4.3-dev | W5500 always enabled. WiFi: OFF / BACKUP / PARALLEL / AP
// Limit transmitter power to 8.5 dBm (STA and AP). Does not change Ethernet.
static const wifi_power_t WIFI_TX_POWER = WIFI_POWER_8_5dBm;
#define ETH_CS 7
#define ETH_IRQ 21
#define ETH_RST -1
#define ETH_SCK 4
#define ETH_MISO 5
#define ETH_MOSI 6

// Standard ESP32-C3 SuperMini blue on-board LED: GPIO8, active LOW.
// GPIO8 is a boot strapping pin; leave external circuitry disconnected.
const uint8_t WIFI_LED_PIN = 8;
bool wifiLedIsOn = false;
const uint8_t relayPins[4] = {0, 1, 3, 10};
bool relayState[4] = {false, false, false, false};
// No Web password. Restrict access to HTTP:80 and Modbus:502 at network level.

// 0 OFF, 1 Ethernet failover STA, 2 ETH+STA simultaneously, 3 ETH+local AP
uint8_t wifiMode = 0;
String wifiSsid, wifiPassword;
String apSsid = "ESP32C3-Relay";
String apPassword = "12357895"; // AP default; saved /wifi settings may override.

bool mqttEnabled = false;
String mqttHost, mqttUser, mqttPassword, mqttBase = "ethernet-relay";
uint16_t mqttPort = 1883;

WebServer web(80);
NetworkServer mbServer(502);
NetworkClient mbClient;
NetworkClient mqttNet;
PubSubClient mqtt(mqttNet);
Preferences prefs;

volatile bool ethReady = false;
volatile bool staReady = false;
volatile bool apReady = false;
bool staStarted = false;
bool servicesStarted = false;
uint8_t currentRoute = 0; // 0 none, 1 Ethernet, 2 WiFi STA
uint32_t startAt = 0, wifiLastTry = 0, mqttLastTry = 0;
uint32_t rebootAt = 0;
bool otaOK = false;
uint8_t mbFrame[260];
uint16_t mbPos = 0, mbTarget = 0;
uint32_t mbLastByte = 0;

WdMachine wd[4];
String wdTarget[4] = {"", "", "", ""};
String wdName[4] = {"Relay 1", "Relay 2", "Relay 3", "Relay 4"};

struct PingEvent { uint8_t type; }; // success=1 timeout=2 end=3
QueueHandle_t pingQueue = nullptr;
esp_ping_handle_t pingHandle = nullptr;
int activePingChannel = -1;
uint8_t pingOutcome = 0;
uint32_t pingStartedAt = 0;
bool pingStopRequested = false;

const char* wdStateName(WdState s) {
  switch (s) {
    case WD_DISABLED: return "DISABLED";
    case WD_GRACE: return "GRACE";
    case WD_HEALTHY: return "HEALTHY";
    case WD_SUSPECT: return "SUSPECT";
    case WD_POWER_OFF: return "POWER_OFF";
    case WD_LOCKOUT: return "LOCKOUT";
    case WD_PAUSED: return "PAUSED";
    default: return "UNKNOWN";
  }
}


String htmlEscape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;");
  s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}



void loadSettings() {
  prefs.begin("relay-mqtt", true);
  mqttEnabled = prefs.getBool("enabled", false);
  mqttHost = prefs.getString("host", "");
  mqttPort = prefs.getUShort("port", 1883);
  mqttUser = prefs.getString("user", "");
  mqttPassword = prefs.getString("pass", "");
  mqttBase = prefs.getString("topic", "ethernet-relay");
  prefs.end();
  if (!mqttPort) mqttPort = 1883;
  if (mqttBase.isEmpty()) mqttBase = "ethernet-relay";

  prefs.begin("relay-wifi", true);
  wifiMode = prefs.getUChar("mode", 0);
  wifiSsid = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("pass", "");
  apSsid = prefs.getString("ap_ssid", "ESP32C3-Relay");
  apPassword = prefs.getString("ap_pass", "12357895");
  prefs.end();

  // One-time migration: use the newly requested AP password, including when
  // an old firmware stored another default in NVS. Later /wifi changes persist.
  prefs.begin("relay-wifi", false);
  if (!prefs.getBool("ap_v142", false)) {
    apPassword = "12357895";
    prefs.putString("ap_pass", apPassword);
    prefs.putBool("ap_v142", true);
    Serial.println("AP password migrated to v1.4.2 setting");
  }
  prefs.end();

  if (wifiMode > 3) wifiMode = 0;
  if (apSsid.isEmpty()) apSsid = "ESP32C3-Relay";
  if (apPassword.length() < 8) apPassword = "12357895";
}

String topicSet(int i) { return mqttBase + "/relay" + String(i + 1) + "/set"; }
String topicState(int i) { return mqttBase + "/relay" + String(i + 1) + "/state"; }
String topicStatus() { return mqttBase + "/status"; }

void setRelay(int ch, bool on) {
  if (ch < 0 || ch > 3 || relayState[ch] == on) return;
  digitalWrite(relayPins[ch], on ? HIGH : LOW);
  relayState[ch] = on;
  Serial.printf("Relay %d -> %s\n", ch + 1, on ? "ON" : "OFF");
  if (mqtt.connected()) {
    String t = topicState(ch);
    mqtt.publish(t.c_str(), on ? "ON" : "OFF", true);
  }
}


bool commandRelay(int ch, bool on) {
  if (ch < 0 || ch > 3 || wd[ch].active()) return false;
  if (wd[ch].config.mode == WD_PULSE && on) {
    wd[ch].startPulse(millis());
    setRelay(ch, true);
    return true;
  }
  if (wd[ch].config.mode == WD_PULSE) wd[ch].cancelPulse();
  setRelay(ch, on);
  return true;
}

void watchdogEvent(int ch, const char* event) {
  Serial.printf("Watchdog ch%d: %s, state=%s, target=%s\n",
    ch+1, event, wdStateName(wd[ch].state), wdTarget[ch].c_str());
  if (mqtt.connected()) {
    String topic = mqttBase + "/relay" + String(ch+1) + "/watchdog/event";
    mqtt.publish(topic.c_str(), event, false);
  }
}

void applyWdAction(int ch, WdAction action) {
  if (action == WD_SET_ON) {
    setRelay(ch, true);
    watchdogEvent(ch, "power_on");
  } else if (action == WD_SET_OFF) {
    setRelay(ch, false);
    watchdogEvent(ch, "power_cycle");
  }
}

void loadWatchdogSettings() {
  prefs.begin("relay-wd", true);
  for (int i = 0; i < 4; ++i) {
    String k = String(i);
    WdConfig c;
    c.mode = prefs.getUChar(("m"+k).c_str(), 0);
    if (c.mode > WD_PING) c.mode = WD_MANUAL;
    c.armed = prefs.getBool(("arm"+k).c_str(), false);
    c.intervalSec = constrain((int)prefs.getUShort(("int"+k).c_str(), 10), 5, 3600);
    c.failLimit = constrain((int)prefs.getUChar(("fail"+k).c_str(), 3), 1, 20);
    c.offSec = constrain((int)prefs.getUShort(("off"+k).c_str(), 10), 1, 300);
    c.graceSec = constrain((int)prefs.getUShort(("gr"+k).c_str(), 120), 10, 1800);
    c.maxCycles = constrain((int)prefs.getUChar(("cap"+k).c_str(), 3), 1, 10);
    c.pulseMs = constrain(prefs.getUInt(("pulse"+k).c_str(), 500), (uint32_t)100, (uint32_t)60000);
    wdTarget[i] = prefs.getString(("ip"+k).c_str(), "");
    wdName[i] = prefs.getString(("name"+k).c_str(), "Relay "+String(i+1));
    if (wdName[i].isEmpty()) wdName[i] = "Relay "+String(i+1);
    IPAddress addr;
    if (!addr.fromString(wdTarget[i]) || addr[0] == 0 || addr[0] >= 224)
      c.armed = false;
    if (c.mode != WD_PING) c.armed = false;
    applyWdAction(i, wd[i].configure(c, millis()));
  }
  prefs.end();
}

void saveWatchdogSettings() {
  prefs.begin("relay-wd", false);
  for (int i = 0; i < 4; ++i) {
    String k = String(i);
    const WdConfig &c = wd[i].config;
    prefs.putUChar(("m"+k).c_str(), c.mode);
    prefs.putBool(("arm"+k).c_str(), c.armed);
    prefs.putUShort(("int"+k).c_str(), c.intervalSec);
    prefs.putUChar(("fail"+k).c_str(), c.failLimit);
    prefs.putUShort(("off"+k).c_str(), c.offSec);
    prefs.putUShort(("gr"+k).c_str(), c.graceSec);
    prefs.putUChar(("cap"+k).c_str(), c.maxCycles);
    prefs.putUInt(("pulse"+k).c_str(), c.pulseMs);
    prefs.putString(("ip"+k).c_str(), wdTarget[i]);
    prefs.putString(("name"+k).c_str(), wdName[i]);
  }
  prefs.end();
}

static void pingSuccess(esp_ping_handle_t, void*) {
  if (pingQueue) { PingEvent e{1}; xQueueSend(pingQueue, &e, 0); }
}
static void pingTimeout(esp_ping_handle_t, void*) {
  if (pingQueue) { PingEvent e{2}; xQueueSend(pingQueue, &e, 0); }
}
static void pingEnd(esp_ping_handle_t, void*) {
  if (pingQueue) { PingEvent e{3}; xQueueSend(pingQueue, &e, 0); }
}

bool beginPing(int ch) {
  if (pingHandle || activePingChannel >= 0 || !pingQueue) return false;
  IPAddress ip;
  if (!ip.fromString(wdTarget[ch])) return false;
  ip_addr_t destination;
  IP_ADDR4(&destination, ip[0], ip[1], ip[2], ip[3]);
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr = destination;
  cfg.count = 1;
  cfg.interval_ms = 1000;
  cfg.timeout_ms = 1000;
  cfg.data_size = 16;
  esp_ping_callbacks_t cbs = {};
  cbs.on_ping_success = pingSuccess;
  cbs.on_ping_timeout = pingTimeout;
  cbs.on_ping_end = pingEnd;
  if (esp_ping_new_session(&cfg, &cbs, &pingHandle) != ESP_OK) return false;
  activePingChannel = ch;
  pingStartedAt = millis();
  pingStopRequested = false;
  pingOutcome = 0;
  if (esp_ping_start(pingHandle) != ESP_OK) {
    esp_ping_delete_session(pingHandle);
    pingHandle = nullptr;
    activePingChannel = -1;
    return false;
  }
  return true;
}

void serviceWatchdogs() {
  uint32_t now = millis();
  PingEvent evt;
  if (pingQueue) while (xQueueReceive(pingQueue, &evt, 0) == pdTRUE) {
    if (evt.type == 1) pingOutcome = 1;
    if (evt.type == 2) pingOutcome = 2;
    if (evt.type == 3 && pingHandle) {
      int ch = activePingChannel;
      esp_ping_delete_session(pingHandle);
      pingHandle = nullptr;
      activePingChannel = -1;
      pingStopRequested = false;
      if (ch >= 0 && ch < 4) {
        if (!(ethReady || staReady)) wd[ch].abortPing(now);
        else {
          WdState before = wd[ch].state;
          WdAction a = wd[ch].reportPing(pingOutcome == 1, now);
          applyWdAction(ch, a);
          if (wd[ch].state == WD_LOCKOUT && before != WD_LOCKOUT)
            watchdogEvent(ch, "lockout");
          else if (wd[ch].state == WD_HEALTHY && before != WD_HEALTHY)
            watchdogEvent(ch, "recovered");
        }
      }
    }
  }
  if (pingHandle && !pingStopRequested &&
      (uint32_t)(now - pingStartedAt) > 4500) {
    pingStopRequested = true;
    esp_ping_stop(pingHandle);
  }

  // Suspend target checks without Ethernet or WiFi STA.
  // A power-off pulse always finishes and restores power.
  bool networkReady = ethReady || staReady;
  for (int i = 0; i < 4; ++i) {
    WdState before = wd[i].state;
    WdAction a = wd[i].tick(now, networkReady);
    if (a == WD_SEND_PING) {
      if (pingHandle) wd[i].pingPending = false;
      else if (!beginPing(i)) wd[i].abortPing(now);
    } else applyWdAction(i, a);
    if (wd[i].state == WD_PAUSED && before != WD_PAUSED)
      watchdogEvent(i, "network_paused");
  }
}

void onMqttMessage(char *topic, byte *data, unsigned int n) {
  if (n > 32) return;
  String msg;
  for (unsigned int i = 0; i < n; ++i) msg += char(data[i]);
  msg.trim(); msg.toUpperCase();
  for (int ch = 0; ch < 4; ++ch) {
    if (String(topic) != topicSet(ch)) continue;
    if (msg == "ON" || msg == "1") commandRelay(ch, true);
    else if (msg == "OFF" || msg == "0") commandRelay(ch, false);
    else if (msg == "TOGGLE") commandRelay(ch, !relayState[ch]);
    break;
  }
}

void connectMqtt() {
  if (!mqttEnabled || mqttHost.isEmpty() || currentRoute == 0 || mqtt.connected()) return;
  mqtt.setServer(mqttHost.c_str(), mqttPort);
  String id = "relay-c3-" + ETH.macAddress(); id.replace(":", "");
  String status = topicStatus();
  const char *user = mqttUser.isEmpty() ? nullptr : mqttUser.c_str();
  const char *pass = mqttUser.isEmpty() ? nullptr : mqttPassword.c_str();
  bool ok = mqtt.connect(id.c_str(), user, pass, status.c_str(), 0, true, "offline");
  if (!ok) { Serial.printf("MQTT connection failed: %d\n", mqtt.state()); return; }
  mqtt.publish(status.c_str(), "online", true);
  String ipTopic = mqttBase + "/ip";
  String ip = currentRoute == 1 ? ETH.localIP().toString() : WiFi.localIP().toString();
  mqtt.publish(ipTopic.c_str(), ip.c_str(), true);
  for (int i = 0; i < 4; i++) {
    String t = topicSet(i); mqtt.subscribe(t.c_str());
    t = topicState(i); mqtt.publish(t.c_str(), relayState[i] ? "ON" : "OFF", true);
  }
  Serial.println("MQTT connected");
}

void serviceMqtt() {
  if (!mqttEnabled || currentRoute == 0) return;
  if (mqtt.connected()) { mqtt.loop(); return; }
  if ((uint32_t)(millis() - mqttLastTry) >= 10000) {
    mqttLastTry = millis(); connectMqtt();
  }
}

// Non-blocking Wi-Fi status LED, independent of W5500, MQTT and relays.
// STA OK: solid; connecting/lost: 250 ms ON / 250 ms OFF.
// AP: double flash every 2s; Wi-Fi OFF/standby backup: LED OFF.
void serviceWifiLed() {
  const uint32_t now = millis();
  bool ledOn = false;

  if (wifiMode == 3) {
    if (apReady) {
      const uint32_t phase = now % 2000UL;
      ledOn = (phase < 100UL) || (phase >= 250UL && phase < 350UL);
    } else {
      ledOn = ((now / 250UL) % 2UL) == 0;
    }
  } else if (wifiMode == 2 || (wifiMode == 1 && !ethReady)) {
    if (staReady) ledOn = true;
    else ledOn = ((now / 250UL) % 2UL) == 0;
  }

  if (ledOn != wifiLedIsOn) {
    wifiLedIsOn = ledOn;
    digitalWrite(WIFI_LED_PIN, ledOn ? LOW : HIGH);
  }
}

void applyWifiTxPower(const char* role) {
  // Arduino-ESP32 requires the STA/AP interface to have been started first.
  const bool ok = WiFi.setTxPower(WIFI_TX_POWER);
  const float actualDbm = (float)(int)WiFi.getTxPower() / 4.0f;
  Serial.printf("WiFi %s TX limit requested 8.5 dBm: %s; readback %.1f dBm\n",
                role, ok ? "OK" : "FAILED", actualDbm);
}

void startSta() {
  if (staStarted || wifiSsid.isEmpty()) return;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  applyWifiTxPower("STA");
  staStarted = true;
  wifiLastTry = millis();
  Serial.printf("WiFi STA starting: %s\n", wifiSsid.c_str());
}

void stopSta() {
  if (!staStarted) return;
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  staStarted = false; staReady = false;
  Serial.println("WiFi STA stopped: Ethernet restored");
}

void serviceWifi() {
  if (wifiMode == 0 || wifiMode == 3) return;
  if (wifiMode == 1) {
    // Backup: give Ethernet 8s to acquire DHCP on startup.
    if (ethReady) { if (staStarted) stopSta(); return; }
    if ((uint32_t)(millis() - startAt) < 8000) return;
  }
  if (!staStarted) startSta();
  // Reconnect without blocking Web/Modbus loop.
  if (staStarted && !staReady &&
      (uint32_t)(millis() - wifiLastTry) >= 15000) {
    wifiLastTry = millis();
    WiFi.reconnect();
  }
}

void chooseRoute() {
  uint8_t desired = ethReady ? 1 : (staReady ? 2 : 0);
  if (desired == currentRoute) return;
  if (mqtt.connected()) mqtt.disconnect();
  mqttNet.stop();
  if (mbClient) mbClient.stop();
  mbPos = 0; mbTarget = 0;
  if (desired == 1) Network.setDefaultInterface(ETH);
  if (desired == 2) Network.setDefaultInterface(WiFi.STA);
  currentRoute = desired;
  mqttLastTry = millis() - 10000UL;
  Serial.printf("Default outgoing interface: %s\n",
       desired == 1 ? "Ethernet" : desired == 2 ? "WiFi" : "None");
}

void onNetworkEvent(arduino_event_id_t e) {
  switch (e) {
    case ARDUINO_EVENT_ETH_START: ETH.setHostname("esp32c3-relay"); break;
    case ARDUINO_EVENT_ETH_CONNECTED: Serial.println("ETH link UP"); break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      ethReady = true; Serial.print("ETH IP: "); Serial.println(ETH.localIP()); break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_LOST_IP:
      ethReady = false; Serial.println("ETH disconnected"); break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      staReady = true; Serial.print("WiFi IP: "); Serial.println(WiFi.localIP()); break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      staReady = false; break;
    case ARDUINO_EVENT_WIFI_AP_START:
      apReady = true; Serial.print("AP IP: "); Serial.println(WiFi.softAPIP()); break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      apReady = false; break;
    default: break;
  }
}

void startWifiMode() {
  WiFi.persistent(false);
  if (wifiMode == 2) startSta();
  else if (wifiMode == 3) {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    bool ok = WiFi.softAP(apSsid.c_str(), apPassword.c_str());
    if (ok) applyWifiTxPower("AP");
    Serial.printf("WiFi AP: %s, SSID: %s, IP: %s\n", ok ? "OK" : "FAILED",
                  apSsid.c_str(), WiFi.softAPIP().toString().c_str());
  }
  else WiFi.mode(WIFI_OFF);
}

uint16_t mbWord(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
uint16_t readInputReg(uint16_t a) {
  uint32_t uptime = millis() / 1000UL;
  switch (a) {
    case 0: return uptime & 0xFFFF;
    case 1: return uptime >> 16;
    case 2: return ethReady ? 1 : 0;
    case 3: return mqtt.connected() ? 1 : 0;
    case 4: return staReady ? 1 : 0;
    case 5: return apReady ? 1 : 0;
    case 6: return wifiMode;
    case 7: return staReady ? (uint16_t)(int16_t)WiFi.RSSI() : 0;
  }
  if (a >= 8 && a < 32) {
    int ch = (a - 8) / 6;
    int item = (a - 8) % 6;
    switch (item) {
      case 0: return wd[ch].config.mode;
      case 1: return wd[ch].state;
      case 2: return wd[ch].failures;
      case 3: return (uint16_t)(wd[ch].cyclesTotal & 0xFFFF);
      case 4: return wd[ch].active() ? 1 : 0;
      case 5: return wd[ch].lastPingOk ? 1 : 0;
    }
  }
  return 0;
}

void mbReply(uint8_t fc, const uint8_t *body, uint16_t n) {
  uint8_t rsp[260];
  memcpy(rsp, mbFrame, 4);
  uint16_t len = n + 2; rsp[4] = len >> 8; rsp[5] = len & 255;
  rsp[6] = mbFrame[6]; rsp[7] = fc;
  if (n) memcpy(rsp + 8, body, n);
  mbClient.write(rsp, n + 8);
}
void mbException(uint8_t fc, uint8_t code) { mbReply(fc | 0x80, &code, 1); }

void processModbus() {
  if (mbPos < 8) return;
  if (mbFrame[2] || mbFrame[3]) { mbClient.stop(); return; }
  uint8_t fc = mbFrame[7];
  if (fc == 1 || fc == 2) {
    if (mbPos != 12) { mbException(fc, 3); return; }
    uint16_t a = mbWord(mbFrame+8), n = mbWord(mbFrame+10);
    if (!n || n > 4) { mbException(fc, 3); return; }
    if (a >= 4 || a + n > 4) { mbException(fc, 2); return; }
    uint8_t bits = 0;
    for (int i = 0; i < n; i++) if (relayState[a+i]) bits |= (1 << i);
    uint8_t body[] = {1, bits}; mbReply(fc, body, sizeof(body)); return;
  }
  if (fc == 5) {
    if (mbPos != 12) { mbException(fc, 3); return; }
    uint16_t a = mbWord(mbFrame+8), v = mbWord(mbFrame+10);
    if (a >= 4) { mbException(fc, 2); return; }
    if (v != 0xFF00 && v != 0) { mbException(fc, 3); return; }
    if (!commandRelay(a, v == 0xFF00)) { mbException(fc, 6); return; }
    mbReply(fc, mbFrame+8, 4); return;
  }
  if (fc == 15) {
    if (mbPos < 14) { mbException(fc, 3); return; }
    uint16_t a = mbWord(mbFrame+8), n = mbWord(mbFrame+10);
    uint8_t count = mbFrame[12];
    if (!n || n > 4) { mbException(fc, 3); return; }
    if (a >= 4 || a + n > 4) { mbException(fc, 2); return; }
    if (count != (n+7)/8 || mbPos != 13+count) { mbException(fc, 3); return; }
    for (int i = 0; i < n; i++)
      if (wd[a+i].active()) { mbException(fc, 6); return; }
    for (int i = 0; i < n; i++)
      commandRelay(a+i, (mbFrame[13+i/8] >> (i%8)) & 1);
    mbReply(fc, mbFrame+8, 4); return;
  }
  if (fc == 4) {
    if (mbPos != 12) { mbException(fc, 3); return; }
    uint16_t a = mbWord(mbFrame+8), n = mbWord(mbFrame+10);
    if (!n || n > 32) { mbException(fc, 3); return; }
    if (a >= 32 || (uint32_t)a + n > 32) { mbException(fc, 2); return; }
    uint8_t body[65]; body[0] = n*2;
    for (int i=0;i<n;i++) { uint16_t v=readInputReg(a+i); body[1+2*i]=v>>8; body[2+2*i]=v&255; }
    mbReply(fc, body, 1+n*2); return;
  }
  mbException(fc, 1);
}

void serviceModbus() {
  if (mbClient && !mbClient.connected()) { mbClient.stop(); mbPos=mbTarget=0; }
  if (!mbClient) {
    NetworkClient incoming = mbServer.accept();
    if (incoming) { mbClient=incoming; mbPos=mbTarget=0; }
  }
  if (!mbClient) return;
  int maxBytes = 128;
  while (mbClient.available() && maxBytes-- > 0) {
    int c = mbClient.read(); if (c < 0) break;
    if (mbPos >= sizeof(mbFrame)) { mbClient.stop(); mbPos=mbTarget=0; return; }
    mbFrame[mbPos++] = c; mbLastByte = millis();
    if (mbPos == 7) {
      uint16_t len = mbWord(mbFrame+4);
      if (len < 2 || len > 254) { mbClient.stop(); mbPos=mbTarget=0; return; }
      mbTarget = len + 6;
    }
    if (mbTarget && mbPos == mbTarget) {
      processModbus(); mbPos=mbTarget=0;
      if (!mbClient.connected()) return;
    }
  }
  if (mbPos && (uint32_t)(millis()-mbLastByte) > 1000) {
    mbClient.stop(); mbPos=mbTarget=0;
  }
}

const char INDEX_HTML[] PROGMEM =
  "<!doctype html>\n"
  "<html lang=\"ru\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
  "<title>ESP32-C3 Ethernet Relay</title>\n"
  "<style>\n"
  "*{box-sizing:border-box}body{font-family:Arial,sans-serif;background:#101827;color:#f8fafc;max-width:640px;margin:16px auto;padding:16px}h2{text-align:center;margin:8px 0 14px}.net{color:#94a3b8;font-size:13px;text-align:center;line-height:1.7;white-space:pre-line;margin-bottom:16px}.card{background:#243247;border:1px solid #334155;border-radius:13px;padding:17px;margin:12px 0}.head{display:flex;gap:12px;align-items:center;font-size:19px}.state{margin-left:auto;font-weight:700}.lamp{display:inline-block;background:#ef4444;box-shadow:0 0 10px #ef4444;width:18px;height:18px;border-radius:50%}.on .lamp{background:#22c55e;box-shadow:0 0 12px #22c55e}.buttons{display:flex;gap:12px;margin-top:15px}button{flex:1;border:0;border-radius:9px;padding:13px;color:white;cursor:pointer;font-weight:700;font-size:16px}.bon{background:#059669}.boff{background:#dc2626}button:active{opacity:.7}.links{text-align:center;margin-top:25px;line-height:2.3}.links a{color:#93c5fd;margin:0 9px;text-decoration:none}.version{color:#94a3b8;font-size:12px;text-align:center;margin-top:6px}\n"
  "</style></head><body>\n"
  "<h2>ESP32-C3 Ethernet Relay</h2>\n"
  "<div class=\"net\" id=\"net\">Соединение...</div>\n"
  "<div id=\"relays\"></div>\n"
  "<div class=\"links\"><a href=\"/wifi\">Настройки Wi-Fi</a><a href=\"/mqtt\">Настройки MQTT</a><a href=\"/watchdog\">Watchdog</a><a href=\"/update\">Обновление .bin</a></div>\n"
  "<div class=\"version\">Firmware v1.4.3-dev · Wi-Fi TX 8.5 dBm · Modbus TCP :502</div>\n"
  "<script>\n"
  "const area=document.getElementById('relays');\n"
  "for(let i=0;i<4;i++){\n"
  " const card=document.createElement('div'); card.className='card off'; card.id='r'+i;\n"
  " card.innerHTML='<div class=\"head\"><span class=\"lamp\"></span><strong>Реле '+(i+1)+'</strong><span class=\"state\" id=\"s'+i+'\">OFF</span></div><div class=\"buttons\"><button class=\"bon\" onclick=\"setRelay('+i+',1)\">ON</button><button class=\"boff\" onclick=\"setRelay('+i+',0)\">OFF</button></div>';\n"
  " area.appendChild(card);\n"
  "}\n"
  "function showState(i,on){document.getElementById('r'+i).className='card '+(on?'on':'off');document.getElementById('s'+i).textContent=on?'ON':'OFF';}\n"
  "async function refresh(){try{\n"
  " const r=await fetch('/api/state',{cache:'no-store'});if(!r.ok)throw Error('HTTP');const d=await r.json();\n"
  " d.relays.forEach((v,i)=>showState(i,v===1));\n"
  " const eth=d.ethernet?'Ethernet: '+d.eth_ip:'Ethernet: нет связи';\n"
  " const wifi=d.wifi_sta?'Wi-Fi: '+d.wifi_ip+' ('+d.rssi+' dBm)':(d.wifi_ap?'Wi-Fi AP: '+d.ap_ssid+' | '+d.ap_ip:'Wi-Fi: отключён/нет соединения');\n"
  " document.getElementById('net').textContent=eth+'\\n'+wifi+'\\nMQTT: '+(d.mqtt_connected?'подключён':(d.mqtt_enabled?'нет подключения':'выключен'))+' | Uptime: '+d.uptime+' с';\n"
  "}catch(e){document.getElementById('net').textContent='Нет связи с устройством';}}\n"
  "async function setRelay(ch,val){try{const r=await fetch('/api/set?ch='+ch+'&val='+val,{method:'POST'});if(!r.ok)throw Error('HTTP '+r.status);await refresh();}catch(e){alert('Не удалось переключить реле');}}\n"
  "refresh();setInterval(refresh,1500);\n"
  "</script></body></html>\n";


void handleRoot() {
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/html; charset=utf-8", FPSTR(INDEX_HTML));
}

void handleState() {
  String json = "{\"relays\":[";
  for (int i=0; i<4; i++) { if(i) json += ","; json += relayState[i] ? "1" : "0"; }
  json += "],\"uptime\":"+String(millis()/1000UL);
  json += ",\"ethernet\":"; json += ethReady ? "true" : "false";
  json += ",\"eth_ip\":\"" + ETH.localIP().toString() + "\"";
  json += ",\"wifi_sta\":"; json += staReady ? "true" : "false";
  json += ",\"wifi_ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"wifi_ap\":"; json += apReady ? "true" : "false";
  json += ",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"";
  json += ",\"ap_ssid\":\"" + htmlEscape(apSsid) + "\"";
  json += ",\"rssi\":" + String(staReady ? WiFi.RSSI() : 0);
  json += ",\"wifi_tx_dbm\":" + String((staStarted || apReady)
                                    ? (float)(int)WiFi.getTxPower() / 4.0f : 0.0f, 1);
  json += ",\"mqtt_enabled\":"; json += mqttEnabled ? "true" : "false";
  json += ",\"mqtt_connected\":"; json += mqtt.connected() ? "true" : "false";
  json += ",\"watchdogs\":[";
  for (int i=0;i<4;++i) {
    if (i) json += ",";
    json += "{\"name\":\"" + jsonString(wdName[i]) + "\"";
    json += ",\"mode\":" + String(wd[i].config.mode);
    json += ",\"armed\":"; json += wd[i].active() ? "true" : "false";
    json += ",\"ip\":\"" + wdTarget[i] + "\"";
    json += ",\"state\":\"" + String(wdStateName(wd[i].state)) + "\"";
    json += ",\"failures\":" + String(wd[i].failures);
    json += ",\"cycles\":" + String(wd[i].cyclesTotal);
    json += ",\"cycles_hour\":" + String(wd[i].recentCycles(millis()));
    json += ",\"last_ping_ok\":"; json += wd[i].lastPingOk ? "true" : "false";
    json += "}";
  }
  json += "]}";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", json);
}

void handleSet() {
  if (!web.hasArg("ch") || !web.hasArg("val")) { web.send(400, "text/plain", "Missing args"); return; }
  String ch = web.arg("ch"), val = web.arg("val");
  if (ch.length() != 1 || ch[0] < '0' || ch[0] > '3' || (val != "0" && val != "1")) {
    web.send(400, "text/plain", "Invalid relay command"); return;
  }
  if (!commandRelay(ch[0]-'0', val == "1")) {
    web.send(409, "text/plain", "Watchdog owns this relay; disarm it first"); return;
  }
  web.send(200, "text/plain", "OK");
}

const char* SETTINGS_STYLE =
  "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<style>*{box-sizing:border-box}body{font-family:Arial;background:#111827;color:#f8fafc;max-width:560px;margin:24px auto;padding:18px}"
  "label{display:block;margin-top:15px}input[type=text],input[type=password],input[type=number],select{width:100%;padding:12px;margin-top:6px;border:1px solid #475569;background:#243247;color:white;border-radius:8px}"
  "button{background:#059669;color:white;padding:13px 28px;margin-top:22px;border:0;border-radius:8px;font-size:16px}a{color:#93c5fd}small{color:#94a3b8}</style>";

void handleWifiPage() {
  String page; page.reserve(4400);
  page = "<!doctype html><html lang='ru'><head>";
  page += SETTINGS_STYLE;
  page += "<title>Wi-Fi</title></head><body><h2>Настройки Wi-Fi</h2>";
  page += "<p>Ethernet W5500 работает при любом режиме. При смене настроек устройство перезагрузится.</p>";
  page += "<p><small>Ethernet: "; page += ethReady ? ETH.localIP().toString() : "нет подключения";
  page += "<br>Wi-Fi STA: "; page += staReady ? WiFi.localIP().toString() : "нет подключения";
  page += "<br>Wi-Fi AP: "; page += apReady ? WiFi.softAPIP().toString() : "выключена";
  page += "</small></p><form method='POST' action='/wifi/save'>";
  page += "<label>Режим Wi-Fi</label><select name='mode'>";
  const char* modes[] = {"0: Отключён", "1: Резерв Ethernet (STA)", "2: Ethernet + Wi-Fi STA", "3: Собственная точка доступа (AP)"};
  for (int i=0; i<4; i++) {
    page += "<option value='" + String(i) + "'";
    if (wifiMode == i) page += " selected";
    page += ">" + String(modes[i]) + "</option>";
  }
  page += "</select><h3>Подключение к домашнему Wi-Fi (STA)</h3>";
  page += "<label>SSID, только 2,4 ГГц<input type='text' name='ssid' maxlength='32' value='" + htmlEscape(wifiSsid) + "'></label>";
  page += "<label>Пароль STA (пусто — оставить текущий)<input type='password' name='pass' maxlength='63' autocomplete='new-password'></label>";
  page += "<label><input type='checkbox' name='clear_sta'> Удалить сохранённый пароль STA (открытая сеть)</label>";
  page += "<h3>Собственная точка доступа (AP)</h3>";
  page += "<label>SSID AP<input type='text' name='apssid' maxlength='32' value='" + htmlEscape(apSsid) + "'></label>";
  page += "<label>Пароль AP (8–63 символа; пусто — оставить текущий)<input type='password' name='appass' maxlength='63' autocomplete='new-password'></label>";
  page += "<p><small>Пароль AP хранится в настройках устройства (если пароль уже задан, он сохраняется после обновления). Адрес AP: 192.168.4.1.</small></p>";
  page += "<button type='submit'>Сохранить и перезагрузить</button></form><p><a href='/'>На главную</a></p></body></html>";
  web.send(200, "text/html; charset=utf-8", page);
}

void handleWifiSave() {
  String m = web.arg("mode"), ssid = web.arg("ssid"), apssid = web.arg("apssid");
  String pass = web.arg("pass"), appass = web.arg("appass");
  if (m.length()!=1 || m[0]<'0' || m[0]>'3' || ssid.length()>32 || apssid.isEmpty() || apssid.length()>32) {
    web.send(400, "text/plain", "Invalid WiFi settings"); return;
  }
  uint8_t newMode = m[0]-'0';
  if ((newMode==1 || newMode==2) && ssid.isEmpty()) {
    web.send(400, "text/plain", "SSID required for STA mode"); return;
  }
  String newStaPass = wifiPassword;
  if (web.hasArg("clear_sta")) newStaPass = "";
  else if (!pass.isEmpty()) newStaPass = pass;
  if (!newStaPass.isEmpty() && newStaPass.length()<8) {
    web.send(400, "text/plain", "STA password must have at least 8 chars"); return;
  }
  String newApPass = appass.isEmpty() ? apPassword : appass;
  if (newApPass.length()<8 || newApPass.length()>63) {
    web.send(400, "text/plain", "AP password must have 8-63 chars"); return;
  }
  prefs.begin("relay-wifi", false);
  prefs.putUChar("mode", newMode);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", newStaPass);
  prefs.putString("ap_ssid", apssid);
  prefs.putString("ap_pass", newApPass);
  prefs.end();
  web.send(200, "text/html; charset=utf-8",
      "<meta charset='utf-8'><h2>Настройки сохранены</h2><p>Перезагрузка через 2 секунды.</p><p>Ethernet адрес обычно сохранится. В режиме AP подключитесь к заданной Wi-Fi сети и откройте http://192.168.4.1/</p>");
  rebootAt = millis()+2000;
}

void handleMqttPage() {
  String page; page.reserve(3000);
  page = "<!doctype html><html lang='ru'><head>";
  page += SETTINGS_STYLE;
  page += "<title>MQTT</title></head><body><h2>MQTT</h2><form method='POST' action='/mqtt/save'>";
  page += "<label><input type='checkbox' name='enabled' "; if (mqttEnabled) page += "checked";
  page += "> Включить MQTT</label>";
  page += "<label>IP / DNS брокера<input name='host' type='text' maxlength='100' value='" + htmlEscape(mqttHost) + "'></label>";
  page += "<label>Порт<input name='port' type='number' min='1' max='65535' value='"+String(mqttPort)+"'></label>";
  page += "<label>Логин<input name='user' type='text' maxlength='64' value='"+htmlEscape(mqttUser)+"'></label>";
  page += "<label>Пароль (пусто — оставить)<input name='pass' type='password' maxlength='100' autocomplete='new-password'></label>";
  page += "<label><input type='checkbox' name='clearpass'> Удалить пароль</label>";
  page += "<label>Базовый топик<input name='topic' type='text' maxlength='70' value='"+htmlEscape(mqttBase)+"'></label>";
  page += "<button type='submit'>Сохранить</button></form>";
  page += "<p><small>Состояние: "; page += mqtt.connected() ? "Connected" : "Disconnected";
  page += "</small></p><p><a href='/'>На главную</a></p></body></html>";
  web.send(200, "text/html; charset=utf-8", page);
}

void handleMqttSave() {
  String host=web.arg("host"), user=web.arg("user"), topic=web.arg("topic"), pass=web.arg("pass");
  host.trim(); topic.trim();
  long port=web.arg("port").toInt();
  if (host.length()>100 || user.length()>64 || topic.isEmpty() || topic.length()>70 ||
      topic.indexOf('#')>=0 || topic.indexOf('+')>=0 || topic.indexOf(' ')>=0 ||
      port<1 || port>65535 || (web.hasArg("enabled") && host.isEmpty())) {
    web.send(400, "text/plain", "Invalid MQTT settings"); return;
  }
  while (topic.endsWith("/")) topic.remove(topic.length()-1);
  if (topic.isEmpty()) { web.send(400, "text/plain", "Invalid topic"); return; }
  if (mqtt.connected()) { String t=topicStatus(); mqtt.publish(t.c_str(), "offline", true); mqtt.disconnect(); }
  mqttNet.stop();
  mqttEnabled=web.hasArg("enabled"); mqttHost=host; mqttUser=user;
  mqttPort=(uint16_t)port; mqttBase=topic;
  if (web.hasArg("clearpass")) mqttPassword="";
  else if (!pass.isEmpty()) mqttPassword=pass;
  prefs.begin("relay-mqtt",false);
  prefs.putBool("enabled",mqttEnabled); prefs.putString("host",mqttHost);
  prefs.putUShort("port",mqttPort); prefs.putString("user",mqttUser);
  prefs.putString("pass",mqttPassword); prefs.putString("topic",mqttBase);
  prefs.end();
  mqtt.setServer(mqttHost.c_str(),mqttPort);
  mqttLastTry=millis()-10000UL;
  web.sendHeader("Location", "/mqtt"); web.send(303,"text/plain","");
}


String wdFieldName(const char* prefix, int ch) { return String(prefix) + String(ch); }
String jsonString(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", " ");
  s.replace("\r", " ");
  return s;
}

void handleWatchdogPage() {
  String page;
  page.reserve(12500);
  page = "<!doctype html><html lang='ru'><head>";
  page += SETTINGS_STYLE;
  page += "<title>Watchdog</title></head><body><h2>Реле / Ping Watchdog</h2>";
  page += "<p><small>На каждом канале свой режим и IP. Управление автоматикой отключено, пока не включён флажок «Активировать». Watchdog подразумевает питание через NO: ON=питание подано.</small></p>";
  page += "<form method='POST' action='/watchdog/save'>";
  for (int i=0; i<4; ++i) {
    const WdConfig &c = wd[i].config;
    String ch = String(i);
    page += "<fieldset style='border:1px solid #475569;border-radius:10px;margin:16px 0;padding:12px'>";
    page += "<legend>Реле " + String(i+1) + " — " + htmlEscape(wdName[i]) + "</legend>";
    page += "<label>Имя<input type='text' maxlength='32' name='name"+ch+"' value='"+htmlEscape(wdName[i])+"'></label>";
    page += "<label>Режим<select name='mode"+ch+"'>";
    const char* modes[]={"Ручной ON/OFF","Импульсный","Ping Watchdog"};
    for (int m=0; m<3; ++m) {
      page += "<option value='"+String(m)+"'";
      if (c.mode==m) page+=" selected";
      page+=">"+String(modes[m])+"</option>";
    }
    page += "</select></label>";
    page += "<label><input type='checkbox' name='arm"+ch+"'";
    if (c.armed && c.mode==WD_PING) page += " checked";
    page += "> Активировать Ping Watchdog (может включить реле)</label>";
    page += "<label>Контролируемый IPv4<input type='text' maxlength='15' name='ip"+ch+"' value='"+htmlEscape(wdTarget[i])+"' placeholder='192.168.1.1'></label>";
    page += "<label>Период проверки, с<input type='number' name='int"+ch+"' min='5' max='3600' value='"+String(c.intervalSec)+"'></label>";
    page += "<label>Неудач подряд<input type='number' name='fail"+ch+"' min='1' max='20' value='"+String(c.failLimit)+"'></label>";
    page += "<label>Длительность OFF, с<input type='number' name='off"+ch+"' min='1' max='300' value='"+String(c.offSec)+"'></label>";
    page += "<label>Ожидание загрузки, с<input type='number' name='gr"+ch+"' min='10' max='1800' value='"+String(c.graceSec)+"'></label>";
    page += "<label>Перезапусков в час<input type='number' name='cap"+ch+"' min='1' max='10' value='"+String(c.maxCycles)+"'></label>";
    page += "<label>Импульс, мс<input type='number' name='pulse"+ch+"' min='100' max='60000' value='"+String(c.pulseMs)+"'></label>";
    page += "<p><small>Состояние: "+String(wdStateName(wd[i].state));
    page += " | ping ошибок: "+String(wd[i].failures);
    page += " | циклов с запуска: "+String(wd[i].cyclesTotal);
    page += " | за час: "+String(wd[i].recentCycles(millis()));
    page += " / "+String(c.maxCycles);
    page += "</small></p></fieldset>";
  }
  page += "<button type='submit'>Сохранить настройки</button></form>";
  page += "<form method='POST' action='/watchdog/reset'>";
  page += "<label>Снять блокировку канала<select name='ch'>";
  for(int i=0;i<4;i++)page+="<option value='"+String(i)+"'>Реле "+String(i+1)+"</option>";
  page+="</select></label><button type='submit'>Сбросить LOCKOUT</button></form>";
  page+="<p><a href='/'>На главную</a></p></body></html>";
  web.send(200,"text/html; charset=utf-8",page);
}

bool validWdNum(const String &s, long minVal, long maxVal, long &out) {
  if (s.length()==0) return false;
  for (unsigned int i=0; i<s.length(); ++i) if (s[i]<'0'||s[i]>'9') return false;
  out=s.toInt();
  return out>=minVal && out<=maxVal;
}

void handleWatchdogSave() {
  WdConfig changed[4];
  String names[4], ips[4];
  for (int i=0; i<4; ++i) {
    String k=String(i);
    String mode=web.arg("mode"+k);
    if (mode.length()!=1 || mode[0]<'0' || mode[0]>'2') {
      web.send(400,"text/plain","Invalid mode"); return;
    }
    changed[i].mode=mode[0]-'0';
    changed[i].armed=(changed[i].mode==WD_PING && web.hasArg("arm"+k));
    names[i]=web.arg("name"+k);
    names[i].trim();
    ips[i]=web.arg("ip"+k);
    ips[i].trim();
    IPAddress ip;
    if (names[i].length()<1 || names[i].length()>32 ||
       (ips[i].length() && (!ip.fromString(ips[i]) || ip[0]==0 || ip[0]>=224))) {
      web.send(400,"text/plain","Invalid channel name / IPv4"); return;
    }
    if (changed[i].armed && ips[i].isEmpty()) {
      web.send(400,"text/plain","Armed watchdog requires target IP"); return;
    }
    long v;
    if (!validWdNum(web.arg("int"+k),5,3600,v)) { web.send(400,"text/plain","Bad interval"); return; }
    changed[i].intervalSec=v;
    if (!validWdNum(web.arg("fail"+k),1,20,v)) { web.send(400,"text/plain","Bad failure threshold"); return; }
    changed[i].failLimit=v;
    if (!validWdNum(web.arg("off"+k),1,300,v)) { web.send(400,"text/plain","Bad OFF duration"); return; }
    changed[i].offSec=v;
    if (!validWdNum(web.arg("gr"+k),10,1800,v)) { web.send(400,"text/plain","Bad boot grace"); return; }
    changed[i].graceSec=v;
    if (!validWdNum(web.arg("cap"+k),1,10,v)) { web.send(400,"text/plain","Bad cycles/hour"); return; }
    changed[i].maxCycles=v;
    if (!validWdNum(web.arg("pulse"+k),100,60000,v)) { web.send(400,"text/plain","Bad pulse ms"); return; }
    changed[i].pulseMs=v;
  }
  // All four channels validated; apply atomically at a logical level.
  for (int i=0; i<4; ++i) {
    const bool targetChanged = (wdTarget[i] != ips[i]);
    wdTarget[i] = ips[i];
    wdName[i] = names[i];
    applyWdAction(i, wd[i].configure(changed[i], millis(), targetChanged));
  }
  saveWatchdogSettings();
  web.sendHeader("Location","/watchdog");
  web.send(303,"text/plain","");
}

void handleWatchdogReset() {
  String s=web.arg("ch");
  if (s.length()!=1 || s[0]<'0'||s[0]>'3') {
    web.send(400,"text/plain","Invalid channel"); return;
  }
  int ch=s[0]-'0';
  if (wd[ch].state != WD_LOCKOUT) {
    web.send(409, "text/plain", "Channel is not in LOCKOUT"); return;
  }
  wd[ch].resetLockout(millis());
  watchdogEvent(ch,"lockout_cleared");
  web.sendHeader("Location","/watchdog");
  web.send(303,"text/plain","");
}

void handleUpdatePage() {
  const char* page =
    "<!doctype html><html lang='ru'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'></head>"
    "<body style='background:#111827;color:white;font:16px Arial;max-width:520px;margin:40px auto;padding:20px'>"
    "<h2>Web OTA · v1.4.3-dev</h2><form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin' required><p><button type='submit'>Загрузить .bin</button></p>"
    "</form><p>Не отключайте питание во время обновления.</p><a style='color:#93c5fd' href='/'>Назад</a></body></html>";
  web.send(200,"text/html; charset=utf-8",page);
}

void handleUpdateUpload() {
  HTTPUpload &upload=web.upload();
  if (upload.status==UPLOAD_FILE_START) {
    otaOK=Update.begin(UPDATE_SIZE_UNKNOWN);
    Serial.printf("OTA: %s\n",upload.filename.c_str());
    if (!otaOK) Update.printError(Serial);
  } else if (upload.status==UPLOAD_FILE_WRITE) {
    if (otaOK && Update.write(upload.buf,upload.currentSize)!=upload.currentSize) {
      otaOK=false; Update.printError(Serial);
    }
  } else if (upload.status==UPLOAD_FILE_END) {
    if (otaOK) otaOK=Update.end(true);
    else Update.end(false);
    Serial.printf("OTA end: %s\n",otaOK?"OK":"FAILED");
    if (!otaOK) Update.printError(Serial);
  } else if (upload.status==UPLOAD_FILE_ABORTED) {
    Update.abort(); otaOK=false;
  }
}

void handleUpdateResult() {
  if (otaOK && !Update.hasError()) {
    web.send(200,"text/html; charset=utf-8","<meta charset='utf-8'><h2>Прошивка загружена.</h2><p>Перезагрузка...</p>");
    rebootAt=millis()+2000;
  } else web.send(500,"text/plain","OTA failed; see Serial Monitor");
}

void setup() {
  // Turn built-in active-LOW LED OFF before configuring output.
  // Never connect another load to GPIO8: it is a boot strapping pin.
  digitalWrite(WIFI_LED_PIN, HIGH);
  pinMode(WIFI_LED_PIN, OUTPUT);
  // HW relay board uses HIGH trigger; no relay should energize on startup.
  for (int i=0;i<4;i++) {
    digitalWrite(relayPins[i],LOW);
    pinMode(relayPins[i],OUTPUT);
    relayState[i]=false;
  }
  Serial.begin(115200);
  delay(500);
  Serial.println("\nESP32-C3 Ethernet Relay v1.4.3-dev (WiFi TX 8.5 dBm / WiFi LED GPIO8)");
  loadSettings();
  loadWatchdogSettings();
  pingQueue = xQueueCreate(12, sizeof(PingEvent));
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(20);
  mqtt.setSocketTimeout(3);
  mqtt.setServer(mqttHost.c_str(),mqttPort);
  web.on("/",HTTP_GET,handleRoot);
  web.on("/api/state",HTTP_GET,handleState);
  web.on("/api/set",HTTP_POST,handleSet);
  web.on("/wifi",HTTP_GET,handleWifiPage);
  web.on("/wifi/save",HTTP_POST,handleWifiSave);
  web.on("/mqtt",HTTP_GET,handleMqttPage);
  web.on("/mqtt/save",HTTP_POST,handleMqttSave);
  web.on("/watchdog",HTTP_GET,handleWatchdogPage);
  web.on("/watchdog/save",HTTP_POST,handleWatchdogSave);
  web.on("/watchdog/reset",HTTP_POST,handleWatchdogReset);
  web.on("/update",HTTP_GET,handleUpdatePage);
  web.on("/update",HTTP_POST,handleUpdateResult,handleUpdateUpload);
  Network.onEvent(onNetworkEvent);
  bool ok=ETH.begin(ETH_PHY_W5500,1,ETH_CS,ETH_IRQ,ETH_RST,
                    SPI2_HOST,ETH_SCK,ETH_MISO,ETH_MOSI);
  Serial.println(ok?"ETH.begin OK":"ETH.begin FAILED");
  startAt=millis();
  startWifiMode();
}

void loop() {
  if (rebootAt && (int32_t)(millis()-rebootAt)>=0) ESP.restart();
  serviceWifi();
  serviceWifiLed();
  chooseRoute();
  bool anyNetwork=ethReady || staReady || apReady;
  if (anyNetwork && !servicesStarted) {
    web.begin(); mbServer.begin(); servicesStarted=true;
    Serial.println("Web :80 and Modbus TCP :502 started");
  }
  if (servicesStarted) {
    web.handleClient();
    serviceModbus();
  }
  serviceMqtt();
  serviceWatchdogs();
  delay(2);
}