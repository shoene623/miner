#include "custom_app.h"

#ifdef CUSTOM_API_UI

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace {

enum class AppState : uint8_t {
  WifiConnecting,
  Fetching,
  Rendering,
  Error
};

struct ApiViewModel {
  String title;
  String line1;
  String line2;
  String line3;
  String footer;
};

constexpr const char* kWifiSsid = "YOUR_SSID";
constexpr const char* kWifiPassword = "YOUR_PASSWORD";
constexpr const char* kApiUrl = "https://example.com/api/status";
constexpr unsigned long kWifiRetryMs = 1000;
constexpr unsigned long kApiPollMs = 10000;

TFT_eSPI tft;
AppState appState = AppState::WifiConnecting;
ApiViewModel currentView;
unsigned long lastWifiAttempt = 0;
unsigned long lastApiPoll = 0;

void drawFrame(const ApiViewModel& view) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(view.title, 10, 12, 4);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(view.line1, 10, 48, 2);
  tft.drawString(view.line2, 10, 76, 2);
  tft.drawString(view.line3, 10, 104, 2);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(view.footer, 10, tft.height() - 24, 2);
}

void showStatus(const String& status) {
  ApiViewModel view;
  view.title = "Custom UI";
  view.line1 = status;
  view.line2 = WiFi.isConnected() ? WiFi.localIP().toString() : "";
  view.line3 = "";
  view.footer = "API-driven display loop";
  drawFrame(view);
}

bool fetchApi(ApiViewModel& view) {
  HTTPClient http;
  WiFiClient client;
  WiFiClientSecure secureClient;

  const String url = String(kApiUrl);
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    if (!http.begin(secureClient, url)) {
      return false;
    }
  } else {
    if (!http.begin(client, url)) {
      return false;
    }
  }

  http.setTimeout(5000);
  const int httpCode = http.GET();
  if (httpCode <= 0) {
    http.end();
    return false;
  }

  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error) {
    return false;
  }

  view.title = doc["title"] | "Custom UI";
  view.line1 = doc["line1"] | doc["name"] | doc["status"] | "No line1 provided";
  view.line2 = doc["line2"] | doc["value"] | doc["details"] | "No line2 provided";
  view.line3 = doc["line3"] | doc["note"] | doc["extra"] | "No line3 provided";
  view.footer = doc["footer"] | doc["updated"] | WiFi.localIP().toString();
  return true;
}

void updateStatus(const String& status) {
  currentView.title = "Custom UI";
  currentView.line1 = status;
  currentView.line2 = WiFi.isConnected() ? WiFi.localIP().toString() : "";
  currentView.line3 = "";
  currentView.footer = "API-driven display loop";
  drawFrame(currentView);
}

} // namespace

void customAppBegin() {
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(kWifiSsid, kWifiPassword);

  appState = AppState::WifiConnecting;
  lastWifiAttempt = millis();
  lastApiPoll = 0;

  currentView.title = "Custom UI";
  currentView.line1 = "Starting...";
  currentView.line2 = "";
  currentView.line3 = "";
  currentView.footer = "Connect WiFi, then fetch JSON";
  drawFrame(currentView);
}

void customAppLoop() {
  const unsigned long now = millis();

  switch (appState) {
    case AppState::WifiConnecting:
      if (WiFi.status() == WL_CONNECTED) {
        appState = AppState::Fetching;
        updateStatus(String("WiFi connected: ") + WiFi.localIP().toString());
        break;
      }

      if (now - lastWifiAttempt >= kWifiRetryMs) {
        lastWifiAttempt = now;
        WiFi.reconnect();
        updateStatus("Connecting to WiFi...");
      }
      break;

    case AppState::Fetching:
      if (WiFi.status() != WL_CONNECTED) {
        appState = AppState::WifiConnecting;
        updateStatus("WiFi lost, reconnecting...");
        break;
      }

      if (now - lastApiPoll < kApiPollMs) {
        break;
      }

      lastApiPoll = now;
      if (fetchApi(currentView)) {
        appState = AppState::Rendering;
        drawFrame(currentView);
      } else {
        appState = AppState::Error;
        updateStatus("API fetch or parse failed");
      }
      break;

    case AppState::Rendering:
      if (WiFi.status() != WL_CONNECTED) {
        appState = AppState::WifiConnecting;
        updateStatus("WiFi lost, reconnecting...");
        break;
      }

      if (now - lastApiPoll >= kApiPollMs) {
        appState = AppState::Fetching;
      }
      break;

    case AppState::Error:
      if (WiFi.status() != WL_CONNECTED) {
        appState = AppState::WifiConnecting;
        updateStatus("WiFi lost, reconnecting...");
        break;
      }

      if (now - lastApiPoll >= kApiPollMs) {
        appState = AppState::Fetching;
      }
      break;
  }
}

#else

void customAppBegin() {}

void customAppLoop() {}

#endif