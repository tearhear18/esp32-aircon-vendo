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
      if (rssi > -50) signalBars = "📶📶📶📶";
      else if (rssi > -60) signalBars = "📶📶📶";
      else if (rssi > -70) signalBars = "📶📶";
      else signalBars = "📶";
      
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
  
  String html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Aircon Timer - Configuration</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { 
            font-family: 'Segoe UI', Arial, sans-serif; 
            margin: 0; 
            padding: 20px; 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
        }
        .container { 
            max-width: 800px; 
            margin: 0 auto; 
            background: white; 
            border-radius: 15px; 
            box-shadow: 0 10px 30px rgba(0,0,0,0.2);
            overflow: hidden;
        }
        .header {
            background: linear-gradient(135deg, #4CAF50 0%, #45a049 100%);
            color: white;
            padding: 30px;
            text-align: center;
        }
        .header h1 { margin: 0; font-size: 2em; }
        .header p { margin: 10px 0 0 0; opacity: 0.9; }
        
        .content { padding: 30px; }
        
        .status-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }
        
        .status-card {
            background: #f8f9fa;
            border-radius: 10px;
            padding: 20px;
            border-left: 4px solid #4CAF50;
        }
        
        .status-card h3 {
            margin: 0 0 15px 0;
            color: #333;
            font-size: 1.1em;
        }
        
        .status-item {
            display: flex;
            justify-content: space-between;
            margin: 8px 0;
            padding: 5px 0;
            border-bottom: 1px solid #eee;
        }
        
        .status-item:last-child { border-bottom: none; }
        
        .status-label { font-weight: 500; color: #666; }
        .status-value { color: #333; font-family: monospace; }
        
        .wifi-form {
            background: #f8f9fa;
            border-radius: 10px;
            padding: 25px;
            margin-top: 20px;
        }
        
        .form-group {
            margin-bottom: 20px;
        }
        
        label {
            display: block;
            margin-bottom: 8px;
            font-weight: 500;
            color: #333;
        }
        
        select, input[type="password"] {
            width: 100%;
            padding: 12px;
            border: 2px solid #ddd;
            border-radius: 8px;
            font-size: 16px;
            box-sizing: border-box;
            transition: border-color 0.3s;
        }
        
        select:focus, input:focus {
            outline: none;
            border-color: #4CAF50;
        }
        
        .btn {
            background: linear-gradient(135deg, #4CAF50 0%, #45a049 100%);
            color: white;
            padding: 15px 30px;
            border: none;
            border-radius: 8px;
            font-size: 16px;
            cursor: pointer;
            width: 100%;
            transition: transform 0.2s;
        }
        
        .btn:hover { transform: translateY(-2px); }
        
        .btn-secondary {
            background: linear-gradient(135deg, #6c757d 0%, #5a6268 100%);
            margin-top: 10px;
        }
        
        .refresh-btn {
            float: right;
            background: #17a2b8;
            color: white;
            border: none;
            padding: 5px 10px;
            border-radius: 5px;
            cursor: pointer;
            font-size: 12px;
        }
        
        .connected { color: #28a745; font-weight: bold; }
        .disconnected { color: #dc3545; font-weight: bold; }
        
        .footer {
            text-align: center;
            padding: 20px;
            color: #666;
            font-size: 14px;
            border-top: 1px solid #eee;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🌡️ ESP32 Aircon Timer</h1>
            <p>Configuration & Status Dashboard</p>
        </div>
        
        <div class="content">
            <div class="status-grid">
                <div class="status-card">
                    <h3>📶 WiFi Status</h3>
                    <div id="wifi-status">Loading...</div>
                </div>
                
                <div class="status-card">
                    <h3>🕒 Clock & Timer</h3>
                    <div id="timer-status">Loading...</div>
                </div>
                
                <div class="status-card">
                    <h3>💻 Device Info</h3>
                    <div id="device-status">Loading...</div>
                </div>
                
                <div class="status-card">
                    <h3>❄️ Aircon Control</h3>
                    <div id="aircon-status">Loading...</div>
                </div>
            </div>
            
            <div class="wifi-form">
                <h3>WiFi Configuration
                    <button class="refresh-btn" onclick="refreshNetworks()">🔄 Refresh</button>
                </h3>
                <form action="/save" method="POST">
                    <div class="form-group">
                        <label for="ssid">Available Networks:</label>
                        <select id="ssid" name="ssid" onchange="document.getElementById('manual-ssid').value = this.value" required>
                            <option value="">Select a network...</option>
                            )" + scannedNetworks + R"(
                        </select>
                    </div>
                    
                    <div class="form-group">
                        <label for="manual-ssid">Or enter manually:</label>
                        <input type="text" id="manual-ssid" placeholder="Network name (SSID)" 
                               onchange="if(this.value) document.getElementById('ssid').value = this.value">
                    </div>
                    
                    <div class="form-group">
                        <label for="password">WiFi Password:</label>
                        <input type="password" id="password" name="password" placeholder="Enter WiFi password" required>
                    </div>
                    
                    <button type="submit" class="btn">💾 Save & Connect</button>
                </form>
            </div>
        </div>
        
        <div class="footer">
            <p>Device will restart after saving new credentials</p>
            <p>Auto-refresh every 5 seconds | Last updated: <span id="last-update"></span></p>
        </div>
    </div>

    <script>
        function updateStatus() {
            fetch('/status')
                .then(response => response.json())
                .then(data => {
                    // Update WiFi status
                    const wifiHtml = `
                        <div class="status-item">
                            <span class="status-label">Status:</span>
                            <span class="status-value ${data.wifiInfo.connected ? 'connected' : 'disconnected'}">
                                ${data.wifiInfo.connected ? '✅ Connected' : '❌ Disconnected'}
                            </span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">SSID:</span>
                            <span class="status-value">${data.wifiInfo.ssid || 'None'}</span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">IP Address:</span>
                            <span class="status-value">${data.wifiInfo.ip}</span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">Signal:</span>
                            <span class="status-value">${data.wifiInfo.rssi} dBm</span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">MAC:</span>
                            <span class="status-value">${data.wifiInfo.mac}</span>
                        </div>
                    `;
                    document.getElementById('wifi-status').innerHTML = wifiHtml;
                    
                    // Update Timer status
                    const timerHtml = `
                        <div class="status-item">
                            <span class="status-label">RTC Time:</span>
                            <span class="status-value">${data.rtcInfo.time}</span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">Temperature:</span>
                            <span class="status-value">${data.rtcInfo.temperature}°C</span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">Timer:</span>
                            <span class="status-value ${data.timerInfo.enabled ? 'connected' : 'disconnected'}">
                                ${data.timerInfo.enabled ? '✅ Enabled' : '❌ Disabled'}
                            </span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">Schedule:</span>
                            <span class="status-value">${data.timerInfo.onTime} - ${data.timerInfo.offTime}</span>
                        </div>
                    `;
                    document.getElementById('timer-status').innerHTML = timerHtml;
                    
                    // Update Device status
                    const deviceHtml = `
                        <div class="status-item">
                            <span class="status-label">Chip:</span>
                            <span class="status-value">${data.deviceInfo.chipModel}</span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">Uptime:</span>
                            <span class="status-value">${Math.floor(data.deviceInfo.uptime / 3600)}h ${Math.floor((data.deviceInfo.uptime % 3600) / 60)}m</span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">Free Memory:</span>
                            <span class="status-value">${(data.deviceInfo.freeHeap / 1024).toFixed(1)} KB</span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">Flash:</span>
                            <span class="status-value">${(data.deviceInfo.flashSize / 1024 / 1024).toFixed(1)} MB</span>
                        </div>
                    `;
                    document.getElementById('device-status').innerHTML = deviceHtml;
                    
                    // Update Aircon status
                    const airconHtml = `
                        <div class="status-item">
                            <span class="status-label">Current State:</span>
                            <span class="status-value ${data.timerInfo.airconState ? 'connected' : 'disconnected'}">
                                ${data.timerInfo.airconState ? '❄️ ON' : '🔥 OFF'}
                            </span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">Auto Control:</span>
                            <span class="status-value">${data.timerInfo.enabled ? 'Active' : 'Inactive'}</span>
                        </div>
                        <div class="status-item">
                            <span class="status-label">Next Action:</span>
                            <span class="status-value">${data.timerInfo.airconState ? 'Turn OFF at ' + data.timerInfo.offTime : 'Turn ON at ' + data.timerInfo.onTime}</span>
                        </div>
                    `;
                    document.getElementById('aircon-status').innerHTML = airconHtml;
                    
                    document.getElementById('last-update').innerText = new Date().toLocaleTimeString();
                })
                .catch(error => {
                    console.error('Error updating status:', error);
                });
        }
        
        function refreshNetworks() {
            document.querySelector('.refresh-btn').innerText = '🔄 Scanning...';
            fetch('/scan')
                .then(response => response.text())
                .then(data => {
                    document.getElementById('ssid').innerHTML = '<option value="">Select a network...</option>' + data;
                    document.querySelector('.refresh-btn').innerText = '🔄 Refresh';
                })
                .catch(error => {
                    console.error('Error scanning networks:', error);
                    document.querySelector('.refresh-btn').innerText = '🔄 Error';
                });
        }
        
        // Update status every 5 seconds
        updateStatus();
        setInterval(updateStatus, 5000);
    </script>
</body>
</html>
  )";
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
    
    String html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>WiFi Saved</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; margin: 40px; background-color: #f0f0f0; text-align: center; }
        .container { max-width: 400px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        .success { background-color: #d4edda; color: #155724; border: 1px solid #c3e6cb; padding: 15px; border-radius: 5px; margin: 20px 0; }
    </style>
</head>
<body>
    <div class="container">
        <h1>✅ WiFi Configuration Saved!</h1>
        <div class="success">
            <strong>New Settings:</strong><br>
            SSID: )" + ssid + R"(<br>
            Password: )" + String("*").substring(0, password.length()) + R"(
        </div>
        <p>Device will restart in 3 seconds and attempt to connect to your WiFi network.</p>
        <p><small>If connection fails, hold the boot button for 3 seconds to enter config mode again.</small></p>
    </div>
    <script>
        setTimeout(function(){ 
            document.body.innerHTML = '<div class="container"><h1>Restarting...</h1><p>Please reconnect to your main WiFi network.</p></div>'; 
        }, 3000);
    </script>
</body>
</html>
    )";
    
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
  
  // Disconnect WiFi to save power
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi disconnected to save power");
  
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
  
  // Check if config button is pressed during startup (for 3 seconds)
  bool enterConfigMode = false;
  Serial.println("Hold BOOT button for 3 seconds to enter config mode...");
  for(int i = 0; i < 30; i++) {
    if(digitalRead(CONFIG_BUTTON_PIN) == LOW) {
      enterConfigMode = true;
      break;
    }
    delay(100);
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
    Serial.println("RTC time seems valid, skipping NTP sync");
    Serial.printf("Current RTC time: %04d-%02d-%02d %02d:%02d:%02d\n",
      now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
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
      Serial.printf("Timer: ON, Aircon: %s\n", airconState ? "ON" : "OFF");
    } else {
      Serial.println("Timer: DISABLED");
    }
  }
  
  // Use smaller delay and yield to improve responsiveness
  delay(100);
}
