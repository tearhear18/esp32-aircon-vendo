#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include "RTClib.h"
#include "time.h"

RTC_DS3231 rtc;
WebServer server(80);

// WiFi configuration
struct WiFiConfig {
  char ssid[32];
  char password[64];
  bool isConfigured;
};

WiFiConfig wifiConfig;

// AP mode credentials
const char* ap_ssid = "ESP32-AirconTimer";
const char* ap_password = "12345678";  // Must be at least 8 characters

// EEPROM addresses
const int EEPROM_SIZE = 512;
const int WIFI_CONFIG_ADDRESS = 0;

// Use Asia/Manila (UTC+8, no DST)
const char* ntpServer = "asia.pool.ntp.org";
const long  gmtOffset_sec = 8 * 3600;
const int   daylightOffset_sec = 0;

// Configuration mode variables
bool configMode = false;
unsigned long configModeStartTime = 0;
const unsigned long CONFIG_MODE_TIMEOUT = 300000; // 5 minutes
String scannedNetworks = "";
unsigned long lastScan = 0;
const unsigned long SCAN_INTERVAL = 10000; // Scan every 10 seconds

// Timer settings
struct TimerSettings {
  bool enabled = false;
  uint8_t onHour = 18;    // 6 PM
  uint8_t onMinute = 0;
  uint8_t offHour = 6;    // 6 AM
  uint8_t offMinute = 0;
};

TimerSettings timerSettings;

// Pins
const int RELAY_PIN = 2;  // Pin to control air conditioning relay
const int LED_PIN = LED_BUILTIN;
const int CONFIG_BUTTON_PIN = 0;  // Boot button for entering config mode

// Timing variables
unsigned long lastTimeCheck = 0;
const unsigned long TIME_CHECK_INTERVAL = 60000; // Check every minute instead of every second
bool airconState = false;
bool lastAirconState = false;

// EEPROM functions
void saveWiFiConfig() {
  EEPROM.put(WIFI_CONFIG_ADDRESS, wifiConfig);
  EEPROM.commit();
  Serial.println("WiFi config saved to EEPROM");
}

void loadWiFiConfig() {
  EEPROM.get(WIFI_CONFIG_ADDRESS, wifiConfig);
  
  // Check if EEPROM data is valid
  if (strlen(wifiConfig.ssid) == 0 || strlen(wifiConfig.ssid) > 31) {
    wifiConfig.isConfigured = false;
    strcpy(wifiConfig.ssid, "");
    strcpy(wifiConfig.password, "");
    Serial.println("No valid WiFi config found in EEPROM");
  } else {
    Serial.printf("Loaded WiFi config: SSID=%s\n", wifiConfig.ssid);
  }
}

// WiFi scanning function
String scanNetworks() {
  Serial.println("Scanning for WiFi networks...");
  int n = WiFi.scanNetworks();
  String networks = "";
  
  if (n == 0) {
    networks = "<option value=''>No networks found</option>";
  } else {
    // Sort networks by signal strength (optional)
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      int32_t rssi = WiFi.RSSI(i);
      String security = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "Open" : "Secured";
      
      // Create option with signal strength indicator
      String signalBars = "";
      if (rssi > -50) signalBars = "Excellent";
      else if (rssi > -60) signalBars = "Good";
      else if (rssi > -70) signalBars = "Fair";
      else signalBars = "Weak";
      
      networks += "<option value='" + ssid + "'>" + ssid + " (" + signalBars + " " + rssi + "dBm, " + security + ")</option>";
    }
  }
  
  WiFi.scanDelete(); // Clean up
  return networks;
}

// Get device status for web interface
String getDeviceStatus() {
  String status = "";
  
  // Device info
  status += "\"deviceInfo\": {";
  status += "\"chipModel\": \"" + String(ESP.getChipModel()) + "\",";
  status += "\"freeHeap\": " + String(ESP.getFreeHeap()) + ",";
  status += "\"uptime\": " + String(millis() / 1000) + ",";
  status += "\"flashSize\": " + String(ESP.getFlashChipSize()) + "";
  status += "},";
  
  // WiFi info
  status += "\"wifiInfo\": {";
  if (WiFi.status() == WL_CONNECTED) {
    status += "\"connected\": true,";
    status += "\"ssid\": \"" + WiFi.SSID() + "\",";
    status += "\"ip\": \"" + WiFi.localIP().toString() + "\",";
    status += "\"rssi\": " + String(WiFi.RSSI()) + ",";
    status += "\"mac\": \"" + WiFi.macAddress() + "\"";
  } else {
    status += "\"connected\": false,";
    status += "\"ssid\": \"" + String(wifiConfig.ssid) + "\",";
    status += "\"ip\": \"Not connected\",";
    status += "\"rssi\": 0,";
    status += "\"mac\": \"" + WiFi.macAddress() + "\"";
  }
  status += "},";
  
  // RTC info
  DateTime now = rtc.now();
  status += "\"rtcInfo\": {";
  status += "\"connected\": " + String(rtc.begin() ? "true" : "false") + ",";
  status += "\"time\": \"" + String(now.year()) + "-" + 
            String(now.month() < 10 ? "0" : "") + String(now.month()) + "-" + 
            String(now.day() < 10 ? "0" : "") + String(now.day()) + " " +
            String(now.hour() < 10 ? "0" : "") + String(now.hour()) + ":" + 
            String(now.minute() < 10 ? "0" : "") + String(now.minute()) + ":" + 
            String(now.second() < 10 ? "0" : "") + String(now.second()) + "\",";
  status += "\"temperature\": " + String(rtc.getTemperature()) + "";
  status += "},";
  
  // Timer info
  status += "\"timerInfo\": {";
  status += "\"enabled\": " + String(timerSettings.enabled ? "true" : "false") + ",";
  status += "\"onTime\": \"" + String(timerSettings.onHour < 10 ? "0" : "") + String(timerSettings.onHour) + ":" + 
            String(timerSettings.onMinute < 10 ? "0" : "") + String(timerSettings.onMinute) + "\",";
  status += "\"offTime\": \"" + String(timerSettings.offHour < 10 ? "0" : "") + String(timerSettings.offHour) + ":" + 
            String(timerSettings.offMinute < 10 ? "0" : "") + String(timerSettings.offMinute) + "\",";
  status += "\"airconState\": " + String(airconState ? "true" : "false") + "";
  status += "}";
  
  return "{" + status + "}";
}

// Web server handlers
void handleRoot() {
  // Perform network scan if needed
  if (millis() - lastScan > SCAN_INTERVAL || scannedNetworks.isEmpty()) {
    scannedNetworks = scanNetworks();
    lastScan = millis();
  }
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<title>ESP32 Aircon Timer - Configuration</title>";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', Arial, sans-serif; margin: 0; padding: 20px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; }";
  html += ".container { max-width: 800px; margin: 0 auto; background: white; border-radius: 15px; box-shadow: 0 10px 30px rgba(0,0,0,0.2); overflow: hidden; }";
  html += ".header { background: linear-gradient(135deg, #4CAF50 0%, #45a049 100%); color: white; padding: 30px; text-align: center; }";
  html += ".header h1 { margin: 0; font-size: 2em; }";
  html += ".header p { margin: 10px 0 0 0; opacity: 0.9; }";
  html += ".content { padding: 30px; }";
  html += ".status-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 20px; margin-bottom: 30px; }";
  html += ".status-card { background: #f8f9fa; border-radius: 10px; padding: 20px; border-left: 4px solid #4CAF50; }";
  html += ".status-card h3 { margin: 0 0 15px 0; color: #333; font-size: 1.1em; }";
  html += ".status-item { display: flex; justify-content: space-between; margin: 8px 0; padding: 5px 0; border-bottom: 1px solid #eee; }";
  html += ".status-item:last-child { border-bottom: none; }";
  html += ".status-label { font-weight: 500; color: #666; }";
  html += ".status-value { color: #333; font-family: monospace; }";
  html += ".wifi-form { background: #f8f9fa; border-radius: 10px; padding: 25px; margin-top: 20px; }";
  html += ".form-group { margin-bottom: 20px; }";
  html += "label { display: block; margin-bottom: 8px; font-weight: 500; color: #333; }";
  html += "select, input[type=\"password\"], input[type=\"text\"] { width: 100%; padding: 12px; border: 2px solid #ddd; border-radius: 8px; font-size: 16px; box-sizing: border-box; transition: border-color 0.3s; }";
  html += "select:focus, input:focus { outline: none; border-color: #4CAF50; }";
  html += ".btn { background: linear-gradient(135deg, #4CAF50 0%, #45a049 100%); color: white; padding: 15px 30px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; width: 100%; transition: transform 0.2s; }";
  html += ".btn:hover { transform: translateY(-2px); }";
  html += ".refresh-btn { float: right; background: #17a2b8; color: white; border: none; padding: 5px 10px; border-radius: 5px; cursor: pointer; font-size: 12px; }";
  html += ".connected { color: #28a745; font-weight: bold; }";
  html += ".disconnected { color: #dc3545; font-weight: bold; }";
  html += ".footer { text-align: center; padding: 20px; color: #666; font-size: 14px; border-top: 1px solid #eee; }";
  html += "</style></head><body>";
  
  html += "<div class=\"container\">";
  html += "<div class=\"header\">";
  html += "<h1>ESP32 Aircon Timer</h1>";
  html += "<p>Configuration & Status Dashboard</p>";
  html += "</div>";
  
  html += "<div class=\"content\">";
  html += "<div class=\"status-grid\">";
  html += "<div class=\"status-card\"><h3>WiFi Status</h3><div id=\"wifi-status\">Loading...</div></div>";
  html += "<div class=\"status-card\"><h3>Clock & Timer</h3><div id=\"timer-status\">Loading...</div></div>";
  html += "<div class=\"status-card\"><h3>Device Info</h3><div id=\"device-status\">Loading...</div></div>";
  html += "<div class=\"status-card\"><h3>Aircon Control</h3><div id=\"aircon-status\">Loading...</div>";
  html += "<button class=\"btn\" onclick=\"toggleAircon()\" id=\"aircon-btn\">Toggle Aircon</button>";
  html += "<button class=\"btn\" onclick=\"toggleTimer()\" id=\"timer-btn\" style=\"margin-top: 10px;\">Toggle Timer</button>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class=\"wifi-form\">";
  html += "<h3>WiFi Configuration <button class=\"refresh-btn\" onclick=\"refreshNetworks()\">Refresh</button></h3>";
  html += "<form action=\"/save\" method=\"POST\">";
  html += "<div class=\"form-group\">";
  html += "<label for=\"ssid\">Available Networks:</label>";
  html += "<select id=\"ssid\" name=\"ssid\" onchange=\"document.getElementById('manual-ssid').value = this.value\" required>";
  html += "<option value=\"\">Select a network...</option>";
  html += scannedNetworks;
  html += "</select></div>";
  
  html += "<div class=\"form-group\">";
  html += "<label for=\"manual-ssid\">Or enter manually:</label>";
  html += "<input type=\"text\" id=\"manual-ssid\" placeholder=\"Network name (SSID)\" onchange=\"if(this.value) document.getElementById('ssid').value = this.value\">";
  html += "</div>";
  
  html += "<div class=\"form-group\">";
  html += "<label for=\"password\">WiFi Password:</label>";
  html += "<input type=\"password\" id=\"password\" name=\"password\" placeholder=\"Enter WiFi password\" required>";
  html += "</div>";
  
  html += "<button type=\"submit\" class=\"btn\">Save & Connect</button>";
  html += "</form></div></div>";
  
  html += "<div class=\"footer\">";
  html += "<p>Device will restart after saving new credentials</p>";
  html += "<p>Auto-refresh every 5 seconds | Last updated: <span id=\"last-update\"></span></p>";
  html += "</div></div>";
  
  html += "<script>";
  html += "function updateStatus() {";
  html += "fetch('/status').then(response => response.json()).then(data => {";
  
  html += "var wifiHtml = \"\";";
  html += "wifiHtml += '<div class=\"status-item\"><span class=\"status-label\">Status:</span>';";
  html += "wifiHtml += '<span class=\"status-value ' + (data.wifiInfo.connected ? 'connected' : 'disconnected') + '\">';";
  html += "wifiHtml += data.wifiInfo.connected ? 'Connected' : 'Disconnected';";
  html += "wifiHtml += '</span></div>';";
  html += "wifiHtml += '<div class=\"status-item\"><span class=\"status-label\">SSID:</span>';";
  html += "wifiHtml += '<span class=\"status-value\">' + (data.wifiInfo.ssid || 'None') + '</span></div>';";
  html += "wifiHtml += '<div class=\"status-item\"><span class=\"status-label\">IP Address:</span>';";
  html += "wifiHtml += '<span class=\"status-value\">' + data.wifiInfo.ip + '</span></div>';";
  html += "wifiHtml += '<div class=\"status-item\"><span class=\"status-label\">Signal:</span>';";
  html += "wifiHtml += '<span class=\"status-value\">' + data.wifiInfo.rssi + ' dBm</span></div>';";
  html += "wifiHtml += '<div class=\"status-item\"><span class=\"status-label\">MAC:</span>';";
  html += "wifiHtml += '<span class=\"status-value\">' + data.wifiInfo.mac + '</span></div>';";
  html += "document.getElementById('wifi-status').innerHTML = wifiHtml;";
  
  html += "var timerHtml = \"\";";
  html += "timerHtml += '<div class=\"status-item\"><span class=\"status-label\">RTC Time:</span>';";
  html += "timerHtml += '<span class=\"status-value\">' + data.rtcInfo.time + '</span></div>';";
  html += "timerHtml += '<div class=\"status-item\"><span class=\"status-label\">Temperature:</span>';";
  html += "timerHtml += '<span class=\"status-value\">' + data.rtcInfo.temperature + '&deg;C</span></div>';";
  html += "timerHtml += '<div class=\"status-item\"><span class=\"status-label\">Timer:</span>';";
  html += "timerHtml += '<span class=\"status-value ' + (data.timerInfo.enabled ? 'connected' : 'disconnected') + '\">';";
  html += "timerHtml += data.timerInfo.enabled ? 'Enabled' : 'Disabled';";
  html += "timerHtml += '</span></div>';";
  html += "timerHtml += '<div class=\"status-item\"><span class=\"status-label\">Schedule:</span>';";
  html += "timerHtml += '<span class=\"status-value\">' + data.timerInfo.onTime + ' - ' + data.timerInfo.offTime + '</span></div>';";
  html += "document.getElementById('timer-status').innerHTML = timerHtml;";
  
  html += "var deviceHtml = \"\";";
  html += "deviceHtml += '<div class=\"status-item\"><span class=\"status-label\">Chip:</span>';";
  html += "deviceHtml += '<span class=\"status-value\">' + data.deviceInfo.chipModel + '</span></div>';";
  html += "deviceHtml += '<div class=\"status-item\"><span class=\"status-label\">Uptime:</span>';";
  html += "deviceHtml += '<span class=\"status-value\">' + Math.floor(data.deviceInfo.uptime / 3600) + 'h ' + Math.floor((data.deviceInfo.uptime % 3600) / 60) + 'm</span></div>';";
  html += "deviceHtml += '<div class=\"status-item\"><span class=\"status-label\">Free Memory:</span>';";
  html += "deviceHtml += '<span class=\"status-value\">' + (data.deviceInfo.freeHeap / 1024).toFixed(1) + ' KB</span></div>';";
  html += "deviceHtml += '<div class=\"status-item\"><span class=\"status-label\">Flash:</span>';";
  html += "deviceHtml += '<span class=\"status-value\">' + (data.deviceInfo.flashSize / 1024 / 1024).toFixed(1) + ' MB</span></div>';";
  html += "document.getElementById('device-status').innerHTML = deviceHtml;";
  
  html += "var airconHtml = \"\";";
  html += "airconHtml += '<div class=\"status-item\"><span class=\"status-label\">Current State:</span>';";
  html += "airconHtml += '<span class=\"status-value ' + (data.timerInfo.airconState ? 'connected' : 'disconnected') + '\">';";
  html += "airconHtml += data.timerInfo.airconState ? 'ON' : 'OFF';";
  html += "airconHtml += '</span></div>';";
  html += "airconHtml += '<div class=\"status-item\"><span class=\"status-label\">Auto Control:</span>';";
  html += "airconHtml += '<span class=\"status-value\">' + (data.timerInfo.enabled ? 'Active' : 'Inactive') + '</span></div>';";
  html += "airconHtml += '<div class=\"status-item\"><span class=\"status-label\">Next Action:</span>';";
  html += "airconHtml += '<span class=\"status-value\">' + (data.timerInfo.airconState ? 'Turn OFF at ' + data.timerInfo.offTime : 'Turn ON at ' + data.timerInfo.onTime) + '</span></div>';";
  html += "document.getElementById('aircon-status').innerHTML = airconHtml;";
  
  html += "document.getElementById('last-update').innerText = new Date().toLocaleTimeString();";
  html += "}).catch(error => { console.error('Error updating status:', error); }); }";
  
  html += "function refreshNetworks() {";
  html += "document.querySelector('.refresh-btn').innerText = 'Scanning...';";
  html += "fetch('/scan').then(response => response.text()).then(data => {";
  html += "document.getElementById('ssid').innerHTML = '<option value=\"\">Select a network...</option>' + data;";
  html += "document.querySelector('.refresh-btn').innerText = 'Refresh';";
  html += "}).catch(error => { console.error('Error scanning networks:', error); document.querySelector('.refresh-btn').innerText = 'Error'; }); }";
  
  html += "function toggleAircon() {";
  html += "fetch('/toggle-aircon').then(response => response.json()).then(data => {";
  html += "if(data.success) { updateStatus(); alert(data.message); }";
  html += "}).catch(error => { console.error('Error toggling aircon:', error); }); }";
  
  html += "function toggleTimer() {";
  html += "fetch('/toggle-timer').then(response => response.json()).then(data => {";
  html += "if(data.success) { updateStatus(); alert(data.message); }";
  html += "}).catch(error => { console.error('Error toggling timer:', error); }); }";
  
  html += "updateStatus(); setInterval(updateStatus, 5000);";
  html += "</script></body></html>";
  
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("password")) {
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    
    // Clear the struct
    memset(&wifiConfig, 0, sizeof(wifiConfig));
    
    // Copy new credentials
    ssid.toCharArray(wifiConfig.ssid, sizeof(wifiConfig.ssid));
    password.toCharArray(wifiConfig.password, sizeof(wifiConfig.password));
    wifiConfig.isConfigured = true;
    
    // Save to EEPROM
    saveWiFiConfig();
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<title>WiFi Saved</title>";
    html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
    html += "<style>";
    html += "body { font-family: Arial; margin: 40px; background-color: #f0f0f0; text-align: center; }";
    html += ".container { max-width: 400px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
    html += ".success { background-color: #d4edda; color: #155724; border: 1px solid #c3e6cb; padding: 15px; border-radius: 5px; margin: 20px 0; }";
    html += "</style></head><body>";
    html += "<div class=\"container\">";
    html += "<h1>WiFi Configuration Saved!</h1>";
    html += "<div class=\"success\">";
    html += "<strong>New Settings:</strong><br>";
    html += "SSID: " + ssid + "<br>";
    html += "Password: ";
    for(int i = 0; i < password.length(); i++) {
      html += "*";
    }
    html += "</div>";
    html += "<p>Device will restart in 3 seconds and attempt to connect to your WiFi network.</p>";
    html += "<p><small>If connection fails, hold the boot button for 3 seconds to enter config mode again.</small></p>";
    html += "</div>";
    html += "<script>";
    html += "setTimeout(function(){ ";
    html += "document.body.innerHTML = '<div class=\"container\"><h1>Restarting...</h1><p>Please reconnect to your main WiFi network.</p></div>'; ";
    html += "}, 3000);";
    html += "</script></body></html>";
    
    server.send(200, "text/html", html);
    
    // Restart after a short delay
    delay(3000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing SSID or password");
  }
}

// New API endpoints
void handleStatus() {
  server.send(200, "application/json", getDeviceStatus());
}

void handleScan() {
  String networks = scanNetworks();
  scannedNetworks = networks;
  lastScan = millis();
  server.send(200, "text/html", networks);
}

void handleToggleAircon() {
  // Toggle aircon manually
  airconState = !airconState;
  digitalWrite(RELAY_PIN, airconState ? HIGH : LOW);
  digitalWrite(LED_PIN, airconState ? HIGH : LOW);
  
  Serial.printf("Aircon manually turned %s\n", airconState ? "ON" : "OFF");
  
  String response = "{\"success\": true, \"airconState\": " + String(airconState ? "true" : "false") + ", \"message\": \"Aircon turned " + String(airconState ? "ON" : "OFF") + "\"}";
  server.send(200, "application/json", response);
}

void handleToggleTimer() {
  // Toggle timer enable/disable
  timerSettings.enabled = !timerSettings.enabled;
  
  Serial.printf("Timer %s\n", timerSettings.enabled ? "ENABLED" : "DISABLED");
  
  String response = "{\"success\": true, \"timerEnabled\": " + String(timerSettings.enabled ? "true" : "false") + ", \"message\": \"Timer " + String(timerSettings.enabled ? "enabled" : "disabled") + "\"}";
  server.send(200, "application/json", response);
}

void handleNotFound() {
  // Redirect to root for captive portal behavior
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void startConfigMode() {
  Serial.println("Starting WiFi configuration mode...");
  configMode = true;
  configModeStartTime = millis();
  
  // Start AP mode
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("AP Mode started. Connect to: %s\n", ap_ssid);
  Serial.printf("Password: %s\n", ap_password);
  Serial.printf("Open browser and go to: http://%s\n", IP.toString().c_str());
  
  // Initial network scan
  scannedNetworks = scanNetworks();
  lastScan = millis();
  
  // Setup web server with new endpoints
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", handleStatus);
  server.on("/scan", handleScan);
  server.onNotFound(handleNotFound);
  server.begin();
  
  Serial.println("Web server started with status dashboard");
  
  // Flash LED to indicate config mode
  for(int i = 0; i < 5; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
  }
}

bool connectToWiFi() {
  if (!wifiConfig.isConfigured) {
    Serial.println("No WiFi credentials configured");
    return false;
  }
  
  Serial.printf("Connecting to %s", wifiConfig.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiConfig.ssid, wifiConfig.password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connection failed!");
    return false;
  }
  
  Serial.println(" CONNECTED");
  Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

bool initializeWiFiAndNTP() {
  if (!connectToWiFi()) {
    return false;
  }
  
  // Init NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  struct tm timeinfo;
  int ntpAttempts = 0;
  while (!getLocalTime(&timeinfo) && ntpAttempts < 10) {
    delay(1000);
    ntpAttempts++;
    Serial.print("Getting time from NTP...");
  }
  
  if (ntpAttempts >= 10) {
    Serial.println("Failed to obtain time from NTP");
    return false;
  }
  
  Serial.println("Time obtained from NTP:");
  Serial.printf("%04d-%02d-%02d %02d:%02d:%02d\n", 
    timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
    timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  
  // Set RTC with NTP time
  rtc.adjust(DateTime(
    timeinfo.tm_year + 1900,
    timeinfo.tm_mon + 1,
    timeinfo.tm_mday,
    timeinfo.tm_hour,
    timeinfo.tm_min,
    timeinfo.tm_sec
  ));
  
  Serial.println("RTC time set from NTP!");
  
  // Keep WiFi connected for web server access
  Serial.println("WiFi staying connected for web server access");
  
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== ESP32 Aircon Timer Starting ===");
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Initialize pins
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(RELAY_PIN, LOW);  // Ensure aircon is off initially
  digitalWrite(LED_PIN, LOW);

  // Load WiFi configuration from EEPROM
  loadWiFiConfig();
  
  // Check if config button is pressed during startup (for 5 seconds for easier detection)
  bool enterConfigMode = false;
  Serial.println("Hold BOOT button for 3 seconds to enter config mode...");
  for(int i = 0; i < 50; i++) {  // Increased from 30 to 50 (5 seconds)
    if(digitalRead(CONFIG_BUTTON_PIN) == LOW) {
      Serial.print(".");
      enterConfigMode = true;
      // Continue checking to ensure button is held for full duration
    } else if (enterConfigMode) {
      // Button was released before 3 seconds
      enterConfigMode = false;
      Serial.println(" Button released too early, continuing normal boot");
      break;
    }
    delay(100);
  }
  
  if (enterConfigMode) {
    Serial.println(" BOOT button detected - CLEARING WiFi config and entering configuration mode");
    // Clear WiFi configuration to force config mode
    memset(&wifiConfig, 0, sizeof(wifiConfig));
    wifiConfig.isConfigured = false;
    saveWiFiConfig();
  }
  
  if (enterConfigMode || !wifiConfig.isConfigured) {
    if (enterConfigMode) {
      Serial.println("Config button pressed - entering configuration mode");
    } else {
      Serial.println("No WiFi credentials found - entering configuration mode");
    }
    startConfigMode();
    return; // Skip the rest of setup when in config mode
  }

  Serial.printf("WiFi configured for: %s\n", wifiConfig.ssid);

  // Start I2C on ESP32 default pins (SDA=21, SCL=22)
  Wire.begin(21, 22);

  // Initialize RTC
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    // Flash LED to indicate error
    for(int i = 0; i < 10; i++) {
      digitalWrite(LED_PIN, HIGH);
      delay(200);
      digitalWrite(LED_PIN, LOW);
      delay(200);
    }
    while (1) delay(1000); // Halt with periodic delay
  }
  
  // Check if RTC lost power and time is unrealistic
  DateTime now = rtc.now();
  if (now.year() < 2020) {
    Serial.println("RTC time seems invalid, syncing with NTP...");
    if (!initializeWiFiAndNTP()) {
      Serial.println("Failed to initialize WiFi/NTP. Using default RTC time.");
    }
  } else {
    Serial.println("RTC time seems valid, connecting to WiFi for web server...");
    Serial.printf("Current RTC time: %04d-%02d-%02d %02d:%02d:%02d\n",
      now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
    
    // Connect to WiFi for web server access
    if (connectToWiFi()) {
      // Setup web server for normal operation
      server.on("/", handleRoot);
      server.on("/save", HTTP_POST, handleSave);
      server.on("/status", handleStatus);
      server.on("/scan", handleScan);
      server.on("/toggle-aircon", handleToggleAircon);
      server.on("/toggle-timer", handleToggleTimer);
      server.onNotFound(handleNotFound);
      server.begin();
      
      Serial.println("Web server started for monitoring and control");
      Serial.printf("Access dashboard at: http://%s\n", WiFi.localIP().toString().c_str());
    }
  }
  
  // Initialize timer settings
  timerSettings.enabled = true;  // Enable timer by default
  
  Serial.println("Setup completed!");
  Serial.printf("Timer: %s (ON: %02d:%02d, OFF: %02d:%02d)\n", 
    timerSettings.enabled ? "ENABLED" : "DISABLED",
    timerSettings.onHour, timerSettings.onMinute,
    timerSettings.offHour, timerSettings.offMinute);
}

void controlAircon(bool shouldBeOn) {
  if (shouldBeOn != airconState) {
    airconState = shouldBeOn;
    digitalWrite(RELAY_PIN, airconState ? HIGH : LOW);
    digitalWrite(LED_PIN, airconState ? HIGH : LOW);
    
    Serial.printf("Aircon turned %s\n", airconState ? "ON" : "OFF");
  }
}

bool isTimeInRange(uint8_t currentHour, uint8_t currentMinute, 
                   uint8_t startHour, uint8_t startMinute,
                   uint8_t endHour, uint8_t endMinute) {
  uint16_t currentTime = currentHour * 60 + currentMinute;
  uint16_t startTime = startHour * 60 + startMinute;
  uint16_t endTime = endHour * 60 + endMinute;
  
  if (startTime <= endTime) {
    // Same day range (e.g., 9:00 to 17:00)
    return currentTime >= startTime && currentTime < endTime;
  } else {
    // Overnight range (e.g., 18:00 to 6:00 next day)
    return currentTime >= startTime || currentTime < endTime;
  }
}

void loop() {
  // Handle configuration mode
  if (configMode) {
    server.handleClient();
    
    // Blink LED to indicate config mode
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 1000) {
      lastBlink = millis();
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
    
    // Check for timeout
    if (millis() - configModeStartTime > CONFIG_MODE_TIMEOUT) {
      Serial.println("Configuration mode timed out. Restarting...");
      ESP.restart();
    }
    
    return;
  }
  
  // Handle web server requests during normal operation
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
  }
  
  unsigned long currentMillis = millis();
  
  // Check for config button press during normal operation
  static unsigned long buttonPressStart = 0;
  static bool buttonPressed = false;
  
  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    if (!buttonPressed) {
      buttonPressed = true;
      buttonPressStart = millis();
    } else if (millis() - buttonPressStart > 3000) {
      Serial.println("Config button held for 3 seconds - entering config mode");
      ESP.restart(); // Restart to enter config mode
    }
  } else {
    buttonPressed = false;
  }
  
  // Only check time every minute to reduce I2C traffic and power consumption
  if (currentMillis - lastTimeCheck >= TIME_CHECK_INTERVAL) {
    lastTimeCheck = currentMillis;
    
    DateTime now = rtc.now();
    
    // Optimized single-line time display
    Serial.printf("RTC: %04d/%02d/%02d %02d:%02d:%02d | ",
      now.year(), now.month(), now.day(), 
      now.hour(), now.minute(), now.second());
    
    if (timerSettings.enabled) {
      bool shouldAirconBeOn = isTimeInRange(
        now.hour(), now.minute(),
        timerSettings.onHour, timerSettings.onMinute,
        timerSettings.offHour, timerSettings.offMinute
      );
      
      controlAircon(shouldAirconBeOn);
      Serial.printf("Timer: ON, Aircon: %s", airconState ? "ON" : "OFF");
    } else {
      Serial.printf("Timer: DISABLED, Aircon: %s", airconState ? "ON" : "OFF");
    }
    
    // Show web server status
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf(" | Web: http://%s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println(" | Web: Offline");
    }
  }
  
  // Use smaller delay and yield to improve responsiveness
  delay(100);
}
