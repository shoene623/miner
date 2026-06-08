#define ESP_DRD_USE_SPIFFS true

// Include Libraries
//#include ".h"

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>

#include "wManager.h"
#include "monitor.h"
#include "drivers/displays/display.h"
#include "drivers/storage/SDCard.h"
#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"
#include "mining.h"
#include "timeconst.h"

#include <ArduinoJson.h>
#include <esp_flash.h>


// Flag for saving data
bool shouldSaveConfig = false;

// Variables to hold data from custom textboxes
TSettings Settings;

// Define WiFiManager Object
WiFiManager wm;
extern monitor_data mMonitor;

nvMemory nvMem;
WebServer settingsWebServer(80);
bool settingsWebServerStarted = false;

extern SDCard SDCrd;

static String sCachedStatusJson;
static unsigned long sCachedStatusMs = 0;

static String htmlEscape(const String &value) {
    String escaped = value;
    escaped.replace("&", "&amp;");
    escaped.replace("<", "&lt;");
    escaped.replace(">", "&gt;");
    escaped.replace("\"", "&quot;");
    escaped.replace("'", "&#39;");
    return escaped;
}

static bool isInvalidMetric(const String &value) {
    if (value.isEmpty()) return true;
    String normalized = value;
    normalized.trim();
    normalized.toLowerCase();
    return normalized == "nan" || normalized == "-nan" || normalized == "inf" || normalized == "-inf";
}

static String sanitizeMetric(const String &value, const String &fallback = "--") {
    return isInvalidMetric(value) ? fallback : value;
}

static String getUiIpAddress() {
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    IPAddress apIp = WiFi.softAPIP();
    if (apIp[0] != 0 || apIp[1] != 0 || apIp[2] != 0 || apIp[3] != 0) {
        return apIp.toString();
    }
    return String("--");
}

static void normalizePoolEndpoint(String &poolAddress, int &poolPort) {
    String input = poolAddress;
    input.trim();
    input.replace(" ", "");

    if (input.startsWith("stratum+tcp://")) {
        input = input.substring(strlen("stratum+tcp://"));
    } else if (input.startsWith("tcp://")) {
        input = input.substring(strlen("tcp://"));
    }

    // Strip trailing path/query fragments if pasted from a full URI.
    int slashPos = input.indexOf('/');
    if (slashPos >= 0) {
        input = input.substring(0, slashPos);
    }
    int queryPos = input.indexOf('?');
    if (queryPos >= 0) {
        input = input.substring(0, queryPos);
    }

    int colonPos = input.lastIndexOf(':');
    if (colonPos > 0 && colonPos < (int)input.length() - 1) {
        String portPart = input.substring(colonPos + 1);
        bool digitsOnly = true;
        for (int i = 0; i < (int)portPart.length(); ++i) {
            if (!isDigit(portPart[i])) {
                digitsOnly = false;
                break;
            }
        }

        if (digitsOnly) {
            int parsedPort = portPart.toInt();
            if (parsedPort >= 1 && parsedPort <= 65535) {
                poolPort = parsedPort;
                input = input.substring(0, colonPos);
            }
        }
    }

    input.trim();
    if (!input.isEmpty()) {
        poolAddress = input;
    }
}

static String renderMonitorPageHtml() {
    String html;
    html.reserve(4200);
    html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>NerdMiner Monitor</title>";
    html += "<style>body{font-family:Arial,sans-serif;margin:0;background:#0d1311;color:#e8f7ef}main{max-width:980px;margin:0 auto;padding:14px}.top{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:12px}.btn{display:inline-block;padding:8px 12px;border-radius:8px;border:1px solid #2c4b3d;background:#183127;color:#dfffee;text-decoration:none;font-weight:700}.btn.p{background:#2a9c63;border-color:#39be79;color:#fff}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px}.card{background:#12211a;border:1px solid #2f4d3f;border-radius:10px;padding:10px}.k{font-size:11px;color:#9fc7b4;text-transform:uppercase}.v{margin-top:6px;font-size:18px;font-weight:700;word-break:break-word}.ok{color:#72f5ad}.warn{color:#ffd280}.mono{font-family:Consolas,monospace}.foot{margin-top:12px;color:#9cb9ad;font-size:12px}</style></head><body>";
    html += "<main><div class='top'><a class='btn' href='/settings'>Settings</a><a class='btn' href='/api/status'>JSON</a><button class='btn p' id='refresh'>Refresh</button></div>";
    html += "<div class='grid'>";
    html += "<div class='card'><div class='k'>Device IP</div><div class='v mono' id='ip'>--</div></div>";
    html += "<div class='card'><div class='k'>Hashrate</div><div class='v' id='hashrate'>--</div></div>";
    html += "<div class='card'><div class='k'>Shares / Valids</div><div class='v' id='shares'>--</div></div>";
    html += "<div class='card'><div class='k'>Submit Attempts</div><div class='v' id='submits'>--</div></div>";
    html += "<div class='card'><div class='k'>Pool Difficulty</div><div class='v' id='pdiff'>--</div></div>";
    html += "<div class='card'><div class='k'>Workers</div><div class='v' id='workers'>--</div></div>";
    html += "<div class='card'><div class='k'>Best Diff Local</div><div class='v' id='bestLocal'>--</div></div>";
    html += "<div class='card'><div class='k'>Best Diff Pool</div><div class='v' id='bestPool'>--</div></div>";
    html += "<div class='card'><div class='k'>Uptime</div><div class='v' id='uptime'>--</div></div>";
    html += "<div class='card'><div class='k'>Pool API URL</div><div class='v mono' id='api'>--</div></div>";
    html += "</div><div class='foot' id='stamp'>Waiting for data...</div></main>";
    html += "<script>let refreshing=false;async function refresh(){if(refreshing)return;refreshing=true;try{const r=await fetch('/api/status',{cache:'no-store'});const d=await r.json();const m=d.mining||{},p=d.pool||{};document.getElementById('ip').textContent=d.ip||'--';document.getElementById('hashrate').textContent=(m.hashrate||'0.00')+' KH/s';document.getElementById('shares').textContent=(m.shares||'0')+' / '+(m.valids||'0');document.getElementById('submits').textContent=m.submitAttempts||'0';document.getElementById('pdiff').textContent=m.poolDifficulty||'--';document.getElementById('workers').textContent=(p.workersCount||0)+' / '+(p.workersHash||'--');document.getElementById('bestLocal').textContent=m.bestDiff||'--';document.getElementById('bestPool').textContent=p.bestDifficulty||'--';document.getElementById('uptime').textContent=m.uptime||'--';document.getElementById('api').textContent=p.apiUrl||'--';document.getElementById('stamp').textContent='Updated '+new Date().toLocaleTimeString();}catch(e){document.getElementById('stamp').textContent='Fetch failed: '+e;}finally{refreshing=false;}}document.getElementById('refresh').addEventListener('click',refresh);refresh();</script></body></html>";
    return html;
}

static String renderSettingsPageHtml(const String &message = "") {
    String html;
    html.reserve(5400);
    html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>NerdMiner Settings</title>";
    html += "<style>body{font-family:Arial,sans-serif;margin:0;background:#111;color:#eee}main{max-width:680px;margin:20px auto;padding:20px;background:#1a1a1a;border-radius:12px}h1{margin-top:0}label{display:block;margin:14px 0 6px}input[type=text]{width:100%;padding:10px;border:1px solid #444;border-radius:8px;background:#0f0f0f;color:#eee}button{margin-top:18px;padding:10px 14px;background:#008c7a;color:#fff;border:0;border-radius:8px;font-weight:700;cursor:pointer}.msg{margin:10px 0;padding:8px 10px;background:#1f3c2e;border-left:4px solid #37c978;border-radius:6px}.meta{color:#aaa;font-size:12px}</style></head><body>";
    html += "<main><h1>NerdMiner Settings</h1>";
    html += "<div class='meta'>Device IP: ";
    html += getUiIpAddress();
    html += "</div>";
    if (!message.isEmpty()) {
        html += "<div class='msg'>" + message + "</div>";
    }
    html += "<form method='POST' action='/save'>";
    html += "<label for='poolAddress'>Pool URL</label>";
    html += "<input id='poolAddress' name='poolAddress' type='text' maxlength='79' value='" + htmlEscape(Settings.PoolAddress) + "'>";
    html += "<label for='poolPort'>Pool Port</label>";
    html += "<input id='poolPort' name='poolPort' type='text' maxlength='6' value='" + String(Settings.PoolPort) + "'>";
    html += "<label for='poolPassword'>Pool Password</label>";
    html += "<input id='poolPassword' name='poolPassword' type='text' maxlength='79' value='" + htmlEscape(String(Settings.PoolPassword)) + "'>";
    html += "<label for='btcWallet'>BTC Wallet</label>";
    html += "<input id='btcWallet' name='btcWallet' type='text' maxlength='79' value='" + htmlEscape(String(Settings.BtcWallet)) + "'>";
    html += "<label for='timezone'>Timezone (UTC offset)</label>";
    html += "<input id='timezone' name='timezone' type='text' maxlength='4' value='" + String(Settings.Timezone) + "'>";
    html += "<label><input name='rearLedEnabled' type='checkbox' value='1'";
    if (Settings.rearLedEnabled) {
        html += " checked";
    }
    html += "> Rear LED enabled</label>";
    html += "<button type='submit'>Save Settings</button></form>";
    html += "</main></body></html>";
    return html;
}

static void handleSettingsRoot() {
    settingsWebServer.send(200, "text/html", renderMonitorPageHtml());
}

static void handleSettingsPage() {
    settingsWebServer.send(200, "text/html", renderSettingsPageHtml());
}

static void handleSettingsSave() {
    if (settingsWebServer.hasArg("poolAddress")) {
        String address = settingsWebServer.arg("poolAddress");
        address.trim();
        if (!address.isEmpty()) Settings.PoolAddress = address;
    }

    if (settingsWebServer.hasArg("poolPort")) {
        int parsed = settingsWebServer.arg("poolPort").toInt();
        if (parsed >= 1 && parsed <= 65535) {
            Settings.PoolPort = parsed;
        }
    }

    normalizePoolEndpoint(Settings.PoolAddress, Settings.PoolPort);

    if (settingsWebServer.hasArg("poolPassword")) {
        String pass = settingsWebServer.arg("poolPassword");
        pass.trim();
        strncpy(Settings.PoolPassword, pass.c_str(), sizeof(Settings.PoolPassword) - 1);
        Settings.PoolPassword[sizeof(Settings.PoolPassword) - 1] = '\0';
    }

    if (settingsWebServer.hasArg("btcWallet")) {
        String wallet = settingsWebServer.arg("btcWallet");
        wallet.trim();
        strncpy(Settings.BtcWallet, wallet.c_str(), sizeof(Settings.BtcWallet) - 1);
        Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';
    }

    if (settingsWebServer.hasArg("timezone")) {
        int tz = settingsWebServer.arg("timezone").toInt();
        if (tz < -12) tz = -12;
        if (tz > 14) tz = 14;
        Settings.Timezone = tz;
    }

    Settings.rearLedEnabled = settingsWebServer.hasArg("rearLedEnabled");
    nvMem.saveConfig(&Settings);
    settingsWebServer.send(200, "text/html", renderSettingsPageHtml("Settings saved."));
}

static void handleStatusJson() {
    // Keep status endpoint lightweight to avoid blocking the web loop.
    const unsigned long nowMs = millis();
    if (!sCachedStatusJson.isEmpty() && (nowMs - sCachedStatusMs) < 1000) {
        settingsWebServer.send(200, "application/json", sCachedStatusJson);
        return;
    }

    const unsigned long elapsed = 1000;
    mining_data miningData = getMiningData(elapsed);

    DynamicJsonDocument doc(768);
    doc["ip"] = getUiIpAddress();

    JsonObject mining = doc.createNestedObject("mining");
    mining["hashrate"] = sanitizeMetric(miningData.currentHashRate, "0.00");
    mining["shares"] = miningData.completedShares;
    mining["valids"] = miningData.valids;
    mining["uptime"] = miningData.timeMining;
    mining["bestDiff"] = miningData.bestDiff;
    mining["submitAttempts"] = "--";
    mining["poolDifficulty"] = "--";

    JsonObject pool = doc.createNestedObject("pool");
    pool["workersCount"] = 0;
    pool["workersHash"] = "--";
    pool["bestDifficulty"] = "--";
    pool["apiUrl"] = getPoolAPIUrl();

    String payload;
    serializeJson(doc, payload);
    sCachedStatusJson = payload;
    sCachedStatusMs = nowMs;
    settingsWebServer.send(200, "application/json", sCachedStatusJson);
}

static void ensureSettingsWebServerStarted() {
    if (settingsWebServerStarted) {
        return;
    }

    settingsWebServer.on("/", HTTP_GET, handleSettingsRoot);
    settingsWebServer.on("/settings", HTTP_GET, handleSettingsPage);
    settingsWebServer.on("/save", HTTP_POST, handleSettingsSave);
    settingsWebServer.on("/api/status", HTTP_GET, handleStatusJson);
    settingsWebServer.begin();
    settingsWebServerStarted = true;
    Serial.println("Settings web server started on http://" + getUiIpAddress());
}

String readCustomAPName() {
    Serial.println("DEBUG: Attempting to read custom AP name from flash at 0x3F0000...");
    
    // Leer directamente desde flash
    const size_t DATA_SIZE = 128;
    uint8_t buffer[DATA_SIZE];
    memset(buffer, 0, DATA_SIZE); // Clear buffer
    
    // Leer desde 0x3F0000
    esp_err_t result = esp_flash_read(NULL, buffer, 0x3F0000, DATA_SIZE);
    if (result != ESP_OK) {
        Serial.printf("DEBUG: Flash read error: %s\n", esp_err_to_name(result));
        return "";
    }
    
    Serial.println("DEBUG: Successfully read from flash");
    String data = String((char*)buffer);
    
    // Debug: show raw data read
    Serial.printf("DEBUG: Raw flash data: '%s'\n", data.c_str());
    
    if (data.startsWith("WEBFLASHER_CONFIG:")) {
        Serial.println("DEBUG: Found WEBFLASHER_CONFIG marker");
        String jsonPart = data.substring(18); // Despu+�s del marcador "WEBFLASHER_CONFIG:"
        
        Serial.printf("DEBUG: JSON part: '%s'\n", jsonPart.c_str());
        
        DynamicJsonDocument doc(256);
        DeserializationError error = deserializeJson(doc, jsonPart);
        
        if (error == DeserializationError::Ok) {
            Serial.println("DEBUG: JSON parsed successfully");
            
            if (doc.containsKey("apname")) {
                String customAP = doc["apname"].as<String>();
                customAP.trim();
                
                if (customAP.length() > 0 && customAP.length() < 32) {
                    Serial.printf("G�� Custom AP name from webflasher: %s\n", customAP.c_str());
                    return customAP;
                } else {
                    Serial.printf("DEBUG: AP name invalid length: %d\n", customAP.length());
                }
            } else {
                Serial.println("DEBUG: 'apname' key not found in JSON");
            }
        } else {
            Serial.printf("DEBUG: JSON parse error: %s\n", error.c_str());
        }
    } else {
        Serial.println("DEBUG: WEBFLASHER_CONFIG marker not found - no custom config");
    }
    
    Serial.println("DEBUG: Using default AP name");
    return "";
}

void saveConfigCallback()
// Callback notifying us of the need to save configuration
{
    Serial.println("Should save config");
    shouldSaveConfig = true;    
    //wm.setConfigPortalBlocking(false);
}

/* void saveParamsCallback()
// Callback notifying us of the need to save configuration
{
    Serial.println("Should save config");
    shouldSaveConfig = true;
    nvMem.saveConfig(&Settings);
} */

void configModeCallback(WiFiManager* myWiFiManager)
// Called when config mode launched
{
    Serial.println("Entered Configuration Mode");
    drawSetupScreen();
    Serial.print("Config SSID: ");
    Serial.println(myWiFiManager->getConfigPortalSSID());

    Serial.print("Config IP Address: ");
    Serial.println(WiFi.softAPIP());
}

void reset_configuration()
{
    Serial.println("Erasing Config, restarting");
    nvMem.deleteConfig();
    resetStat();
    wm.resetSettings();
    ESP.restart();
}

void init_WifiManager()
{
#ifdef MONITOR_SPEED
    Serial.begin(MONITOR_SPEED);
#else
    Serial.begin(115200);
#endif //MONITOR_SPEED
    //Serial.setTxTimeoutMs(10);
    
    // Check for custom AP name from flasher config, otherwise use default
    String customAPName = readCustomAPName();
    const char* apName = customAPName.length() > 0 ? customAPName.c_str() : DEFAULT_SSID;

    //Init pin 15 to eneble 5V external power (LilyGo bug)
#ifdef PIN_ENABLE5V
    pinMode(PIN_ENABLE5V, OUTPUT);
    digitalWrite(PIN_ENABLE5V, HIGH);
#endif

    // Change to true when testing to force configuration every time we run
    bool forceConfig = false;

#if defined(PIN_BUTTON_2)
    // Check if button2 is pressed to enter configMode with actual configuration
    if (!digitalRead(PIN_BUTTON_2)) {
        Serial.println(F("Button pressed to force start config mode"));
        forceConfig = true;
        wm.setBreakAfterConfig(true); //Set to detect config edition and save
    }
#endif
    // Explicitly set WiFi mode
    WiFi.mode(WIFI_STA);

    if (!nvMem.loadConfig(&Settings))
    {
        //No config file on internal flash.
        if (SDCrd.loadConfigFile(&Settings))
        {
            //Config file on SD card.
            SDCrd.SD2nvMemory(&nvMem, &Settings); // reboot on success.          
        }
        else
        {
            //No config file on SD card. Starting wifi config server.
            forceConfig = true;
        }
    };
    
    // Free the memory from SDCard class 
    SDCrd.terminate();
    
    // Reset settings (only for development)
    //wm.resetSettings();

    //Set dark theme
    //wm.setClass("invert"); // dark theme

    // Set config save notify callback
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.setSaveParamsCallback(saveConfigCallback);

    // Set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
    wm.setAPCallback(configModeCallback);    

    //Advanced settings
    wm.setConfigPortalBlocking(false); //Hacemos que el portal no bloquee el firmware
    wm.setConnectTimeout(40); // how long to try to connect for before continuing
    wm.setConfigPortalTimeout(180); // auto close configportal after n seconds
    // wm.setCaptivePortalEnable(false); // disable captive portal redirection
    // wm.setAPClientCheck(true); // avoid timeout if client connected to softap
    //wm.setTimeout(120);
    //wm.setConfigPortalTimeout(120); //seconds

    // Custom elements

    // Text box (String) - 80 characters maximum
    WiFiManagerParameter pool_text_box("Poolurl", "Pool url", Settings.PoolAddress.c_str(), 80);

    // Need to convert numerical input to string to display the default value.
    char convertedValue[6];
    sprintf(convertedValue, "%d", Settings.PoolPort);

    // Text box (Number) - 7 characters maximum
    WiFiManagerParameter port_text_box_num("Poolport", "Pool port", convertedValue, 7);

    // Text box (String) - 80 characters maximum
    //WiFiManagerParameter password_text_box("Poolpassword", "Pool password (Optional)", Settings.PoolPassword, 80);

    // Text box (String) - 80 characters maximum
    WiFiManagerParameter addr_text_box("btcAddress", "Your BTC address", Settings.BtcWallet, 80);

  // Text box (Number) - 2 characters maximum
  char charZone[6];
  sprintf(charZone, "%d", Settings.Timezone);
  WiFiManagerParameter time_text_box_num("TimeZone", "TimeZone fromUTC (-12/+12)", charZone, 3);

  WiFiManagerParameter features_html("<hr><br><label style=\"font-weight: bold;margin-bottom: 25px;display: inline-block;\">Features</label>");

  char checkboxParams[24] = "type=\"checkbox\"";
  if (Settings.saveStats)
  {
    strcat(checkboxParams, " checked");
  }
  WiFiManagerParameter save_stats_to_nvs("SaveStatsToNVS", "Save mining statistics to flash memory.", "T", 2, checkboxParams, WFM_LABEL_AFTER);
  // Text box (String) - 80 characters maximum
    WiFiManagerParameter password_text_box("PoolpasswordOptional", "Pool password", Settings.PoolPassword, 80);

  // Add all defined parameters
  wm.addParameter(&pool_text_box);
  wm.addParameter(&port_text_box_num);
  wm.addParameter(&password_text_box);
  wm.addParameter(&addr_text_box);
  wm.addParameter(&time_text_box_num);
  wm.addParameter(&features_html);
  wm.addParameter(&save_stats_to_nvs);
  #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
  char checkboxParams2[24] = "type=\"checkbox\"";
  if (Settings.invertColors)
  {
    strcat(checkboxParams2, " checked");
  }
  WiFiManagerParameter invertColors("inverColors", "Invert Display Colors (if the colors looks weird)", "T", 2, checkboxParams2, WFM_LABEL_AFTER);
  wm.addParameter(&invertColors);
  #endif
  #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
    char brightnessConvValue[2];
    sprintf(brightnessConvValue, "%d", Settings.Brightness);
    // Text box (Number) - 3 characters maximum
    WiFiManagerParameter brightness_text_box_num("Brightness", "Screen backlight Duty Cycle (0-255)", brightnessConvValue, 3);
    wm.addParameter(&brightness_text_box_num);
  #endif

    Serial.println("AllDone: ");
    if (forceConfig)    
    {
        // Run if we need a configuration
        //No configuramos timeout al modulo
        wm.setConfigPortalBlocking(true); //Hacemos que el portal SI bloquee el firmware
        drawSetupScreen();
        mMonitor.NerdStatus = NM_Connecting;
        wm.startConfigPortal(apName, DEFAULT_WIFIPW);

        if (shouldSaveConfig)
        {
            //Could be break forced after edditing, so save new config
            Serial.println("failed to connect and hit timeout");
            Settings.PoolAddress = pool_text_box.getValue();
            Settings.PoolPort = atoi(port_text_box_num.getValue());
            normalizePoolEndpoint(Settings.PoolAddress, Settings.PoolPort);
            strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword));
            strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet));
            Settings.Timezone = atoi(time_text_box_num.getValue());
            //Serial.println(save_stats_to_nvs.getValue());
            Settings.saveStats = (strncmp(save_stats_to_nvs.getValue(), "T", 1) == 0);
            #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
                Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
            #endif
            #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
                Settings.Brightness = atoi(brightness_text_box_num.getValue());
            #endif
            nvMem.saveConfig(&Settings);
            delay(3*SECOND_MS);
            //reset and try again, or maybe put it to deep sleep
            ESP.restart();            
        };
    }
    else
    {
        //Tratamos de conectar con la configuraci+�n inicial ya almacenada
        mMonitor.NerdStatus = NM_Connecting;
        // disable captive portal redirection
        wm.setCaptivePortalEnable(true); 
        wm.setConfigPortalBlocking(true);
        wm.setEnableConfigPortal(true);
        // if (!wm.autoConnect(Settings.WifiSSID.c_str(), Settings.WifiPW.c_str()))
        if (!wm.autoConnect(apName, DEFAULT_WIFIPW))
        {
            Serial.println("Failed to connect to configured WIFI, and hit timeout");
            if (shouldSaveConfig) {
                // Save new config            
                Settings.PoolAddress = pool_text_box.getValue();
                Settings.PoolPort = atoi(port_text_box_num.getValue());
                normalizePoolEndpoint(Settings.PoolAddress, Settings.PoolPort);
                strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword));
                strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet));
                Settings.Timezone = atoi(time_text_box_num.getValue());
                // Serial.println(save_stats_to_nvs.getValue());
                Settings.saveStats = (strncmp(save_stats_to_nvs.getValue(), "T", 1) == 0);
                #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
                Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
                #endif
                #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
                Settings.Brightness = atoi(brightness_text_box_num.getValue());
                #endif
                nvMem.saveConfig(&Settings);
                vTaskDelay(2000 / portTICK_PERIOD_MS);      
            }        
            ESP.restart();                            
        } 
    }
    
    //Conectado a la red Wifi
    if (WiFi.status() == WL_CONNECTED) {
        //tft.pushImage(0, 0, MinerWidth, MinerHeight, MinerScreen);
        Serial.println("");
        Serial.println("WiFi connected");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());


        // Lets deal with the user config values

        // Copy the string value
        Settings.PoolAddress = pool_text_box.getValue();
        //strncpy(Settings.PoolAddress, pool_text_box.getValue(), sizeof(Settings.PoolAddress));
        Serial.print("PoolString: ");
        Serial.println(Settings.PoolAddress);

        //Convert the number value
        Settings.PoolPort = atoi(port_text_box_num.getValue());
        normalizePoolEndpoint(Settings.PoolAddress, Settings.PoolPort);
        Serial.print("portNumber: ");
        Serial.println(Settings.PoolPort);

        // Copy the string value
        strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword));
        Serial.print("poolPassword: ");
        Serial.println(Settings.PoolPassword);

        // Copy the string value
        strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet));
        Serial.print("btcString: ");
        Serial.println(Settings.BtcWallet);

        //Convert the number value
        Settings.Timezone = atoi(time_text_box_num.getValue());
        Serial.print("TimeZone fromUTC: ");
        Serial.println(Settings.Timezone);

        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
        Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
        Serial.print("Invert Colors: ");
        Serial.println(Settings.invertColors);        
        #endif

        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
        Settings.Brightness = atoi(brightness_text_box_num.getValue());
        Serial.print("Brightness: ");
        Serial.println(Settings.Brightness);
        #endif

    }

    // Lets deal with the user config values

    // Copy the string value
    Settings.PoolAddress = pool_text_box.getValue();
    //strncpy(Settings.PoolAddress, pool_text_box.getValue(), sizeof(Settings.PoolAddress));
    Serial.print("PoolString: ");
    Serial.println(Settings.PoolAddress);

    //Convert the number value
    Settings.PoolPort = atoi(port_text_box_num.getValue());
    normalizePoolEndpoint(Settings.PoolAddress, Settings.PoolPort);
    Serial.print("portNumber: ");
    Serial.println(Settings.PoolPort);

    // Copy the string value
    strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword));
    Serial.print("poolPassword: ");
    Serial.println(Settings.PoolPassword);

    // Copy the string value
    strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet));
    Serial.print("btcString: ");
    Serial.println(Settings.BtcWallet);

    //Convert the number value
    Settings.Timezone = atoi(time_text_box_num.getValue());
    Serial.print("TimeZone fromUTC: ");
    Serial.println(Settings.Timezone);

    #ifdef ESP32_2432S028R
    Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
    Serial.print("Invert Colors: ");
    Serial.println(Settings.invertColors);
    #endif

    // Save the custom parameters to FS
    if (shouldSaveConfig)
    {
        nvMem.saveConfig(&Settings);
        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
         if (Settings.invertColors) ESP.restart();                
        #endif
        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
        if (Settings.Brightness != 250) ESP.restart();
        #endif
    }
}

//----------------- MAIN PROCESS WIFI MANAGER --------------
int oldStatus = 0;

void wifiManagerProcess() {

    wm.process(); // avoid delays() in loop when non-blocking and other long running code
    ensureSettingsWebServerStarted();

    int newStatus = WiFi.status();
    if (newStatus != oldStatus) {
        if (newStatus == WL_CONNECTED) {
            Serial.println("CONNECTED - Current ip: " + WiFi.localIP().toString());
            ensureSettingsWebServerStarted();
        } else {
            Serial.print("[Error] - current status: ");
            Serial.println(newStatus);
        }
        oldStatus = newStatus;
    }

    if (settingsWebServerStarted) {
        settingsWebServer.handleClient();
    }
}
