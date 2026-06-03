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

static String renderSettingsPageHtml(const String &message = "") {
    String html;
    html.reserve(2048);
    html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>NerdMiner Settings</title>";
    html += "<style>body{font-family:Arial,sans-serif;margin:0;background:#111;color:#eee}main{max-width:680px;margin:20px auto;padding:20px;background:#1a1a1a;border-radius:12px}h1{margin-top:0}label{display:block;margin:14px 0 6px}input[type=text]{width:100%;padding:10px;border:1px solid #444;border-radius:8px;background:#0f0f0f;color:#eee}button{margin-top:18px;padding:10px 14px;background:#008c7a;color:#fff;border:0;border-radius:8px;font-weight:700;cursor:pointer}.msg{margin:10px 0;padding:8px 10px;background:#1f3c2e;border-left:4px solid #37c978;border-radius:6px}.meta{color:#aaa;font-size:12px}</style></head><body>";
    html += "<main><h1>NerdMiner Settings</h1>";
    html += "<div class='meta'>Device IP: ";
    html += WiFi.localIP().toString();
    html += "</div>";
    if (!message.isEmpty()) {
        html += "<div class='msg'>" + message + "</div>";
    }
    html += "<form method='POST' action='/save'>";
    html += "<label for='btcWallet'>BTC Wallet</label>";
    html += "<input id='btcWallet' name='btcWallet' type='text' maxlength='79' value='";
    html += String(Settings.BtcWallet);
    html += "'>";
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
    settingsWebServer.send(200, "text/html", renderSettingsPageHtml());
}

static void handleSettingsSave() {
    if (settingsWebServer.hasArg("btcWallet")) {
        String wallet = settingsWebServer.arg("btcWallet");
        wallet.trim();
        strncpy(Settings.BtcWallet, wallet.c_str(), sizeof(Settings.BtcWallet) - 1);
        Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';
    }

    Settings.rearLedEnabled = settingsWebServer.hasArg("rearLedEnabled");
    nvMem.saveConfig(&Settings);
    settingsWebServer.send(200, "text/html", renderSettingsPageHtml("Settings saved."));
}

static void ensureSettingsWebServerStarted() {
    if (settingsWebServerStarted || WiFi.status() != WL_CONNECTED) {
        return;
    }

    settingsWebServer.on("/", HTTP_GET, handleSettingsRoot);
    settingsWebServer.on("/save", HTTP_POST, handleSettingsSave);
    settingsWebServer.begin();
    settingsWebServerStarted = true;
    Serial.println("Settings web server started on http://" + WiFi.localIP().toString());
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
        String jsonPart = data.substring(18); // Después del marcador "WEBFLASHER_CONFIG:"
        
        Serial.printf("DEBUG: JSON part: '%s'\n", jsonPart.c_str());
        
        DynamicJsonDocument doc(256);
        DeserializationError error = deserializeJson(doc, jsonPart);
        
        if (error == DeserializationError::Ok) {
            Serial.println("DEBUG: JSON parsed successfully");
            
            if (doc.containsKey("apname")) {
                String customAP = doc["apname"].as<String>();
                customAP.trim();
                
                if (customAP.length() > 0 && customAP.length() < 32) {
                    Serial.printf("✅ Custom AP name from webflasher: %s\n", customAP.c_str());
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
  WiFiManagerParameter password_text_box("Poolpassword - Optional", "Pool password", Settings.PoolPassword, 80);

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
        //Tratamos de conectar con la configuración inicial ya almacenada
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
