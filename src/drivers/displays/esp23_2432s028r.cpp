#include "displayDriver.h"

#if defined ESP32_2432S028R || ESP32_2432S028_2USB

#include <TFT_eSPI.h>
#include <TFT_eTouch.h>
#include "media/images_320_170.h"
#include "media/images_bottom_320_70.h"
#include "media/myFonts.h"
#include "media/Free_Fonts.h"
#include "version.h"
#include "monitor.h"
#include "OpenFontRender.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include "rotation.h"
#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"

#define WIDTH 130 //320
#define HEIGHT 170 

extern nvMemory nvMem;

OpenFontRender render;
TFT_eSPI tft = TFT_eSPI();                  // Invoke library, pins defined in platformio.ini
TFT_eSprite background = TFT_eSprite(&tft); // Invoke library sprite
SPIClass hSPI(HSPI);
TFT_eTouch<TFT_eSPI> touch(tft, ETOUCH_CS, 0xFF, hSPI); 

extern monitor_data mMonitor;
extern pool_data pData;
extern DisplayDriver *currentDisplayDriver;
extern bool invertColors; 
extern TSettings Settings;
bool hasChangedScreen = true;

const unsigned long AUTO_SCROLL_INTERVAL_MS = 10000;
bool autoScrollEnabled = false;
unsigned long lastAutoScrollMs = 0;

const int SCREEN_IDX_TICKER = 0;
const int SCREEN_IDX_CANDLES = 1;
const int SCREEN_IDX_SETTINGS = 2;

const char* TICKER_API_URL = "https://seans-forcasting.vercel.app/api/ticker";
const char* CANDLES_API_URL = "https://seans-forcasting.vercel.app/api/candles?window=2h";
const unsigned long TICKER_REFRESH_INTERVAL_MS = 15000;

SemaphoreHandle_t tickerDataMutex = nullptr;
volatile bool tickerFetchInProgress = false;
unsigned long tickerLastFetchAttemptMs = 0;

constexpr int CANDLE_MAX_POINTS = 24;

struct CandlePoint {
  float open = 0.0f;
  float high = 0.0f;
  float low = 0.0f;
  float close = 0.0f;
  float volume = 0.0f;
};

struct CandleSeries {
  String price;
  String direction;
  String move;
  int count = 0;
  CandlePoint points[CANDLE_MAX_POINTS];
};

struct TickerData {
  // Custom sentiment API fields
  String sentiment;
  String direction;
  String category;
  String recommendation;
  String daily;
  String weekly;
  String monthly;
  String btcDirection;
  String btcEntry;
  String btcTakeProfit;
  String xrpDirection;
  String xrpEntry;
  String xrpTakeProfit;
  // Price fields (reserved for future use)
  String btcPrice;
  String btcChange24h;
  String xrpPrice;
  String xrpChange24h;
  String candleWindow;
  String candleTf;
  String candleUpdated;
  CandleSeries btcCandles;
  CandleSeries ethCandles;
  CandleSeries xrpCandles;
  String updated;
  String status;
};

TickerData tickerCache;

String toUpperCopy(String value) {
  value.toUpperCase();
  return value;
}

String formatNumericOrText(JsonObjectConst obj, const char* numericKey, const char* textKey, int decimals = 2) {
  if (obj[numericKey].is<float>() || obj[numericKey].is<double>() || obj[numericKey].is<int>() || obj[numericKey].is<long>()) {
    return String(obj[numericKey].as<double>(), decimals);
  }

  if (textKey != nullptr) {
    return String((const char*)(obj[textKey] | "--"));
  }

  return String("--");
}

static float jsonVariantToFloat(JsonVariantConst value) {
  if (value.is<float>() || value.is<double>() || value.is<int>() || value.is<long>()) {
    return value.as<double>();
  }

  const char* text = value.as<const char*>();
  if (text != nullptr) {
    return String(text).toFloat();
  }

  return 0.0f;
}

static void resetCandleSeries(CandleSeries &series) {
  series = CandleSeries{};
}

static bool parseCandleSeries(JsonObjectConst rootObj, const char* key, CandleSeries &outSeries) {
  resetCandleSeries(outSeries);

  if (!rootObj.containsKey(key)) {
    return false;
  }

  JsonObjectConst seriesObj = rootObj[key].as<JsonObjectConst>();
  if (seriesObj.isNull()) {
    return false;
  }

  outSeries.price = seriesObj["p"] | "--";
  outSeries.direction = seriesObj["d"] | "--";
  outSeries.move = seriesObj["m"] | "--";

  if (!seriesObj.containsKey("c")) {
    return false;
  }

  JsonArrayConst candles = seriesObj["c"].as<JsonArrayConst>();
  if (candles.isNull()) {
    return false;
  }

  int index = 0;
  for (JsonVariantConst candleValue : candles) {
    JsonArrayConst candle = candleValue.as<JsonArrayConst>();
    if (candle.isNull()) {
      continue;
    }

    if (candle.size() < 6) {
      continue;
    }

    if (index >= CANDLE_MAX_POINTS) {
      break;
    }

    CandlePoint &point = outSeries.points[index];
    point.open = jsonVariantToFloat(candle[1]);
    point.high = jsonVariantToFloat(candle[2]);
    point.low = jsonVariantToFloat(candle[3]);
    point.close = jsonVariantToFloat(candle[4]);
    point.volume = jsonVariantToFloat(candle[5]);
    index++;
  }

  outSeries.count = index;
  return index > 0;
}

static String formatPctMove(float current, float previous) {
  if (previous == 0.0f) {
    return "+0.0%";
  }
  const float change = ((current - previous) / previous) * 100.0f;
  const char sign = (change >= 0.0f) ? '+' : '-';
  const float magnitude = (change >= 0.0f) ? change : -change;
  return String(sign) + String(magnitude, 1) + "%";
}

static bool parseCandleSeriesCompact(JsonObjectConst candlesObj, const char* key, CandleSeries &outSeries) {
  resetCandleSeries(outSeries);

  if (!candlesObj.containsKey(key)) {
    return false;
  }

  JsonArrayConst candles = candlesObj[key].as<JsonArrayConst>();
  if (candles.isNull()) {
    return false;
  }

  int index = 0;
  for (JsonVariantConst candleValue : candles) {
    JsonObjectConst candle = candleValue.as<JsonObjectConst>();
    if (candle.isNull()) {
      continue;
    }

    if (index >= CANDLE_MAX_POINTS) {
      break;
    }

    CandlePoint &point = outSeries.points[index];
    point.open = jsonVariantToFloat(candle["o"]);
    point.high = jsonVariantToFloat(candle["h"]);
    point.low = jsonVariantToFloat(candle["l"]);
    point.close = jsonVariantToFloat(candle["c"]);
    point.volume = jsonVariantToFloat(candle["v"]);
    index++;
  }

  outSeries.count = index;
  if (index <= 0) {
    return false;
  }

  const CandlePoint &latest = outSeries.points[index - 1];
  const CandlePoint &previous = outSeries.points[(index > 1) ? (index - 2) : (index - 1)];
  outSeries.price = String(latest.close, 2);
  outSeries.direction = (latest.close >= latest.open) ? "up" : "down";
  outSeries.move = formatPctMove(latest.close, previous.close);
  return true;
}

static bool parseNestedCandles(JsonObjectConst rootObj, TickerData &outData) {
  if (!rootObj.containsKey("candles")) {
    return false;
  }

  JsonObjectConst candlesObj = rootObj["candles"].as<JsonObjectConst>();
  if (candlesObj.isNull()) {
    return false;
  }

  outData.candleWindow = rootObj["w"] | "2h";
  outData.candleTf = candlesObj["tf"] | rootObj["tf"] | "--";
  outData.candleUpdated = rootObj["updated"] | rootObj["u"] | "--";

  bool hasAny = false;
  hasAny = parseCandleSeriesCompact(candlesObj, "btc", outData.btcCandles) || hasAny;
  hasAny = parseCandleSeriesCompact(candlesObj, "eth", outData.ethCandles) || hasAny;
  hasAny = parseCandleSeriesCompact(candlesObj, "xrp", outData.xrpCandles) || hasAny;
  return hasAny;
}

bool parseTickerObject(JsonObjectConst obj, const char* expectedSymbol, String& outPrice, String& outChange) {
  String parsedSymbol = toUpperCopy(String((const char*)(obj["symbol"] | obj["asset"] | obj["name"] | "")));
  if (parsedSymbol.length() > 0 && parsedSymbol != expectedSymbol) {
    return false;
  }

  if (obj["price"].is<float>() || obj["price"].is<double>() || obj["price"].is<int>() || obj["price"].is<long>()) {
    outPrice = String(obj["price"].as<double>(), 2);
  } else if (obj["last"].is<float>() || obj["last"].is<double>() || obj["last"].is<int>() || obj["last"].is<long>()) {
    outPrice = String(obj["last"].as<double>(), 2);
  } else if (obj["value"].is<float>() || obj["value"].is<double>() || obj["value"].is<int>() || obj["value"].is<long>()) {
    outPrice = String(obj["value"].as<double>(), 2);
  } else {
    outPrice = String((const char*)(obj["price"] | obj["last"] | obj["value"] | "--"));
  }

  if (obj["change24h"].is<float>() || obj["change24h"].is<double>() || obj["change24h"].is<int>() || obj["change24h"].is<long>()) {
    outChange = String(obj["change24h"].as<double>(), 2) + "%";
  } else if (obj["change_24h"].is<float>() || obj["change_24h"].is<double>() || obj["change_24h"].is<int>() || obj["change_24h"].is<long>()) {
    outChange = String(obj["change_24h"].as<double>(), 2) + "%";
  } else if (obj["percent_change_24h"].is<float>() || obj["percent_change_24h"].is<double>() || obj["percent_change_24h"].is<int>() || obj["percent_change_24h"].is<long>()) {
    outChange = String(obj["percent_change_24h"].as<double>(), 2) + "%";
  } else {
    outChange = String((const char*)(obj["change24h"] | obj["change_24h"] | obj["percent_change_24h"] | obj["change"] | "--"));
  }

  return true;
}

bool findTickerInVariant(JsonVariantConst node, const char* symbol, String& outPrice, String& outChange) {
  if (node.is<JsonObject>()) {
    JsonObjectConst obj = node.as<JsonObjectConst>();

    if (parseTickerObject(obj, symbol, outPrice, outChange)) {
      return true;
    }

    const char* containers[] = {"data", "tickers", "coins", "results", "items"};
    for (const char* key : containers) {
      if (obj.containsKey(key) && findTickerInVariant(obj[key], symbol, outPrice, outChange)) {
        return true;
      }
    }

    String lowerSymbol = String(symbol);
    lowerSymbol.toLowerCase();
    if (obj.containsKey(symbol) && findTickerInVariant(obj[symbol], symbol, outPrice, outChange)) {
      return true;
    }
    if (obj.containsKey(lowerSymbol.c_str()) && findTickerInVariant(obj[lowerSymbol.c_str()], symbol, outPrice, outChange)) {
      return true;
    }
  }

  if (node.is<JsonArray>()) {
    JsonArrayConst arr = node.as<JsonArrayConst>();
    for (JsonVariantConst item : arr) {
      if (findTickerInVariant(item, symbol, outPrice, outChange)) {
        return true;
      }
    }
  }

  return false;
}

void copyTickerCache(TickerData &dst) {
  if (tickerDataMutex == nullptr) {
    dst = tickerCache;
    return;
  }

  if (xSemaphoreTake(tickerDataMutex, 5 / portTICK_PERIOD_MS) == pdTRUE) {
    dst = tickerCache;
    xSemaphoreGive(tickerDataMutex);
  }
}

void storeTickerCache(const TickerData &src) {
  if (tickerDataMutex == nullptr) {
    tickerCache = src;
    return;
  }

  if (xSemaphoreTake(tickerDataMutex, 50 / portTICK_PERIOD_MS) == pdTRUE) {
    tickerCache = src;
    xSemaphoreGive(tickerDataMutex);
  }
}

bool parseTickerJson(const String &payload, TickerData &outData) {
  // Candle payloads are much larger than sentiment payloads.
  DynamicJsonDocument doc(32768);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    outData.status = "JSON parse failed";
    return false;
  }

  JsonVariant root = doc.as<JsonVariant>();
  JsonObject rootObj;
  if (root.is<JsonObject>()) {
    rootObj = root.as<JsonObject>();
  }

  // Primary path: custom sentiment API
  if (!rootObj.isNull() && (rootObj.containsKey("sentiment") || rootObj.containsKey("market_sentiment"))) {
    outData.sentiment  = rootObj["market_sentiment"] | rootObj["sentiment"] | "--";
    outData.direction  = rootObj["market_direction"] | rootObj["direction"] | "--";
    outData.category   = rootObj["category"]   | "--";
    outData.recommendation = rootObj["recommendation"] | "--";
    outData.daily      = rootObj["daily"]      | "--";
    outData.weekly     = rootObj["weekly"]     | "--";
    outData.monthly    = rootObj["monthly"]    | "--";

    if (rootObj.containsKey("btc") && rootObj["btc"].is<JsonObjectConst>()) {
      JsonObjectConst btc = rootObj["btc"].as<JsonObjectConst>();
      outData.btcDirection = btc["direction"] | "--";
      outData.btcEntry = formatNumericOrText(btc, "ideal_entry", nullptr, 0);
      outData.btcTakeProfit = formatNumericOrText(btc, "take_profit", nullptr, 0);
    } else {
      outData.btcDirection = "--";
      outData.btcEntry = "--";
      outData.btcTakeProfit = "--";
    }

    if (rootObj.containsKey("xrp") && rootObj["xrp"].is<JsonObjectConst>()) {
      JsonObjectConst xrp = rootObj["xrp"].as<JsonObjectConst>();
      outData.xrpDirection = xrp["direction"] | "--";
      outData.xrpEntry = formatNumericOrText(xrp, "ideal_entry", nullptr, 3);
      outData.xrpTakeProfit = formatNumericOrText(xrp, "take_profit", nullptr, 3);
    } else {
      outData.xrpDirection = "--";
      outData.xrpEntry = "--";
      outData.xrpTakeProfit = "--";
    }

    // Ignore the API's stale server-side 'updated' date.
    // The footer will show the live device clock at render time instead.
    outData.updated = ""; // populated at display time via getClockData()

    // Ticker endpoint may include nested candles payload.
    parseNestedCandles(rootObj, outData);

    outData.status = "OK";
    return true;
  }

  if (!rootObj.isNull() && rootObj.containsKey("w") && rootObj.containsKey("tf")) {
    outData.candleWindow = rootObj["w"] | "--";
    outData.candleTf = rootObj["tf"] | "--";
    outData.candleUpdated = rootObj["u"] | rootObj["updated"] | "--";
    parseCandleSeries(rootObj, "b", outData.btcCandles);
    parseCandleSeries(rootObj, "e", outData.ethCandles);
    parseCandleSeries(rootObj, "x", outData.xrpCandles);
    outData.status = "OK";
    return true;
  }

  // Fallback: BTC/XRP price data
  bool hasBtc = findTickerInVariant(root, "BTC", outData.btcPrice, outData.btcChange24h);
  bool hasXrp = findTickerInVariant(root, "XRP", outData.xrpPrice, outData.xrpChange24h);

  if (!rootObj.isNull() && rootObj.containsKey("updated")) {
    outData.updated = String((const char*)rootObj["updated"]);
  } else if (!rootObj.isNull() && rootObj.containsKey("timestamp")) {
    outData.updated = String((const char*)rootObj["timestamp"]);
  } else {
    outData.updated = String(millis() / 1000) + "s";
  }

  outData.status = (hasBtc || hasXrp)
                     ? (hasBtc && hasXrp ? "OK" : "Partial data")
                     : "No known fields";
  return true;
}

bool fetchTickerPayload(const char* endpointUrl, String &payload, String &status) {
  HTTPClient http;
  http.setTimeout(5000);
  http.setConnectTimeout(5000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setReuse(false);

  bool began = false;
  const String url = String(endpointUrl);
  static WiFiClientSecure secureClient;

  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    began = http.begin(secureClient, url);
  } else {
    began = http.begin(url);
  }

  if (!began) {
    status = "HTTP begin failed";
    return false;
  }

  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "NerdMinerV2-Ticker/1.0");

  int httpCode = http.GET();
  if (httpCode <= 0) {
    // Retry once for transient DNS/socket failures seen on ESP32.
    delay(120);
    httpCode = http.GET();
  }

  if (httpCode == HTTP_CODE_OK) {
    payload = http.getString();
    http.end();
    status = "HTTP 200";
    return true;
  }

  status = String("HTTP ") + String(httpCode) + " " + http.errorToString(httpCode);
  http.end();
  return false;
}

void tickerFetchTask(void *param) {
  TickerData newData;
  copyTickerCache(newData);

  if (WiFi.status() != WL_CONNECTED) {
    newData.status = "WiFi disconnected";
    storeTickerCache(newData);
    tickerFetchInProgress = false;
    vTaskDelete(nullptr);
    return;
  }

  String tickerPayload;
  String candlePayload;
  String tickerStatus;
  String candleStatus;
  bool ok = false;
  bool tickerOk = false;
  bool candleOk = false;

  if (fetchTickerPayload(TICKER_API_URL, tickerPayload, tickerStatus)) {
    tickerOk = parseTickerJson(tickerPayload, newData);
    ok = tickerOk || ok;
  }

  const bool hasCandlesFromTicker =
      newData.btcCandles.count > 0 ||
      newData.ethCandles.count > 0 ||
      newData.xrpCandles.count > 0;

  // Free the temporary payload before any optional second HTTPS request.
  tickerPayload = String();

  if (!hasCandlesFromTicker) {
    if (fetchTickerPayload(CANDLES_API_URL, candlePayload, candleStatus)) {
      candleOk = parseTickerJson(candlePayload, newData);
      ok = candleOk || ok;
    }
  } else {
    candleOk = true;
  }

  if (ok) {
    if (!candleOk && tickerOk && !candleStatus.isEmpty()) {
      newData.status = String("Candles issue: ") + candleStatus;
    } else if (!tickerOk && candleOk && !tickerStatus.isEmpty()) {
      newData.status = String("Ticker issue: ") + tickerStatus;
    } else {
      newData.status = "OK";
    }
  } else if (!candleStatus.isEmpty()) {
    newData.status = String("Candles ") + candleStatus;
  } else if (!tickerStatus.isEmpty()) {
    newData.status = String("Ticker ") + tickerStatus;
  } else {
    newData.status = "No API data";
  }

  if (ok) {
    Serial.printf("[TICKER] BTC %s (%s), XRP %s (%s)\n", newData.btcPrice.c_str(), newData.btcChange24h.c_str(), newData.xrpPrice.c_str(), newData.xrpChange24h.c_str());
  } else {
    Serial.printf("[TICKER] Update failed: %s\n", newData.status.c_str());
  }

  storeTickerCache(newData);
  tickerFetchInProgress = false;
  vTaskDelete(nullptr);
}

void maybeStartTickerFetch() {
  const unsigned long now = millis();
  if (tickerFetchInProgress) {
    return;
  }

  if (tickerLastFetchAttemptMs != 0 && (now - tickerLastFetchAttemptMs) < TICKER_REFRESH_INTERVAL_MS) {
    return;
  }

  tickerLastFetchAttemptMs = now;
  tickerFetchInProgress = true;
  BaseType_t created = xTaskCreatePinnedToCore(tickerFetchTask, "TickerFetch", 12288, nullptr, 1, nullptr, 1);
  if (created != pdPASS) {
    tickerFetchInProgress = false;
  }
}

// ──────────────────────────────────────────────────────────
// Dashboard colour palette (RGB565)
static const uint16_t TC_BG     = TFT_BLACK;
static const uint16_t TC_CARD   = 0x2104;  // ~#212021 dark card
static const uint16_t TC_BORDER = 0x4228;  // ~#424242 card border
static const uint16_t TC_DIM    = 0x52AA;  // ~#525252 muted
static const uint16_t TC_SEC    = 0xAD55;  // ~#AAAAAA secondary
static const uint16_t TC_BULL   = TFT_GREEN;
static const uint16_t TC_BEAR   = TFT_RED;
static const uint16_t TC_NEUT   = TFT_ORANGE;
static const uint16_t TC_ACCENT = TFT_CYAN;
static const uint16_t TC_WARN   = TFT_YELLOW;

static String lastRenderedTickerHash = "";
static String lastRenderedCandleHash = "";
static int lastRenderedTickerScreen = -1;
static int lastRenderedCandleScreen = -1;
static int tickerViewMode = 0; // 0 = sentiment, 1 = recommendation
static unsigned long tickerViewLastSwitchMs = 0;

static String tickerDataHash(const TickerData &d) {
  return d.sentiment + d.direction
  + d.recommendation.substring(0, 32)
       + d.daily.substring(0, 24)
       + d.weekly.substring(0, 24)
  + d.monthly.substring(0, 24)
       + d.status;
}

static uint16_t directionColor(const String &dir) {
  String u = dir;
  u.toUpperCase();
  if (u.indexOf("BULL") >= 0) return TC_BULL;
  if (u.indexOf("BEAR") >= 0) return TC_BEAR;
  return TC_NEUT;
}

static bool parseDateDDMMYYYY(const String &dateStr, int &day, int &month, int &year) {
  if (dateStr.length() < 10) return false;
  day = dateStr.substring(0, 2).toInt();
  month = dateStr.substring(3, 5).toInt();
  year = dateStr.substring(6, 10).toInt();
  return day > 0 && month > 0 && month <= 12 && year >= 1970;
}

static int dayOfWeekYMD(int year, int month, int day) {
  // Zeller congruence mapped to 0=Sunday..6=Saturday
  if (month < 3) {
    month += 12;
    year -= 1;
  }
  const int K = year % 100;
  const int J = year / 100;
  const int h = (day + (13 * (month + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
  return (h + 6) % 7;
}

static int nthSunday(int year, int month, int n) {
  const int dowFirst = dayOfWeekYMD(year, month, 1);
  const int firstSunday = (dowFirst == 0) ? 1 : (8 - dowFirst);
  return firstSunday + (n - 1) * 7;
}

static bool isEasternDst(int year, int month, int day, int hour) {
  if (month < 3 || month > 11) return false;
  if (month > 3 && month < 11) return true;

  if (month == 3) {
    const int secondSunday = nthSunday(year, 3, 2);
    if (day > secondSunday) return true;
    if (day < secondSunday) return false;
    return hour >= 2;
  }

  const int firstSunday = nthSunday(year, 11, 1);
  if (day < firstSunday) return true;
  if (day > firstSunday) return false;
  return hour < 2;
}

static int daysInMonth(int month, int year) {
  static const int kDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return kDays[month - 1];
}

static void adjustDateByDays(int &day, int &month, int &year, int deltaDays) {
  day += deltaDays;
  while (day > daysInMonth(month, year)) {
    day -= daysInMonth(month, year);
    month++;
    if (month > 12) {
      month = 1;
      year++;
    }
  }
  while (day < 1) {
    month--;
    if (month < 1) {
      month = 12;
      year--;
    }
    day += daysInMonth(month, year);
  }
}

static String formatAsMMDDYYYY(int month, int day, int year) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d/%02d/%04d", month, day, year);
  return String(buf);
}

static String formatAsHHMM(int hour, int minute) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
  return String(buf);
}

static void getApiPageEasternDateTime(String &outDate, String &outTime) {
  clock_data localClock = getClockData(0);
  clock_data_t localClockT = getClockData_t(0);

  int localDay = 0;
  int localMonth = 0;
  int localYear = 0;
  if (!parseDateDDMMYYYY(localClock.currentDate, localDay, localMonth, localYear)) {
    outDate = localClock.currentDate;
    outTime = localClock.currentTime;
    return;
  }

  int etOffset = isEasternDst(localYear, localMonth, localDay, (int)localClockT.currentHours) ? -4 : -5;
  const int deltaMinutes = (etOffset - Settings.Timezone) * 60;

  int etHour = (int)localClockT.currentHours;
  int etMinute = (int)localClockT.currentMinutes;
  int totalMinutes = etHour * 60 + etMinute + deltaMinutes;

  int dayShift = 0;
  while (totalMinutes < 0) {
    totalMinutes += 1440;
    dayShift--;
  }
  while (totalMinutes >= 1440) {
    totalMinutes -= 1440;
    dayShift++;
  }

  int etDay = localDay;
  int etMonth = localMonth;
  int etYear = localYear;
  if (dayShift != 0) {
    adjustDateByDays(etDay, etMonth, etYear, dayShift);
  }

  etHour = totalMinutes / 60;
  etMinute = totalMinutes % 60;

  outDate = formatAsMMDDYYYY(etMonth, etDay, etYear);
  outTime = formatAsHHMM(etHour, etMinute);
}

static String getApiPageGreeting(const String &mmddyyyy) {
  int month = 0;
  int day = 0;
  int year = 0;
  if (!parseDateDDMMYYYY(mmddyyyy, day, month, year)) {
    return "Welcome Patrick";
  }

  if (year < 2026) {
    return "Happy Fatherday Dad!";
  }

  if (year > 2026) {
    return "Welcome Patrick";
  }

  if (month < 7) {
    return "Happy Fatherday Dad!";
  }

  if (month > 7) {
    return "Welcome Patrick";
  }

  return (day <= 4) ? "Happy Fatherday Dad!" : "Welcome Patrick";
}

// Wrap text to maxLines lines each at most maxW pixels wide.
// font 1 = 8 px tall (lineH 10), font 2 = 16 px tall (lineH 18).
static int drawWrapped(const String &text, int x, int y, int maxW,
                       int maxLines, uint8_t font,
                       uint16_t col, uint16_t bg) {
  if (text.isEmpty() || text == "--") return 0;
  tft.setTextColor(col, bg);
  const int lineH = (font == 2) ? 18 : 10;
  int drawn = 0;
  String rem = text;
  while (rem.length() > 0 && drawn < maxLines) {
    // Binary search: longest prefix that fits in maxW
    int lo = 1, hi = (int)rem.length();
    while (lo < hi) {
      int mid = (lo + hi + 1) / 2;
      if (tft.textWidth(rem.substring(0, mid), font) <= maxW) lo = mid;
      else hi = mid - 1;
    }
    int cut = lo;
    // Prefer word boundary
    if (cut < (int)rem.length()) {
      int wb = cut;
      while (wb > 0 && rem.charAt(wb) != ' ') wb--;
      if (wb > 0) cut = wb;
    }
    String line = rem.substring(0, cut);
    // Last line: append ellipsis if text is truncated
    if (drawn == maxLines - 1 && cut < (int)rem.length()) {
      while (line.length() > 0 && tft.textWidth(line + "...", font) > maxW)
        line = line.substring(0, line.length() - 1);
      line += "...";
    }
    tft.drawString(line, x, y + drawn * lineH, font);
    drawn++;
    rem = (cut >= (int)rem.length())
            ? String("")
            : rem.substring(rem.charAt(cut) == ' ' ? cut + 1 : cut);
  }
  return drawn;
}

static void drawHRule(int y, uint16_t col = 0x4228) {
  tft.drawFastHLine(8, y, 304, col);
}

static void drawInfoCard(int x, int y, int w, int h) {
  tft.fillRoundRect(x, y, w, h, 5, TC_CARD);
  tft.drawRoundRect(x, y, w, h, 5, TC_BORDER);
}

static uint16_t candleColorForDirection(const String &dir) {
  String u = dir;
  u.toUpperCase();
  if (u.indexOf("UP") >= 0 || u.indexOf("BULL") >= 0) return TC_BULL;
  if (u.indexOf("DOWN") >= 0 || u.indexOf("BEAR") >= 0) return TC_BEAR;
  return TC_ACCENT;
}

static String candleDataHash(const TickerData &d) {
  String hash = d.candleWindow + d.candleTf + d.candleUpdated;
  hash += d.btcCandles.price + d.btcCandles.direction + d.btcCandles.move + String(d.btcCandles.count);
  hash += d.ethCandles.price + d.ethCandles.direction + d.ethCandles.move + String(d.ethCandles.count);
  hash += d.xrpCandles.price + d.xrpCandles.direction + d.xrpCandles.move + String(d.xrpCandles.count);
  if (d.btcCandles.count > 0) {
    const CandlePoint &last = d.btcCandles.points[d.btcCandles.count - 1];
    hash += String(last.open, 2) + String(last.high, 2) + String(last.low, 2) + String(last.close, 2);
  }
  return hash;
}

static void drawCandlePanel(int x, int y, int w, int h, const char* label, const CandleSeries &series) {
  drawInfoCard(x, y, w, h);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TC_ACCENT, TC_CARD);
  tft.drawString(label, x + 8, y + 4, 1);

  const String priceText = series.price.isEmpty() ? "--" : series.price;
  tft.setTextColor(TFT_WHITE, TC_CARD);
  tft.drawString(priceText, x + w - tft.textWidth(priceText, 1) - 8, y + 4, 1);

  const String moveText = (series.direction.isEmpty() ? "--" : series.direction) + String(" ") + (series.move.isEmpty() ? "--" : series.move);
  tft.setTextColor(candleColorForDirection(series.direction), TC_CARD);
  tft.drawString(moveText, x + 8, y + 15, 1);

  const int chartX = x + 6;
  const int chartY = y + 28;
  const int chartW = w - 12;
  const int chartH = h - 36;
  const int chartBottom = chartY + chartH - 1;

  tft.drawRect(chartX, chartY, chartW, chartH, TC_BORDER);

  if (series.count <= 0) {
    tft.setTextColor(TC_SEC, TC_CARD);
    tft.drawString("No candle data", x + 8, chartY + chartH / 2 - 4, 1);
    return;
  }

  float minLow = series.points[0].low;
  float maxHigh = series.points[0].high;
  for (int i = 1; i < series.count; ++i) {
    if (series.points[i].low < minLow) minLow = series.points[i].low;
    if (series.points[i].high > maxHigh) maxHigh = series.points[i].high;
  }

  const float range = maxHigh - minLow;
  const float safeRange = (range <= 0.0f) ? 1.0f : range;
  const int slotW = max(1, chartW / series.count);
  const int bodyW = max(2, slotW - 3);

  for (int i = 0; i < series.count; ++i) {
    const CandlePoint &point = series.points[i];
    const int cx = chartX + (i * slotW) + (slotW / 2);

    auto scalePrice = [&](float value) -> int {
      const float normalized = (value - minLow) / safeRange;
      int py = chartBottom - (int)(normalized * (chartH - 2));
      if (py < chartY) py = chartY;
      if (py > chartBottom) py = chartBottom;
      return py;
    };

    const int highY = scalePrice(point.high);
    const int lowY = scalePrice(point.low);
    const int openY = scalePrice(point.open);
    const int closeY = scalePrice(point.close);
    const uint16_t col = (point.close >= point.open) ? TC_BULL : TC_BEAR;

    tft.drawLine(cx, highY, cx, lowY, col);

    const int bodyTop = min(openY, closeY);
    const int bodyBottom = max(openY, closeY);
    const int bodyH = max(1, bodyBottom - bodyTop);
    tft.fillRect(cx - (bodyW / 2), bodyTop, bodyW, bodyH, col);
  }
}

static void drawCandlePage(unsigned long mElapsed) {
  maybeStartTickerFetch();

  TickerData data;
  copyTickerCache(data);

  const String hash = candleDataHash(data);
  const bool screenChanged = (lastRenderedCandleScreen != currentDisplayDriver->current_cyclic_screen);
  const bool changed = screenChanged || (hash != lastRenderedCandleHash);
  if (changed) {
    tft.fillScreen(TC_BG);
    lastRenderedCandleHash = hash;
    lastRenderedCandleScreen = currentDisplayDriver->current_cyclic_screen;
  }

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TC_BG);
  tft.drawString("2H CANDLE CHART", 10, 5, 1);

  const String tfText = (data.candleWindow.isEmpty() ? "--" : data.candleWindow) + String(" / ") + (data.candleTf.isEmpty() ? "--" : data.candleTf);
  tft.setTextColor(TC_DIM, TC_BG);
  tft.drawString(tfText, 10, 16, 1);

  const String updatedText = data.candleUpdated.isEmpty() ? "--" : data.candleUpdated;
  tft.setTextColor(TC_SEC, TC_BG);
  tft.drawString(updatedText, 310 - tft.textWidth(updatedText, 1), 5, 1);

  if (!changed) {
    return;
  }

  drawCandlePanel(5, 28, 310, 146, "BTC", data.btcCandles);

  const String ethLine = String("ETH ") + (data.ethCandles.price.isEmpty() ? "--" : data.ethCandles.price)
                       + String(" ") + (data.ethCandles.direction.isEmpty() ? "--" : data.ethCandles.direction)
                       + String(" ") + (data.ethCandles.move.isEmpty() ? "--" : data.ethCandles.move);
  const String xrpLine = String("XRP ") + (data.xrpCandles.price.isEmpty() ? "--" : data.xrpCandles.price)
                       + String(" ") + (data.xrpCandles.direction.isEmpty() ? "--" : data.xrpCandles.direction)
                       + String(" ") + (data.xrpCandles.move.isEmpty() ? "--" : data.xrpCandles.move);

  tft.setTextColor(candleColorForDirection(data.ethCandles.direction), TC_BG);
  tft.drawString(ethLine, 10, 178, 1);
  tft.setTextColor(candleColorForDirection(data.xrpCandles.direction), TC_BG);
  tft.drawString(xrpLine, 10, 192, 1);

  drawHRule(213);
  tft.setTextColor(TC_DIM, TC_BG);
  String footer = String("Bars: ") + String(data.btcCandles.count) + String(" BTC / ") + String(data.ethCandles.count) + String(" ETH / ") + String(data.xrpCandles.count) + String(" XRP");
  if (data.btcCandles.count == 0 && !data.status.isEmpty()) {
    footer = data.status;
  }
  tft.drawString(footer, 10, 218, 1);
}

static void drawTickerRecommendationTab(const TickerData &data) {
  drawInfoCard(5, 60, 310, 143);
  tft.setTextColor(TC_ACCENT, TC_CARD);
  tft.drawString("BUY/SELL RECOMMENDATION", 13, 64, 1);
  drawHRule(74, TC_BORDER);

  drawWrapped(data.recommendation.isEmpty() ? "No data" : data.recommendation,
              13, 77, 294, 3, 1, TFT_WHITE, TC_CARD);

  const String btcLine = "BTC " + (data.btcDirection.isEmpty() ? "--" : data.btcDirection)
                       + " E:" + (data.btcEntry.isEmpty() ? "--" : data.btcEntry)
                       + " TP:" + (data.btcTakeProfit.isEmpty() ? "--" : data.btcTakeProfit);
  const String xrpLine = "XRP " + (data.xrpDirection.isEmpty() ? "--" : data.xrpDirection)
                       + " E:" + (data.xrpEntry.isEmpty() ? "--" : data.xrpEntry)
                       + " TP:" + (data.xrpTakeProfit.isEmpty() ? "--" : data.xrpTakeProfit);
  tft.setTextColor(directionColor(data.btcDirection), TC_CARD);
  tft.drawString(btcLine, 13, 110, 1);
  tft.setTextColor(directionColor(data.xrpDirection), TC_CARD);
  tft.drawString(xrpLine, 13, 126, 1);
}

static void drawTickerSentimentTab(const TickerData &data) {
  drawInfoCard(5, 60, 310, 143);
  tft.setTextColor(TC_ACCENT, TC_CARD);
  tft.drawString("SENTIMENT", 13, 64, 1);
  drawHRule(74, TC_BORDER);

  const String dir = "Direction: " + (data.direction.isEmpty() ? "--" : data.direction);
  const String cat = "Category: " + (data.category.isEmpty() ? "--" : data.category);
  tft.setTextColor(directionColor(data.direction), TC_CARD);
  tft.drawString(dir, 13, 79, 1);
  tft.setTextColor(TC_SEC, TC_CARD);
  tft.drawString(cat, 13, 90, 1);

  drawWrapped(String("D: ") + (data.daily.isEmpty() ? "--" : data.daily),
              13, 102, 294, 2, 1, TFT_WHITE, TC_CARD);
  drawWrapped(String("W: ") + (data.weekly.isEmpty() ? "--" : data.weekly),
              13, 134, 294, 2, 1, TFT_WHITE, TC_CARD);
  drawWrapped(String("M: ") + (data.monthly.isEmpty() ? "--" : data.monthly),
              13, 166, 294, 2, 1, TFT_WHITE, TC_CARD);
}

void drawTickerPage(unsigned long mElapsed) {
  maybeStartTickerFetch();

  const unsigned long now = millis();
  if (tickerViewLastSwitchMs == 0) {
    tickerViewLastSwitchMs = now;
  } else if ((now - tickerViewLastSwitchMs) >= 10000) {
    tickerViewMode = (tickerViewMode + 1) % 2;
    tickerViewLastSwitchMs = now;
    hasChangedScreen = true;
  }

  TickerData data;
  copyTickerCache(data);

  // Only repaint when data changes – prevents flicker
  const String hash = tickerDataHash(data);
  const bool screenChanged = (lastRenderedTickerScreen != currentDisplayDriver->current_cyclic_screen);
  const bool changed = screenChanged || (hash != lastRenderedTickerHash);
  if (changed) {
    tft.fillScreen(TC_BG);
    lastRenderedTickerHash = hash;
    lastRenderedTickerScreen = currentDisplayDriver->current_cyclic_screen;
  }

  tft.setTextDatum(TL_DATUM);

  if (!changed) return;  // nothing else needs repainting

  // ── HEADER ZONE (y 0–58) ─────────────────────────────────
  // Date-gated greeting (top-left)
  String greetingDate;
  String greetingTime;
  getApiPageEasternDateTime(greetingDate, greetingTime);

  tft.setTextColor(TFT_PINK, TC_BG);
  tft.drawString(getApiPageGreeting(greetingDate), 10, 5, 1);

  const String marketLabel = "MARKET SENTIMENT";
  tft.setTextColor(TC_DIM, TC_BG);
  tft.drawString(marketLabel, 310 - tft.textWidth(marketLabel, 1), 5, 1);

  // Sentiment score – large font
  const String score = data.sentiment.isEmpty() ? "--" : data.sentiment;
  tft.setTextColor(TFT_WHITE, TC_BG);
  tft.drawString(score, 10, 16, 4);

  // Direction badge (color-coded) next to score
  const int scoreW = tft.textWidth(score, 4);
  tft.setTextColor(directionColor(data.direction), TC_BG);
  tft.drawString(data.direction.isEmpty() ? "--" : data.direction,
                 10 + scoreW + 10, 24, 2);

  drawHRule(44);

  // Category (left)
  tft.setTextColor(TC_SEC, TC_BG);
  tft.drawString(data.category.isEmpty() ? "" : data.category, 10, 48, 1);

  // ── AUTO VIEW CARD (y 60+) ───────────────────────────────
  if (tickerViewMode == 0) {
    drawTickerSentimentTab(data);
  } else {
    drawTickerRecommendationTab(data);
  }

  // ── FOOTER (y 213–240) ───────────────────────────────────
  drawHRule(213);

  // Live device clock converted to Eastern Time for API page footer.
  String easternDate;
  String easternTime;
  getApiPageEasternDateTime(easternDate, easternTime);

  const bool offline = (data.status.indexOf("WiFi disconnected") >= 0 ||
                        data.status.indexOf("HTTP begin failed") >= 0 ||
                        data.status.indexOf("HTTP -") >= 0 ||
                        data.status.indexOf("No API data") >= 0);

  if (offline) {
    tft.setTextColor(TC_WARN, TC_BG);
    tft.drawString("! Connection Lost", 10, 218, 1);
  } else {
    tft.setTextColor(TC_SEC, TC_BG);
    tft.drawString("Timezone: ET", 10, 218, 1);

    // Updated: DATE HH:MM (right-aligned)
    const String updStr = "Updated: " + easternDate + " " + easternTime;
    tft.setTextColor(TC_DIM, TC_BG);
    tft.drawString(updStr, 310 - tft.textWidth(updStr, 1), 218, 1);
  }

  // Site credit – bottom row
  drawHRule(229);
  tft.setTextColor(TC_ACCENT, TC_BG);
  const String site = "seanhoene.com/crypto";
  tft.drawString(site, (320 - tft.textWidth(site, 1)) / 2, 232, 1);
}

void drawSettingsPage() {
  tft.fillRect(0, 0, 320, 240, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  tft.drawString("Settings", 8, 8, 4);
  tft.drawString("Page navigation", 8, 48, 2);
  tft.drawString("Swipe left/right", 8, 70, 2);

  tft.drawString("Auto-scroll mode", 8, 104, 2);
  tft.drawRect(10, 126, 145, 40, autoScrollEnabled ? TFT_DARKGREY : TFT_GREEN);
  tft.drawRect(165, 126, 145, 40, autoScrollEnabled ? TFT_GREEN : TFT_DARKGREY);
  tft.drawString("Manual", 46, 140, 2);
  tft.drawString("Auto", 215, 140, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Rear LED", 8, 170, 2);
  tft.drawRect(10, 192, 145, 34, Settings.rearLedEnabled ? TFT_DARKGREY : TFT_GREEN);
  tft.drawRect(165, 192, 145, 34, Settings.rearLedEnabled ? TFT_GREEN : TFT_DARKGREY);
  tft.drawString("Off", 58, 202, 2);
  tft.drawString("On", 223, 202, 2);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(String("Interval: ") + String(AUTO_SCROLL_INTERVAL_MS / 1000) + "s", 8, 232, 1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Tap a button, swipe to exit", 8, 220, 1);
}

void nextScreen() {
  currentDisplayDriver->current_cyclic_screen = (currentDisplayDriver->current_cyclic_screen + 1) % currentDisplayDriver->num_cyclic_screens;
  hasChangedScreen = true;
}

void previousScreen() {
  currentDisplayDriver->current_cyclic_screen = currentDisplayDriver->current_cyclic_screen - 1;
  if (currentDisplayDriver->current_cyclic_screen < 0) {
    currentDisplayDriver->current_cyclic_screen = currentDisplayDriver->num_cyclic_screens - 1;
  }
  hasChangedScreen = true;
}

void maybeAutoScroll(unsigned long now) {
  if (!autoScrollEnabled) {
    return;
  }

  if (currentDisplayDriver->current_cyclic_screen == SCREEN_IDX_SETTINGS) {
    return;
  }

  if (lastAutoScrollMs != 0 && (now - lastAutoScrollMs) < AUTO_SCROLL_INTERVAL_MS) {
    return;
  }

  nextScreen();
  if (currentDisplayDriver->current_cyclic_screen == SCREEN_IDX_SETTINGS) {
    nextScreen();
  }
  lastAutoScrollMs = now;
}

void getChipInfo(void){
  Serial.print("Chip: ");
  Serial.println(ESP.getChipModel());
  Serial.print("ChipRevision: ");
  Serial.println(ESP.getChipRevision());
  Serial.print("Psram size: ");
  Serial.print(ESP.getPsramSize() / 1024);
  Serial.println("KB");
  Serial.print("Flash size: ");
  Serial.print(ESP.getFlashChipSize() / 1024);
  Serial.println("KB");
  Serial.print("CPU frequency: ");
  Serial.print(ESP.getCpuFreqMHz());
  Serial.println("MHz");  
}

void esp32_2432S028R_Init(void)
{ 
  // getChipInfo();  
  tft.init();
  if (nvMem.loadConfig(&Settings))
    {      
     // Serial.print("Invert Colors: ");
     // Serial.println(Settings.invertColors);  
      invertColors = Settings.invertColors;           
    }  
  tft.invertDisplay(invertColors);
  tft.setRotation(1);    
  tft.setSwapBytes(true); // Swap the colour byte order when rendering
  if (invertColors) {
    tft.writecommand(ILI9341_GAMMASET);
    tft.writedata(2);
    delay(120);
    tft.writecommand(ILI9341_GAMMASET); //Gamma curve selected
    tft.writedata(1); 
  }
  hSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, ETOUCH_CS);
  touch.init();

  TFT_eTouchBase::Calibation calibation = { 233, 3785, 3731, 120, 2 };
  touch.setCalibration(calibation);

  // Configuring screen backlight brightness using ledcontrol channel 0.
  // Using 5000Hz in 8bit resolution, which gives 0-255 possible duty cycle setting.
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, Settings.Brightness);
 
  //background.createSprite(WIDTH, HEIGHT); // Background Sprite
  //background.setSwapBytes(true);
  //render.setDrawer(background);  // Link drawing object to background instance (so font will be rendered on background)
  //render.setLineSpaceRatio(0.9); // Espaciado entre texto

  // Load the font and check it can be read OK
  // if (render.loadFont(NotoSans_Bold, sizeof(NotoSans_Bold)))
  if (render.loadFont(DigitalNumbers, sizeof(DigitalNumbers)))
  {
    Serial.println("Initialise error");
    return;
  }
  
  pinMode(LED_PIN, OUTPUT);
  pinMode(LED_PIN_B, OUTPUT);
  pinMode(LED_PIN_G, OUTPUT);
  if (Settings.rearLedEnabled) {
    digitalWrite(LED_PIN, LOW);
    digitalWrite(LED_PIN_B, HIGH);
    digitalWrite(LED_PIN_G, HIGH);
  } else {
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(LED_PIN_B, HIGH);
    digitalWrite(LED_PIN_G, HIGH);
  }
  pData.bestDifficulty = "0";
  pData.workersHash = "0";
  pData.workersCount = 0;

  if (tickerDataMutex == nullptr) {
    tickerDataMutex = xSemaphoreCreateMutex();
  }
  tickerCache.status = "Waiting...";
  autoScrollEnabled = false;
  lastAutoScrollMs = millis();
  tickerLastFetchAttemptMs = 0;
  tickerFetchInProgress = false;
  //Serial.println("=========== Fim Display ==============") ;
}

void esp32_2432S028R_AlternateScreenState(void)
{
  Serial.println("Switching display state");
  int screen_state_duty = ledcRead(0);
  // Switching the duty cycle for the ledc channel, where the TFT_BL pin is attached.
  if (screen_state_duty > 0) {
    ledcWrite(0, 0);
  } else {
    ledcWrite(0, Settings.Brightness);
  }
}

void esp32_2432S028R_AlternateRotation(void)
{
  tft.setRotation( flipRotation(tft.getRotation()) );
  hasChangedScreen = true;
}

bool bottomScreenBlue = true;

void printheap(){
  Serial.print("$$ Free Heap:");
  Serial.println(ESP.getFreeHeap()); 
  // Serial.printf("### stack WMark usage: %d\n", uxTaskGetStackHighWaterMark(NULL));
}

bool createBackgroundSprite(int16_t wdt, int16_t hgt){  // Set the background and link the render, used multiple times to fit in heap
  background.createSprite(wdt, hgt) ; //Background Sprite
  // printheap();
  if (background.created()) {
      background.setColorDepth(16);
      background.setSwapBytes(true);
      render.setDrawer(background); // Link drawing object to background instance (so font will be rendered on background)
      render.setLineSpaceRatio(0.9);      
  } else {
    Serial.println("#### Sprite Error ####");
    Serial.printf("Size w:%d h:%d \n", wdt, hgt);
    printheap();
  }
  return background.created();
}

extern unsigned long mPoolUpdate;

void printPoolData(){
  if ((hasChangedScreen) || (mPoolUpdate == 0) || (millis() - mPoolUpdate > UPDATE_POOL_min * 60 * 1000)){     
      if (Settings.PoolAddress != "tn.vkbit.com") { 
          pData = getPoolData();             
          background.createSprite(320,50); //Background Sprite
          if (!background.created()) {    
            Serial.println("###### POOL SPRITE ERROR ######");
          // Serial.printf("Pool data W:%d H:%s D:%s\n", pData.workersCount, pData.workersHash, pData.bestDifficulty);
            printheap();        
          }       
          background.setSwapBytes(true);
          if (bottomScreenBlue) {
            background.pushImage(0, -20, 320, 70, bottonPoolScreen);
            tft.pushImage(0,170,320,20,bottonPoolScreen);      
          } else {
            background.pushImage(0, -20, 320, 70, bottonPoolScreen_g);
            tft.pushImage(0,170,320,20,bottonPoolScreen_g);
          }
                
          render.setDrawer(background); // Link drawing object to background instance (so font will be rendered on background)
          render.setLineSpaceRatio(1);
          
          render.setFontSize(24);
          render.cdrawString(String(pData.workersCount).c_str(), 157, 16, TFT_BLACK);
          render.setFontSize(18);
          render.setAlignment(Align::BottomRight);
          render.cdrawString(pData.workersHash.c_str(), 265, 14, TFT_BLACK);
          render.setAlignment(Align::BottomLeft);
          render.cdrawString(pData.bestDifficulty.c_str(), 54, 14, TFT_BLACK);
          background.pushSprite(0,190);      
          background.deleteSprite();
      } else {
        pData.bestDifficulty = "TESTNET";
        pData.workersHash = "TESTNET";
        pData.workersCount = 1;
        tft.fillRect(0,170,320,70, TFT_DARKGREEN);        
        background.createSprite(320,40); //Background Sprite
        background.fillSprite(TFT_DARKGREEN);
          if (!background.created()) {    
            Serial.println("###### POOL SPRITE ERROR ######");
          // Serial.printf("Pool data W:%d H:%s D:%s\n", pData.workersCount, pData.workersHash, pData.bestDifficulty);
            printheap();        
          }
        background.setFreeFont(FF24);
        background.setTextDatum(TL_DATUM);
        background.setTextSize(1);
        background.setTextColor(TFT_WHITE, TFT_DARKGREEN);        
        background.drawString("TESTNET", 50, 0, GFXFF);
        background.pushSprite(0,185);  
        mPoolUpdate = millis();
        Serial.println("Testnet");
        background.deleteSprite();
      }
  }
}



void esp32_2432S028R_MinerScreen(unsigned long mElapsed)
{
  mining_data data = getMiningData(mElapsed);

  printPoolData();

  if (hasChangedScreen) tft.pushImage(0, 0, initWidth, initHeight, MinerScreen);
    
  hasChangedScreen = false; 
 
  int wdtOffset = 190;
  // Recreate sprite to the right side of the screen
  createBackgroundSprite(WIDTH-5, HEIGHT-7);
  //Print background screen    
  background.pushImage(-190, 0, MinerWidth, MinerHeight, MinerScreen);
  
  // Total hashes
  render.setFontSize(18);
  render.rdrawString(data.totalMHashes.c_str(), 268-wdtOffset, 138, TFT_BLACK);

  // Block templates
  render.setFontSize(18);
  render.setAlignment(Align::TopLeft);
  render.drawString(data.templates.c_str(), 189-wdtOffset, 20, 0xDEDB);
  // Best diff
  render.drawString(data.bestDiff.c_str(), 189-wdtOffset, 48, 0xDEDB);
  // 32Bit shares
  render.setFontSize(18);
  render.drawString(data.completedShares.c_str(), 189-wdtOffset, 76, 0xDEDB);
  // Hores
  render.setFontSize(14);
  render.rdrawString(data.timeMining.c_str(), 315-wdtOffset, 104, 0xDEDB);

  // Valid Blocks
  render.setFontSize(24);
  render.setAlignment(Align::TopCenter);
  render.drawString(data.valids.c_str(), 290-wdtOffset, 56, 0xDEDB);

  // Print Temp
  render.setFontSize(10);
  render.rdrawString(data.temp.c_str(), 239-wdtOffset, 1, TFT_BLACK);

  render.setFontSize(4);
  render.rdrawString(String(0).c_str(), 244-wdtOffset, 3, TFT_BLACK);

  // Print Hour
  render.setFontSize(10);
  render.rdrawString(data.currentTime.c_str(), 286-wdtOffset, 1, TFT_BLACK);

  // Push prepared background to screen
  background.pushSprite(190, 0);

  // Delete sprite to free the memory heap
  background.deleteSprite();   
  // printheap();

   //Serial.println("=========== Mining Display ==============") ;
  // Create background sprite to print data at once
  createBackgroundSprite(WIDTH-7, HEIGHT-100); // initHeight); //Background Sprite
  //Print background screen    
  background.pushImage(0, -90, MinerWidth, MinerHeight, MinerScreen);

  // Hashrate 
  render.setFontSize(35);
  render.setCursor(19, 118);
  render.setFontColor(TFT_BLACK);
  render.rdrawString(data.currentHashRate.c_str(), 118, 114-90, TFT_BLACK);
  
  // Push prepared background to screen
  background.pushSprite(0, 90);
  
  // Delete sprite to free the memory heap
  background.deleteSprite();  

  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                data.completedShares.c_str(), data.totalKHashes.c_str(), data.currentHashRate.c_str()); 
   
  #ifdef DEBUG_MEMORY
    // Print heap
    printheap();
  #endif
}

void esp32_2432S028R_ClockScreen(unsigned long mElapsed)
{
  if (hasChangedScreen) tft.pushImage(0, 0, minerClockWidth, minerClockHeight, minerClockScreen);
  
  printPoolData();

  hasChangedScreen = false;

  clock_data data = getClockData(mElapsed);

 // Create background sprite to print data at once
  createBackgroundSprite(270,36);

  // Print background screen
  background.pushImage(0, -130, minerClockWidth, minerClockHeight, minerClockScreen);
  // Hashrate
  render.setFontSize(25);
  render.setFontColor(TFT_BLACK);
  render.rdrawString(data.currentHashRate.c_str(), 95, 0, TFT_BLACK);

  // Print BlockHeight
  render.setFontSize(18);
  render.rdrawString(data.blockHeight.c_str(), 254, 9, TFT_BLACK);

  // Push prepared background to screen
  background.pushSprite(0, 130);
  // Delete sprite to free the memory heap
  background.deleteSprite(); 

  createBackgroundSprite(169,105);
  // Print background screen
  background.pushImage(-130, -3, minerClockWidth, minerClockHeight, minerClockScreen);
  
  // Print BTC Price
  background.setFreeFont(FSSB9);
  background.setTextSize(1);
  background.setTextDatum(TL_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(data.btcPrice.c_str(), 202-130, 0, GFXFF);
 
  // Print Hour
  background.setFreeFont(FF23);
  background.setTextSize(2);
  background.setTextColor(0xDEDB, TFT_BLACK);
  background.drawString(data.currentTime.c_str(), 0, 50, GFXFF);
 
  // Push prepared background to screen
  background.pushSprite(130, 3);

  // Delete sprite to free the memory heap
  background.deleteSprite();   

  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                data.completedShares.c_str(), data.totalKHashes.c_str(), data.currentHashRate.c_str());

  #ifdef DEBUG_MEMORY
  // Print heap
  printheap();
  #endif
}

void esp32_2432S028R_GlobalHashScreen(unsigned long mElapsed)
{
  if (hasChangedScreen) tft.pushImage(0, 0, globalHashWidth, globalHashHeight, globalHashScreen);
  
  printPoolData();
  
  hasChangedScreen = false;
  
  coin_data data = getCoinData(mElapsed);

  // Create background sprite to print data at once
  createBackgroundSprite(169,105);
  // Print background screen
  background.pushImage(-160, -3, minerClockWidth, minerClockHeight, globalHashScreen);
  
  // Print BTC Price
  background.setFreeFont(FSSB9);
  background.setTextSize(1);
  background.setTextDatum(TL_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(data.btcPrice.c_str(), 198-160, 0, GFXFF);
  // Print Hour
  background.setFreeFont(FSSB9);
  background.setTextSize(1);
  background.setTextDatum(TL_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(data.currentTime.c_str(), 268-160, 0, GFXFF);

  // Print Last Pool Block
  background.setFreeFont(FSS9);
  background.setTextDatum(TR_DATUM);
  background.setTextColor(0x9C92);
  background.drawString(data.halfHourFee.c_str(), 302-160, 49, GFXFF);

  // Print Difficulty
  background.setFreeFont(FSS9);
  background.setTextDatum(TR_DATUM);
  background.setTextColor(0x9C92);
  background.drawString(data.netwrokDifficulty.c_str(), 302-160, 85, GFXFF);
  // Push prepared background to screen
  background.pushSprite(160, 3);
  // Delete sprite to free the memory heap
  background.deleteSprite();   

 // Create background sprite to print data at once
  createBackgroundSprite(280,30);
  // Print background screen
  background.pushImage(0, -139, minerClockWidth, minerClockHeight, globalHashScreen);
  //background.fillSprite(TFT_CYAN);
  // Print Global Hashrate
  render.setFontSize(17);
  render.rdrawString(data.globalHashRate.c_str(), 274, 145-139, TFT_BLACK);

  // Draw percentage rectangle
  int x2 = 2 + (138 * data.progressPercent / 100);
  background.fillRect(2, 149-139, x2, 168, 0xDEDB);

  // Print Remaining BLocks
  background.setTextFont(FONT2);
  background.setTextSize(1); 
  background.setTextDatum(MC_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(data.remainingBlocks.c_str(), 72, 159-139, FONT2);

  // Push prepared background to screen
  background.pushSprite(0, 139);
  // Delete sprite to free the memory heap
  background.deleteSprite();   

 // Create background sprite to print data at once
  createBackgroundSprite(140,40);
  // Print background screen
  background.pushImage(-5, -100, minerClockWidth, minerClockHeight, globalHashScreen);
  //background.fillSprite(TFT_CYAN);
  // Print BlockHeight
  render.setFontSize(28);
  render.rdrawString(data.blockHeight.c_str(), 140-5, 104-100, 0xDEDB);

  // Push prepared background to screen
  background.pushSprite(5, 100);
  // Delete sprite to free the memory heap
  background.deleteSprite();   

  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                data.completedShares.c_str(), data.totalKHashes.c_str(), data.currentHashRate.c_str());

  #ifdef DEBUG_MEMORY
  // Print heap
  printheap();
  #endif
}
void esp32_2432S028R_BTCprice(unsigned long mElapsed)
{
  if (hasChangedScreen) tft.pushImage(0, 0, priceScreenWidth, priceScreenHeight, priceScreen);
  printPoolData();
  hasChangedScreen = false;

  clock_data data = getClockData(mElapsed);

 // Create background sprite to print data at once
  createBackgroundSprite(270,36);

  // Print background screen
  background.pushImage(0, -130, priceScreenWidth, priceScreenHeight, priceScreen);
  // Hashrate
  render.setFontSize(25);
  render.setFontColor(TFT_BLACK);
  render.rdrawString(data.currentHashRate.c_str(), 95, 0, TFT_BLACK);

  // Print BlockHeight
  render.setFontSize(18);
  render.rdrawString(data.blockHeight.c_str(), 254, 9, TFT_WHITE);

  // Push prepared background to screen
  background.pushSprite(0, 130);
  // Delete sprite to free the memory heap
  background.deleteSprite(); 

  createBackgroundSprite(180,105);
  // Print background screen
  background.pushImage(-130, -3, priceScreenWidth, priceScreenHeight, priceScreen);
  
  // Print Hour
  background.setFreeFont(FSSB9);
  background.setTextSize(1);
  background.setTextDatum(TL_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(data.currentTime.c_str(), 202-130, 0, GFXFF);
 
  // Print BTC Price
  background.setFreeFont(FF24);
  background.setTextDatum(TL_DATUM);
  background.setTextSize(1);
  background.setTextColor(0xDEDB, TFT_BLACK);
  background.drawString(data.btcPrice.c_str(), 0, 50, GFXFF);
 
  // Push prepared background to screen
  background.pushSprite(130, 3);

  // Delete sprite to free the memory heap
  background.deleteSprite();   

  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                data.completedShares.c_str(), data.totalKHashes.c_str(), data.currentHashRate.c_str());

  #ifdef DEBUG_MEMORY
  // Print heap
  printheap();
  #endif
}

void esp32_2432S028R_TickerScreen(unsigned long mElapsed)
{
  drawTickerPage(mElapsed);
}

void esp32_2432S028R_CandleScreen(unsigned long mElapsed)
{
  drawCandlePage(mElapsed);
}

void esp32_2432S028R_SettingsScreen(unsigned long mElapsed)
{
  drawSettingsPage();
}

void esp32_2432S028R_LoadingScreen(void)
{
  tft.fillScreen(TFT_BLACK);
  tft.pushImage(0, 33, initWidth, initHeight, initScreen);
  tft.setTextColor(TFT_BLACK);
  tft.drawString(CURRENT_VERSION, 24, 147, FONT2);
  // delay(2000);
  // tft.fillScreen(TFT_BLACK);
  // tft.pushImage(0, 0, initWidth, initHeight, MinerScreen);
}

void esp32_2432S028R_SetupScreen(void)
{
  tft.fillScreen(TFT_BLACK);
  tft.pushImage(0, 33, setupModeWidth, setupModeHeight, setupModeScreen);
}

void esp32_2432S028R_AnimateCurrentScreen(unsigned long frame)
{
}

// Variables para controlar el parpadeo con millis()
unsigned long previousMillis = 0;
unsigned long touchGestureLastMs = 0;
char currentScreen = 0;
bool touchTracking = false;
int16_t touchStartX = 0;
int16_t touchStartY = 0;
int16_t touchLastX = 0;
int16_t touchLastY = 0;

const int SWIPE_THRESHOLD_PX = 45;
const unsigned long TOUCH_GESTURE_DEBOUNCE_MS = 120;

void esp32_2432S028R_DoLedStuff(unsigned long frame)
{
  unsigned long currentMillis = millis();    
  int16_t t_x, t_y;
  bool pressed = touch.getXY(t_x, t_y);

  if (pressed) {
    if (!touchTracking) {
      touchTracking = true;
      touchStartX = t_x;
      touchStartY = t_y;
    }
    touchLastX = t_x;
    touchLastY = t_y;
  } else if (touchTracking && (currentMillis - touchGestureLastMs >= TOUCH_GESTURE_DEBOUNCE_MS)) {
    int dx = touchLastX - touchStartX;
    int dy = touchLastY - touchStartY;

    if (abs(dx) > SWIPE_THRESHOLD_PX && abs(dx) > abs(dy)) {
      if (dx < 0) {
        nextScreen();
      } else {
        previousScreen();
      }
      lastAutoScrollMs = currentMillis;
    } else {
      // Treat as tap for settings toggles and existing utility controls.
      if (currentDisplayDriver->current_cyclic_screen == SCREEN_IDX_SETTINGS) {
        if ((touchLastY > 126) && (touchLastY < 166)) {
          if (touchLastX < 160) {
            autoScrollEnabled = false;
          } else {
            autoScrollEnabled = true;
          }
          lastAutoScrollMs = currentMillis;
          hasChangedScreen = true;
        } else if ((touchLastY > 192) && (touchLastY < 226)) {
          Settings.rearLedEnabled = touchLastX >= 160;
          nvMem.saveConfig(&Settings);
          hasChangedScreen = true;
        }
      } else if (((touchLastX > 109) && (touchLastX < 211)) && ((touchLastY > 185) && (touchLastY < 241))) {
        bottomScreenBlue ^= true;
        hasChangedScreen = true;
      } else if ((touchLastX > 235) && ((touchLastY > 0) && (touchLastY < 16))) {
        esp32_2432S028R_AlternateScreenState();
      }
    }

    touchTracking = false;
    touchGestureLastMs = currentMillis;
  }

  maybeAutoScroll(currentMillis);

    if (currentScreen != currentDisplayDriver->current_cyclic_screen) hasChangedScreen ^= true;
    currentScreen = currentDisplayDriver->current_cyclic_screen;

  if (!Settings.rearLedEnabled) {
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(LED_PIN_B, HIGH);
    digitalWrite(LED_PIN_G, HIGH);
  } else {
    switch (mMonitor.NerdStatus)
    {
    case NM_waitingConfig:
      digitalWrite(LED_PIN, LOW); // LED encendido de forma continua
      break;

    case NM_Connecting:
      if (currentMillis - previousMillis >= 500)
      { // 0.5sec blink
        previousMillis = currentMillis;
        // Serial.print("C");
        digitalWrite(LED_PIN, HIGH);
        digitalWrite(LED_PIN_B, !digitalRead(LED_PIN)); // Cambia el estado del LED
      }
      break;

    case NM_hashing:
      if (currentMillis - previousMillis >= 500)
      { // 0.1sec blink
        // Serial.print("h");
        previousMillis = currentMillis;
        digitalWrite(LED_PIN_B, HIGH);
        digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Cambia el estado del LED      
      }
      break;
    }
  }
  

}

CyclicScreenFunction esp32_2432S028RCyclicScreens[] = {
  esp32_2432S028R_TickerScreen,
  esp32_2432S028R_CandleScreen,
  esp32_2432S028R_SettingsScreen
};

DisplayDriver esp32_2432S028RDriver = {
    esp32_2432S028R_Init,
    esp32_2432S028R_AlternateScreenState,
    esp32_2432S028R_AlternateRotation,
    esp32_2432S028R_LoadingScreen,
    esp32_2432S028R_SetupScreen,
    esp32_2432S028RCyclicScreens,
    esp32_2432S028R_AnimateCurrentScreen,
    esp32_2432S028R_DoLedStuff,
    SCREENS_ARRAY_SIZE(esp32_2432S028RCyclicScreens),
    0,
    WIDTH,
    HEIGHT};
#endif
