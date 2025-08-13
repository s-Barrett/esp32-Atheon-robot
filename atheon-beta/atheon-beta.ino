#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_system.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

Preferences preferences;

String defaultAPSSID = "Atheon";
String defaultAPPassword = "12345678";

// Variables to hold current AP SSID and password (modifiable in device settings)
String apSSID;
String apPassword;

WebServer server(80);

const int PWMA = 1;
const int AIN1 = 2;
const int AIN2 = 4;
const int PWMB = 5;
const int BIN1 = 7;
const int BIN2 = 8;

int motorSpeed = 200;
String serialLog = "";

unsigned long startTime;
unsigned long runtimeLimit = 10 * 60 * 1000;
bool batterySafe = true;

String sequence = "";
bool runningSequence = false;
unsigned long sequenceStart;
int sequenceIndex = 0;
bool updateAvailable = false;

const char* firmwareVersion = "1.0.2";

bool apEnabled = true;

String wifiSSID = "";
String wifiPassword = "";

String latestFirmwareURL = "";

unsigned long lastUpdateCheckMillis = 0;

// --- Motor control ---

void setupMotors() {
  pinMode(PWMA, OUTPUT);
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(PWMB, OUTPUT);
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);
}

void logMessage(String message) {
  Serial.println(message);
  serialLog += message + "<br>";
  if (serialLog.length() > 3000) {
    serialLog = serialLog.substring(serialLog.length() - 3000);
  }
}

void moveForward() {
  if (!batterySafe) return;
  digitalWrite(AIN1, HIGH);
  digitalWrite(AIN2, LOW);
  digitalWrite(BIN1, HIGH);
  digitalWrite(BIN2, LOW);
  analogWrite(PWMA, motorSpeed);
  analogWrite(PWMB, motorSpeed);
  logMessage("Moving forward at speed: " + String(motorSpeed));
}

void moveBackward() {
  if (!batterySafe) return;
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, HIGH);
  digitalWrite(BIN1, LOW);
  digitalWrite(BIN2, HIGH);
  analogWrite(PWMA, motorSpeed);
  analogWrite(PWMB, motorSpeed);
  logMessage("Moving backward at speed: " + String(motorSpeed));
}

void turnLeft() {
  if (!batterySafe) return;
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, HIGH);
  digitalWrite(BIN1, HIGH);
  digitalWrite(BIN2, LOW);
  analogWrite(PWMA, motorSpeed);
  analogWrite(PWMB, motorSpeed);
  logMessage("Turning left at speed: " + String(motorSpeed));
}

void turnRight() {
  if (!batterySafe) return;
  digitalWrite(AIN1, HIGH);
  digitalWrite(AIN2, LOW);
  digitalWrite(BIN1, LOW);
  digitalWrite(BIN2, HIGH);
  analogWrite(PWMA, motorSpeed);
  analogWrite(PWMB, motorSpeed);
  logMessage("Turning right at speed: " + String(motorSpeed));
}

void stopMotors() {
  analogWrite(PWMA, 0);
  analogWrite(PWMB, 0);
  logMessage("Stopped motors");
}

void runNextCommand() {
  if (sequenceIndex >= sequence.length()) {
    stopMotors();
    runningSequence = false;
    logMessage("Sequence complete");
    return;
  }
  char cmd = sequence.charAt(sequenceIndex);
  sequenceIndex++;
  switch (cmd) {
    case 'F': moveForward(); break;
    case 'B': moveBackward(); break;
    case 'L': turnLeft(); break;
    case 'R': turnRight(); break;
    case 'S': stopMotors(); break;
  }
  sequenceStart = millis();
}

// --- Version compare & formatting ---

int compareVersions(String v1, String v2) {
  int v1Parts[3] = {0, 0, 0};
  int v2Parts[3] = {0, 0, 0};
  sscanf(v1.c_str(), "%d.%d.%d", &v1Parts[0], &v1Parts[1], &v1Parts[2]);
  sscanf(v2.c_str(), "%d.%d.%d", &v2Parts[0], &v2Parts[1], &v2Parts[2]);
  for (int i = 0; i < 3; i++) {
    if (v1Parts[i] > v2Parts[i]) return 1;
    if (v1Parts[i] < v2Parts[i]) return -1;
  }
  return 0;
}

String formatMillis(unsigned long ms) {
  unsigned long seconds = ms / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  unsigned long days = hours / 24;

  seconds %= 60;
  minutes %= 60;
  hours %= 24;

  char buf[50];
  if (days > 0)
    sprintf(buf, "%lu days %02lu:%02lu:%02lu ago", days, hours, minutes, seconds);
  else
    sprintf(buf, "%02lu:%02lu:%02lu ago", hours, minutes, seconds);

  return String(buf);
}

// --- OTA update functions ---

bool performOTAUpdate(const String& url) {
  HTTPClient http;
  http.begin(url);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setUserAgent("ESP32-OTA-Update");

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP error: %d\n", httpCode);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  if (!Update.begin(contentLength)) {
    Serial.println("Not enough space to begin OTA");
    http.end();
    return false;
  }

  WiFiClient *client = http.getStreamPtr();
  size_t written = Update.writeStream(*client);

  if (written != contentLength) {
    Serial.printf("Written only %d of %d bytes\n", written, contentLength);
  }

  if (Update.end()) {
    if (Update.isFinished()) {
      Serial.println("OTA Update finished successfully. Rebooting...");
      http.end();
      ESP.restart();
      return true;
    } else {
      Serial.println("OTA Update not finished.");
    }
  } else {
    Serial.printf("OTA Update failed. Error #: %d\n", Update.getError());
  }

  http.end();
  return false;
}

bool fetchLatestFirmwareURL() {
  const char* apiURL = "https://api.github.com/repos/s-Barrett/esp32-Atheon-robot/releases/latest";
  HTTPClient http;
  http.begin(apiURL);
  http.setUserAgent("ESP32-GitHub-OTA");

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("Failed to fetch GitHub API, HTTP code: %d\n", httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  const size_t capacity = 32 * 1024;
  DynamicJsonDocument doc(capacity);

  auto err = deserializeJson(doc, payload);
  if (err) {
    Serial.print(F("JSON parse error: "));
    Serial.println(err.f_str());
    return false;
  }

  JsonArray assets = doc["assets"].as<JsonArray>();
  for (JsonObject asset : assets) {
    const char* name = asset["name"];
    if (name && String(name).endsWith(".bin")) {
      latestFirmwareURL = String(asset["browser_download_url"].as<const char*>());
      Serial.println("Firmware URL from GitHub API: " + latestFirmwareURL);
      return true;
    }
  }
  Serial.println("No .bin asset found in latest release.");
  return false;
}

void checkForUpdateFromGitHub(bool performUpdate = false) {
  String versionURL = "https://raw.githubusercontent.com/s-Barrett/esp32-Atheon-robot/main/version";

  HTTPClient http;
  http.begin(versionURL);
  http.setUserAgent("ESP32-GitHub-OTA");

  int httpCode = http.GET();
  if (httpCode == 200) {
    String remoteVersion = http.getString();
    remoteVersion.trim();

    Serial.println("Remote firmware version: " + remoteVersion);
    Serial.println("Current firmware version: " + String(firmwareVersion));

    if (compareVersions(remoteVersion, String(firmwareVersion)) > 0) {
      updateAvailable = true;
      Serial.println("New update available: " + remoteVersion);

      if (performUpdate) {
        if (!fetchLatestFirmwareURL()) {
          Serial.println("Failed to get latest firmware URL, aborting update.");
          return;
        }
        bool success = performOTAUpdate(latestFirmwareURL);
        if (!success) {
          Serial.println("OTA Update failed.");
        }
      }
    } else {
      updateAvailable = false;
      Serial.println("No new update available.");
    }
  } else {
    updateAvailable = false;
    Serial.printf("Failed to fetch version file. HTTP code: %d\n", httpCode);
  }

  http.end();
  lastUpdateCheckMillis = millis();
}

// --- Wi-Fi management ---

void startAP() {
  WiFi.mode(WIFI_AP_STA);
  bool result = WiFi.softAP(apSSID.c_str(), apPassword.c_str());
  if(result) {
    Serial.println("Hotspot started: " + apSSID);
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Failed to start Hotspot");
  }
  apEnabled = true;
}

void stopAP() {
  WiFi.softAPdisconnect(true);
  apEnabled = false;
  Serial.println("Hotspot stopped");
}

void connectToWiFi(String newSSID, String newPass) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect();
    delay(100);
  }
  WiFi.mode(WIFI_AP_STA); // Keep AP + STA mode so hotspot stays visible
  WiFi.setHostname("Atheon");
  WiFi.begin(newSSID.c_str(), newPass.c_str());

  Serial.print("Connecting to WiFi ");
  Serial.print(newSSID);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());
    wifiSSID = newSSID;
    wifiPassword = newPass;

    preferences.begin("wifiCreds", false);
    preferences.putString("ssid", wifiSSID);
    preferences.putString("pass", wifiPassword);
    preferences.end();

    // Keep AP running even after connection

  } else {
    Serial.println("Failed to connect to WiFi");
    wifiSSID = "";
    wifiPassword = "";

    if (!apEnabled) {
      startAP();
    }

    WiFi.scanDelete();
    WiFi.scanNetworks(true);
  }
}

String getWiFiModeStatus() {
  if (apEnabled && WiFi.status() == WL_CONNECTED) return "AP + Station (Internet connected)";
  else if (apEnabled) return "AP only (No internet)";
  else if (WiFi.status() == WL_CONNECTED) return "Station only (Internet connected)";
  else return "Disconnected";
}

// --- HTML Templates and Styling ---

String HTMLHeader(String title) {
  return String(R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
  <title>)rawliteral" + title + R"rawliteral(</title>
  <link rel="manifest" href="/manifest.json">
  <meta name="theme-color" content="#203764">
  <meta name="apple-mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
  <style>
    body {
      background: #f9fbfc;
      color: #203764;
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      text-align: center;
      font-size: 110%;
      margin: 0;
      padding: 15px;
      -webkit-font-smoothing: antialiased;
      -moz-osx-font-smoothing: grayscale;
    }
    button {
      background-color: #203764;
      border: none;
      color: white;
      margin: 10px;
      padding: 14px 28px;
      font-size: 115%;
      border-radius: 6px;
      cursor: pointer;
      box-shadow: 0 4px 8px rgba(32,55,100,0.3);
      transition: background-color 0.3s ease;
    }
    button:hover {
      background-color: #415a8c;
    }
    input[type=range] {
      width: 90%;
      margin: 10px 0;
    }
    input[type=text], input[type=password], select {
      width: 90%;
      max-width: 400px;
      padding: 10px;
      font-size: 105%;
      border: 1.5px solid #203764;
      border-radius: 6px;
      margin: 8px 0;
      box-sizing: border-box;
      outline: none;
      transition: border-color 0.3s ease;
    }
    input[type=text]:focus, input[type=password]:focus, select:focus {
      border-color: #415a8c;
    }
    .section {
      margin-top: 25px;
      border: 1px solid #c7d0db;
      padding: 20px;
      width: 90%;
      max-width: 500px;
      margin-left: auto;
      margin-right: auto;
      background-color: #ffffff;
      border-radius: 12px;
      box-shadow: 0 4px 12px rgba(32,55,100,0.15);
      text-align: left;
      color: #203764;
    }
    nav {
      margin-bottom: 15px;
    }
    nav button {
      margin: 5px;
      padding: 10px 20px;
      font-size: 100%;
      box-shadow: none;
      border-radius: 5px;
      background-color: transparent;
      color: #203764;
      border: 2px solid #203764;
      transition: all 0.3s ease;
    }
    nav button:hover {
      background-color: #203764;
      color: white;
      box-shadow: 0 4px 8px rgba(32,55,100,0.3);
    }
    img.logo {
      max-width: 230px;
      height: auto;
      margin: 15px auto 20px auto;
      display: block;
      filter: drop-shadow(0 0 2px rgba(32,55,100,0.4));
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 110%;
      color: #203764;
    }
    table td {
      padding: 10px 8px;
      border-bottom: 1px solid #c7d0db;
    }
    p small {
      color: #7a8ba6;
    }
  </style>
</head>
<body>
)rawliteral");
}

String HTMLFooter() {
  return String(R"rawliteral(
<script>
  function updateSpeed(val) {
    document.getElementById('speedVal').innerText = val;
    fetch('/speed?value=' + val);
  }
  function runSeq() {
    const seq = document.getElementById('seq').value;
    fetch('/runseq?value=' + seq);
  }
  function checkForUpdates() {
    fetch('/checkupdate')
      .then(response => response.text())
      .then(text => {
        document.getElementById('updateStatus').innerText = text;
        if (text.includes("Update available")) {
          location.reload();
        }
      })
      .catch(() => {
        document.getElementById('updateStatus').innerText = 'Error checking updates';
      });
  }
  function updateLastCheckTimer() {
    const el = document.getElementById('lastCheckTimer');
    if (!el) return;
    const lastCheck = parseInt(el.getAttribute('data-lastcheck'));
    if (!lastCheck) return;
    const elapsed = Math.floor((Date.now() - lastCheck) / 1000);
    let text = '';
    if (elapsed < 60) text = `${elapsed} seconds ago`;
    else if (elapsed < 3600) text = `${Math.floor(elapsed/60)} minutes ago`;
    else if (elapsed < 86400) text = `${Math.floor(elapsed/3600)} hours ago`;
    else text = `${Math.floor(elapsed/86400)} days ago`;
    el.textContent = text;
  }
  setInterval(updateLastCheckTimer, 1000);
</script>
</body>
</html>
  )rawliteral");
}

// --- Handlers ---

void handleRoot() {
  String updateSection = "";
  if (updateAvailable) {
    updateSection += "<p style='color:green;'>Update available!</p>";
    updateSection += "<a href=\"/autoupdate\"><button>Install Update Now</button></a>";
  } else {
    updateSection += "<p>No updates available.</p>";
  }

  String html = HTMLHeader("Atheon Control Panel");

  if (WiFi.status() == WL_CONNECTED) {
    html += "<img class='logo' src='https://cdn.shopify.com/s/files/1/0978/9253/2488/files/cool_logo.png?v=1753436023' alt='Atheon Logo'>";
  }

  html += R"rawliteral(
    <h1>Atheon Control Panel</h1>
    <nav>
      <a href="/status"><button>Status Panel</button></a>
      <a href="/serial"><button>Serial Terminal</button></a>
      <a href="/wifi"><button>Wi-Fi Settings</button></a>
      <a href="/devicesettings"><button>Device Settings</button></a>
    </nav>
    <div class='section'>
      <button onclick="fetch('/forward')">Forward</button>
      <button onclick="fetch('/back')">Back</button><br>
      <button onclick="fetch('/left')">Left</button>
      <button onclick="fetch('/right')">Right</button><br>
      <button onclick="fetch('/stop')">Stop</button>
    </div>
    <div class='section'>
      <label for='speed'>Speed:</label>
      <input type='range' id='speed' min='0' max='255' value='200' onchange='updateSpeed(this.value)'>
      <br><span id='speedVal'>200</span>
    </div>
    <div class='section'>
      <h3>Sequence (F=forward, B=backward, L=left, R=right, S=stop):</h3>
      <input id='seq' placeholder='e.g. FFRLS' style='width:200px'>
      <button onclick='runSeq()'>Run</button>
    </div>
    <div class='section'>
      <h3>Firmware Update</h3>
  )rawliteral" + updateSection + R"rawliteral(
      <button onclick="checkForUpdates()">Check for Updates</button>
      <p id="updateStatus"></p>
      <p><small>Last update check: <span id="lastCheckTimer" data-lastcheck=)rawliteral" + String(lastUpdateCheckMillis) + R"rawliteral(></span></small></p>
    </div>
    <div class='section'>
      <h3>Manual Update Upload</h3>
      <form method='POST' action='/update' enctype='multipart/form-data'>
        <input type='file' name='update'>
        <input type='submit' value='Upload & Update'>
      </form>
    </div>
  )rawliteral";

  html += HTMLFooter();

  server.send(200, "text/html", html);
}

void handleStatus() {
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t uptime = (millis() / 1000);

  String html = HTMLHeader("Status Panel");
  html += "<h2>System Status</h2><table>";
  html += "<tr><td>Uptime:</td><td>" + String(uptime) + " sec</td></tr>";
  html += "<tr><td>Free Memory:</td><td>" + String(freeHeap) + " bytes</td></tr>";
  html += "<tr><td>Motor Speed:</td><td>" + String(motorSpeed) + "/255</td></tr>";
  html += "<tr><td>Battery Safe:</td><td>" + String(batterySafe ? "YES" : "NO") + "</td></tr>";
  html += "</table><br><a href='/'><button>Back</button></a>";
  html += HTMLFooter();

  server.send(200, "text/html", html);
}

void handleSerial() {
  String html = HTMLHeader("Serial Output");
  html += R"rawliteral(
    <h2>Serial Output</h2>
    <div id='serial' style='background:#111;color:#0f0;width:90%;margin:auto;border:1px solid #ccc;padding:10px;height:300px;overflow:auto;text-align:left;'></div>
    <br><a href='/'><button>Back</button></a>
    <script>
      function updateSerial(){
        fetch('/serialtext')
          .then(r => r.text())
          .then(t => {document.getElementById('serial').innerHTML = t;});
      }
      setInterval(updateSerial,1000);
    </script>
  )rawliteral";
  html += HTMLFooter();

  server.send(200, "text/html", html);
}

void handleWiFiSettings() {
  String html = HTMLHeader("Wi-Fi Settings");
  html += "<h2>Wi-Fi Settings</h2>";
  html += "<p><b>Current Mode:</b> " + getWiFiModeStatus() + "</p>";

  html += R"rawliteral(
    <button onclick="scanNetworks()">Scan Wi-Fi Networks</button>
    <select id="ssidList" onchange="ssidChanged()" style="width:90%;max-width:400px;margin-top:10px;padding:10px;font-size:110%;border:1.5px solid #203764;border-radius:6px;">
      <option value="">-- Select Wi-Fi Network --</option>
    </select>
    <form method="POST" action="/wificonnect" style="margin-top:15px;">
      <label for="password">Password:</label>
      <input type="password" id="password" name="password" placeholder="Enter Wi-Fi Password" required>
      <input type="hidden" id="ssid" name="ssid" value="">
      <button type="submit" style="margin-top:15px;">Connect to Wi-Fi</button>
    </form>
    <br>
    <form method="POST" action="/toggleap">
      <button type="submit">)rawliteral" + String(apEnabled ? "Disable" : "Enable") + R"rawliteral( Hotspot</button>
    </form>
    <br>
    <a href="/"><button>Back to Control Panel</button></a>
  )rawliteral";

  html += HTMLFooter();

  html += R"rawliteral(
  <script>
    function scanNetworks() {
      const ssidSelect = document.getElementById('ssidList');
      ssidSelect.innerHTML = '<option>Scanning...</option>';
      fetch('/wifiscan')
        .then(response => response.json())
        .then(networks => {
          ssidSelect.innerHTML = '<option value="">-- Select Wi-Fi Network --</option>';
          networks.forEach(ssid => {
            let option = document.createElement('option');
            option.value = ssid;
            option.textContent = ssid;
            ssidSelect.appendChild(option);
          });
        })
        .catch(() => {
          ssidSelect.innerHTML = '<option>Error scanning networks</option>';
        });
    }
    function ssidChanged() {
      const ssidSelect = document.getElementById('ssidList');
      const selectedSSID = ssidSelect.value;
      document.getElementById('ssid').value = selectedSSID;
    }
    window.onload = scanNetworks;
  </script>
  )rawliteral";

  server.send(200, "text/html", html);
}

void handleWiFiConnect() {
  if (server.hasArg("ssid") && server.hasArg("password")) {
    String newSSID = server.arg("ssid");
    String newPass = server.arg("password");
    logMessage("Connecting to Wi-Fi SSID: " + newSSID);
    connectToWiFi(newSSID, newPass);
  }
  server.sendHeader("Location", "/wifi");
  server.send(303);
}

void handleToggleAP() {
  if (apEnabled) {
    stopAP();
  } else {
    startAP();
  }
  server.sendHeader("Location", "/wifi");
  server.send(303);
}

void handleWiFiScan() {
  int n = WiFi.scanNetworks();
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; ++i) {
    arr.add(WiFi.SSID(i));
  }
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

// --- New: Device Settings Page ---

void handleDeviceSettings() {
  String html = HTMLHeader("Device Settings");
  html += "<h2>Device Settings</h2>";
  html += "<form method='POST' action='/saveDeviceSettings' class='section'>";
  html += "<label for='apssid'>AP SSID:</label><br>";
  html += "<input type='text' id='apssid' name='apssid' value='" + apSSID + "' required><br><br>";
  html += "<label for='appassword'>AP Password:</label><br>";
  html += "<input type='password' id='appassword' name='appassword' value='" + apPassword + "' minlength='8' required><br><br>";
  html += "<button type='submit'>Save Settings</button>";
  html += "</form>";
  html += "<br><a href='/'><button>Back to Control Panel</button></a>";
  html += HTMLFooter();
  server.send(200, "text/html", html);
}

void handleSaveDeviceSettings() {
  if (server.hasArg("apssid") && server.hasArg("appassword")) {
    String newSSID = server.arg("apssid");
    String newPass = server.arg("appassword");

    if (newSSID.length() >= 1 && newPass.length() >= 8) {
      apSSID = newSSID;
      apPassword = newPass;

      preferences.begin("deviceSettings", false);
      preferences.putString("apssid", apSSID);
      preferences.putString("appass", apPassword);
      preferences.end();

      logMessage("Device Settings updated: AP SSID=" + apSSID);

      // Restart AP with new settings immediately
      if (apEnabled) {
        stopAP();
      }
      startAP();
    }
  }
  server.sendHeader("Location", "/devicesettings");
  server.send(303);
}

void handleManifest() {
  const char* manifestJson = R"json(
  {
    "name": "Atheon Robot",
    "short_name": "Atheon",
    "start_url": "/",
    "display": "standalone",
    "background_color": "#f9fbfc",
    "theme_color": "#203764",
    "icons": [
      {
        "src": "icon-192.png",
        "sizes": "192x192",
        "type": "image/png"
      },
      {
        "src": "icon-512.png",
        "sizes": "512x512",
        "type": "image/png"
      }
    ]
  }
  )json";
  server.send(200, "application/json", manifestJson);
}

// --- Setup and loop ---

void setup() {
  Serial.begin(115200);

  preferences.begin("deviceSettings", false);
  apSSID = preferences.getString("apssid", defaultAPSSID);
  apPassword = preferences.getString("appass", defaultAPPassword);
  preferences.end();

  preferences.begin("wifiCreds", false);
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("pass", "");
  preferences.end();

  if (wifiSSID.length() > 0 && wifiPassword.length() > 0) {
    connectToWiFi(wifiSSID, wifiPassword);
  } else {
    startAP();
  }

  setupMotors();
  startTime = millis();

  logMessage("Wi-Fi started. Connect to AP: " + apSSID);

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/serial", handleSerial);
  server.on("/serialtext", []() {
    server.send(200, "text/plain", serialLog);
  });
  server.on("/wifi", handleWiFiSettings);
  server.on("/wificonnect", HTTP_POST, handleWiFiConnect);
  server.on("/toggleap", HTTP_POST, handleToggleAP);
  server.on("/wifiscan", handleWiFiScan);
  server.on("/devicesettings", handleDeviceSettings);
  server.on("/saveDeviceSettings", HTTP_POST, handleSaveDeviceSettings);
  server.on("/manifest.json", handleManifest);

  server.on("/forward", []() { moveForward(); server.send(200, "text/plain", "Moving forward"); });
  server.on("/back", []() { moveBackward(); server.send(200, "text/plain", "Moving backward"); });
  server.on("/left", []() { turnLeft(); server.send(200, "text/plain", "Turning left"); });
  server.on("/right", []() { turnRight(); server.send(200, "text/plain", "Turning right"); });
  server.on("/stop", []() { stopMotors(); server.send(200, "text/plain", "Stopped"); });
  server.on("/speed", []() {
    if (server.hasArg("value")) {
      motorSpeed = server.arg("value").toInt();
      logMessage("Speed set to: " + String(motorSpeed));
      server.send(200, "text/plain", "Speed updated");
    }
  });
  server.on("/runseq", []() {
    if (server.hasArg("value")) {
      sequence = server.arg("value");
      sequenceIndex = 0;
      runningSequence = true;
      logMessage("Running sequence: " + sequence);
      runNextCommand();
      server.send(200, "text/plain", "Sequence started");
    }
  });

  // OTA update routes
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", Update.hasError() ? "Update Failed" : "Update Successful. Rebooting...");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.on("/autoupdate", []() {
    if (updateAvailable) {
      logMessage("Starting OTA update...");
      checkForUpdateFromGitHub(true);
      server.send(200, "text/plain", "Update started, device will reboot if successful.");
    } else {
      server.send(200, "text/plain", "No update available.");
    }
  });

  server.on("/checkupdate", []() {
    checkForUpdateFromGitHub(false);
    if (updateAvailable) {
      server.send(200, "text/plain", "Update available!");
    } else {
      server.send(200, "text/plain", "No update available.");
    }
  });

  server.begin();
  logMessage("HTTP server started");

  checkForUpdateFromGitHub(false);
}

void loop() {
  server.handleClient();
  if (millis() - startTime > runtimeLimit && batterySafe) {
    batterySafe = false;
    stopMotors();
    logMessage("Battery runtime limit reached. Motors disabled.");
  }
  if (runningSequence && millis() - sequenceStart > 1000) {
    runNextCommand();
  }
}
