#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "mbedtls/md.h"
#include "HTTPClient.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <list>
#include "mining.h"
#include "utils.h"
#include "monitor.h"
#include "drivers/storage/storage.h"
#include "drivers/devices/device.h"

extern uint32_t templates;
extern uint32_t hashes;
extern uint32_t Mhashes;
extern uint32_t totalKHashes;
extern uint32_t elapsedKHs;
extern uint64_t upTime;

extern uint32_t shares; // increase if blockhash has 32 bits of zeroes
extern uint32_t valids; // increased if blockhash <= targethalfshares

extern double best_diff; // track best diff

extern monitor_data mMonitor;

//from saved config
extern TSettings Settings; 
bool invertColors = false;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 3600, 60000);
unsigned int bitcoin_price=0;
String current_block = "793261";
global_data gData;
pool_data pData;
unsigned long mGlobalUpdate = 0;
unsigned long mHeightUpdate = 0;
unsigned long mBTCUpdate = 0;
unsigned long mPoolUpdate = 0;
String poolAPIUrl;
volatile bool networkFetchInProgress = false;

SemaphoreHandle_t monitorDataMutex = nullptr;
static volatile bool monitorFetchInProgress = false;

void lockMonitorData() {
    if (monitorDataMutex != nullptr) {
        xSemaphoreTake(monitorDataMutex, portMAX_DELAY);
    }
}

void unlockMonitorData() {
    if (monitorDataMutex != nullptr) {
        xSemaphoreGive(monitorDataMutex);
    }
}

void runUpdateGlobalData(void){
    mGlobalUpdate = millis(); // Set early to prevent spamming if request fails/blocks
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
        
    //Make first API call to get global hash and current difficulty
    HTTPClient http;
    http.setTimeout(8000);
    try {
    String url1 = String(getGlobalHash);
    http.begin(url1);
    http.addHeader("User-Agent", "NerdMinerV2-Monitor/1.0");
    http.addHeader("Accept", "application/json");
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, payload);
        String temp = "";
        String globalHashVal = "";
        String difficultyVal = "";
        if (doc.containsKey("currentHashrate")) temp = String(doc["currentHashrate"].as<float>());
        if(temp.length()>18 + 3) //Exahashes more than 18 digits + 3 digits decimals
          globalHashVal = temp.substring(0,temp.length()-18 - 3);
        if (doc.containsKey("currentDifficulty")) temp = String(doc["currentDifficulty"].as<float>());
        if(temp.length()>10 + 3){ //Terahash more than 10 digits + 3 digit decimals
          temp = temp.substring(0,temp.length()-10 - 3);
          difficultyVal = temp.substring(0,temp.length()-2) + "." + temp.substring(temp.length()-2,temp.length()) + "T";
        }
        doc.clear();

        lockMonitorData();
        if (!globalHashVal.isEmpty()) gData.globalHash = globalHashVal;
        if (!difficultyVal.isEmpty()) gData.difficulty = difficultyVal;
        unlockMonitorData();

        mGlobalUpdate = millis();
    }
    http.end();

    //Make third API call to get fees
    String url2 = String(getFees);
    http.begin(url2);
    http.addHeader("User-Agent", "NerdMinerV2-Monitor/1.0");
    http.addHeader("Accept", "application/json");
    int httpCode2 = http.GET();

    if (httpCode2 == HTTP_CODE_OK) {
        String payload = http.getString();
        
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, payload);
        int halfHourFee = 0;
#ifdef SCREEN_FEES_ENABLE
        int fastestFee = 0;
        int hourFee = 0;
        int economyFee = 0;
        int minimumFee = 0;
#endif
        if (doc.containsKey("halfHourFee")) halfHourFee = doc["halfHourFee"].as<int>();
#ifdef SCREEN_FEES_ENABLE
        if (doc.containsKey("fastestFee"))  fastestFee = doc["fastestFee"].as<int>();
        if (doc.containsKey("hourFee"))     hourFee = doc["hourFee"].as<int>();
        if (doc.containsKey("economyFee"))  economyFee = doc["economyFee"].as<int>();
        if (doc.containsKey("minimumFee"))  minimumFee = doc["minimumFee"].as<int>();
#endif
        doc.clear();

        lockMonitorData();
        if (halfHourFee != 0) gData.halfHourFee = halfHourFee;
#ifdef SCREEN_FEES_ENABLE
        if (fastestFee != 0) gData.fastestFee = fastestFee;
        if (hourFee != 0) gData.hourFee = hourFee;
        if (economyFee != 0) gData.economyFee = economyFee;
        if (minimumFee != 0) gData.minimumFee = minimumFee;
#endif
        unlockMonitorData();

        mGlobalUpdate = millis();
    }
    
    http.end();
    } catch(...) {
      Serial.println("Global data HTTP error caught");
      http.end();
    }
}

void runUpdateBlockHeight(void) {
    mHeightUpdate = millis(); // Set early to prevent spamming if request fails/blocks
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
        
    HTTPClient http;
    http.setTimeout(8000);
    try {
    String url = String(getHeightAPI);
    http.begin(url);
    http.addHeader("User-Agent", "NerdMinerV2-Monitor/1.0");
    http.addHeader("Accept", "application/json");
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        payload.trim();

        lockMonitorData();
        current_block = payload;
        unlockMonitorData();

        mHeightUpdate = millis();
    }        
    http.end();
    } catch(...) {
      Serial.println("Height HTTP error caught");
      http.end();
    }
}

void runUpdateBTCprice(void) {
    mBTCUpdate = millis(); // Set early to prevent spamming if request fails/blocks
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    
    HTTPClient http;
    http.setTimeout(8000);

    try {
    String url = String(getBTCAPI);
    http.begin(url);
    http.addHeader("User-Agent", "NerdMinerV2-Monitor/1.0");
    http.addHeader("Accept", "application/json");
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();

        DynamicJsonDocument doc(1024);
        deserializeJson(doc, payload);
      
        if (doc.containsKey("bitcoin") && doc["bitcoin"].containsKey("usd")) {
            unsigned int price = doc["bitcoin"]["usd"];
            lockMonitorData();
            bitcoin_price = price;
            unlockMonitorData();
        }

        doc.clear();

        mBTCUpdate = millis();
    }
    
    http.end();
    } catch(...) {
      Serial.println("BTC price HTTP error caught");
      http.end();
    }
}

void runUpdatePoolData(void) {
    mPoolUpdate = millis(); // Set early to prevent spamming if request fails/blocks
    if (WiFi.status() != WL_CONNECTED) return;
    
    HTTPClient http;
    http.setTimeout(8000);        
    try {          
      String btcWallet = Settings.BtcWallet;
      if (btcWallet.indexOf(".")>0) btcWallet = btcWallet.substring(0,btcWallet.indexOf("."));
      String url;
#ifdef SCREEN_WORKERS_ENABLE
      Serial.println("Pool API : " + poolAPIUrl+btcWallet);
      url = poolAPIUrl+btcWallet;
#else
      url = String(getPublicPool)+btcWallet;
#endif

      http.begin(url);
      http.addHeader("User-Agent", "NerdMinerV2-Monitor/1.0");
      http.addHeader("Accept", "application/json");
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
          String payload = http.getString();
          DynamicJsonDocument filter(300);
          filter["bestDifficulty"] = true;
          filter["workersCount"] = true;
          filter["workers"][0]["sessionId"] = true;
          filter["workers"][0]["hashRate"] = true;
          DynamicJsonDocument doc(2048);
          deserializeJson(doc, payload, DeserializationOption::Filter(filter));
          
          int workersCount = 0;
          String workersHash = "0";
          String bestDifficulty = "0";
          
          if (doc.containsKey("workersCount")) workersCount = doc["workersCount"].as<int>();
          if (doc.containsKey("workers") && doc["workers"].is<JsonArray>()) {
              const JsonArray workers = doc["workers"].as<JsonArray>();
              float totalhashs = 0;
              for (const JsonObject worker : workers) {
                  totalhashs += worker["hashRate"].as<double>();
              }
              char totalhashs_s[16] = {0};
              suffix_string(totalhashs, totalhashs_s, 16, 0);
              workersHash = String(totalhashs_s);
          } else {
              workersHash = "0";
          }

          double temp;
          if (doc.containsKey("bestDifficulty")) {
              temp = doc["bestDifficulty"].as<double>();            
              char best_diff_string[16] = {0};
              suffix_string(temp, best_diff_string, 16, 0);
              bestDifficulty = String(best_diff_string);
          }
          doc.clear();
          
          lockMonitorData();
          pData.workersCount = workersCount;
          pData.workersHash = workersHash;
          pData.bestDifficulty = bestDifficulty;
          unlockMonitorData();
          
          mPoolUpdate = millis();
          Serial.println("\n####### Pool Data OK!");               
      } else {
          Serial.println("\n####### Pool Data HTTP Error!");    
          lockMonitorData();
          pData.bestDifficulty = "P";
          pData.workersHash = "E";
          pData.workersCount = 0;
          unlockMonitorData();
      }
      http.end();
    } catch(...) {
      Serial.println("####### Pool Error!");          
      lockMonitorData();
      pData.bestDifficulty = "P";
      pData.workersHash = "Error";
      pData.workersCount = 0;
      unlockMonitorData();
      http.end();
    } 
}

void monitorFetchTask(void *param) {
    const unsigned long now = millis();
    
    // Check global due
    if ((mGlobalUpdate == 0) || (now - mGlobalUpdate > UPDATE_Global_min * 60 * 1000)) {
       runUpdateGlobalData();
    }
    
    // Check height due
    if ((mHeightUpdate == 0) || (now - mHeightUpdate > UPDATE_Height_min * 60 * 1000)) {
       runUpdateBlockHeight();
    }
    
    // Check BTC due
    if ((mBTCUpdate == 0) || (now - mBTCUpdate > UPDATE_BTC_min * 60 * 1000)) {
       runUpdateBTCprice();
    }
    
    // Check pool due
    if ((mPoolUpdate == 0) || (now - mPoolUpdate > UPDATE_POOL_min * 60 * 1000)) {
       runUpdatePoolData();
    }
    
    monitorFetchInProgress = false;
    networkFetchInProgress = false;
    vTaskDelete(nullptr);
}

void maybeStartMonitorFetch() {
    if (monitorFetchInProgress || networkFetchInProgress) {
        return;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    
    const unsigned long now = millis();
    bool globalDue = (mGlobalUpdate == 0) || (now - mGlobalUpdate > UPDATE_Global_min * 60 * 1000);
    bool heightDue = (mHeightUpdate == 0) || (now - mHeightUpdate > UPDATE_Height_min * 60 * 1000);
    bool btcDue = (mBTCUpdate == 0) || (now - mBTCUpdate > UPDATE_BTC_min * 60 * 1000);
    bool poolDue = (mPoolUpdate == 0) || (now - mPoolUpdate > UPDATE_POOL_min * 60 * 1000);
    
    if (globalDue || heightDue || btcDue || poolDue) {
        monitorFetchInProgress = true;
        networkFetchInProgress = true;
        BaseType_t created = xTaskCreatePinnedToCore(monitorFetchTask, "MonitorFetch", 8192, nullptr, 1, nullptr, 1);
        if (created != pdPASS) {
            monitorFetchInProgress = false;
            networkFetchInProgress = false;
        }
    }
}

void setup_monitor(void){
    /******** TIME ZONE SETTING *****/

    timeClient.begin();
    
    // Adjust offset depending on your zone
    // GMT +2 in seconds (zona horaria de Europa Central)
    timeClient.setTimeOffset(3600 * Settings.Timezone);

    Serial.println("TimeClient setup done");
#ifdef SCREEN_WORKERS_ENABLE
    poolAPIUrl = getPoolAPIUrl();
    Serial.println("poolAPIUrl: " + poolAPIUrl);
#endif

    if (monitorDataMutex == nullptr) {
        monitorDataMutex = xSemaphoreCreateMutex();
    }

    // Delay the initial API requests to prevent blocking boot/UI tasks
    mGlobalUpdate = millis() - (UPDATE_Global_min * 60 * 1000) + 15 * 1000;  // 15 seconds delay
    mHeightUpdate = millis() - (UPDATE_Height_min * 60 * 1000) + 30 * 1000;  // 30 seconds delay
    mBTCUpdate = millis() - (UPDATE_BTC_min * 60 * 1000) + 45 * 1000;        // 45 seconds delay
    mPoolUpdate = millis() - (UPDATE_POOL_min * 60 * 1000) + 60 * 1000;      // 60 seconds delay
}


void updateGlobalData(void){
    maybeStartMonitorFetch();
}


String getBlockHeight(void){
    maybeStartMonitorFetch();
    String height;
    lockMonitorData();
    height = current_block;
    unlockMonitorData();
    return height;
}


String getBTCprice(void){
    maybeStartMonitorFetch();
    unsigned int price;
    lockMonitorData();
    price = bitcoin_price;
    unlockMonitorData();
    
    static char price_buffer[16];
    snprintf(price_buffer, sizeof(price_buffer), "$%u", price);
    return String(price_buffer);
}

unsigned long mTriggerUpdate = 0;
unsigned long initialMillis = millis();
unsigned long initialTime = 0;

void getTime(unsigned long* currentHours, unsigned long* currentMinutes, unsigned long* currentSeconds){
  
  //Check if need an NTP call to check current time
  if((mTriggerUpdate == 0) || (millis() - mTriggerUpdate > UPDATE_PERIOD_h * 60 * 60 * 1000)){ //60 sec. * 60 min * 1000ms
    if(WiFi.status() == WL_CONNECTED) {
        if(timeClient.update()) mTriggerUpdate = millis(); //NTP call to get current time
        initialTime = timeClient.getEpochTime(); // Guarda la hora inicial (en segundos desde 1970)
        Serial.print("TimeClient NTPupdateTime ");
    }
  }

  unsigned long elapsedTime = (millis() - mTriggerUpdate) / 1000; // Tiempo transcurrido en segundos
  unsigned long currentTime = initialTime + elapsedTime; // La hora actual

  // convierte la hora actual en horas, minutos y segundos
  *currentHours = currentTime % 86400 / 3600;
  *currentMinutes = currentTime % 3600 / 60;
  *currentSeconds = currentTime % 60;
}

String getDate(){
  
  unsigned long elapsedTime = (millis() - mTriggerUpdate) / 1000; // Tiempo transcurrido en segundos
  unsigned long currentTime = initialTime + elapsedTime; // La hora actual

  // Convierte la hora actual (epoch time) en una estructura tm
  struct tm *tm = localtime((time_t *)&currentTime);

  int year = tm->tm_year + 1900; // tm_year es el número de años desde 1900
  int month = tm->tm_mon + 1;    // tm_mon es el mes del año desde 0 (enero) hasta 11 (diciembre)
  int day = tm->tm_mday;         // tm_mday es el día del mes

  char currentDate[20];
  sprintf(currentDate, "%02d/%02d/%04d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);

  return String(currentDate);
}

String getTime(void){
  unsigned long currentHours, currentMinutes, currentSeconds;
  getTime(&currentHours, &currentMinutes, &currentSeconds);

  char LocalHour[10];
  sprintf(LocalHour, "%02d:%02d", currentHours, currentMinutes);
  
  String mystring(LocalHour);
  return LocalHour;
}

enum EHashRateScale
{
  HashRateScale_99KH,
  HashRateScale_999KH,
  HashRateScale_9MH
};

static EHashRateScale s_hashrate_scale = HashRateScale_99KH;
static uint32_t s_skip_first = 3;
static double s_top_hashrate = 0.0;

static std::list<double> s_hashrate_avg_list;
static double s_hashrate_summ = 0.0;
static uint8_t s_hashrate_recalc = 0;

String getCurrentHashRate(unsigned long mElapsed)
{
  double hashrate = (double)elapsedKHs * 1000.0 / (double)mElapsed;

  s_hashrate_summ += hashrate;
  s_hashrate_avg_list.push_back(hashrate);
  if (s_hashrate_avg_list.size() > 10)
  {
    s_hashrate_summ -= s_hashrate_avg_list.front();
    s_hashrate_avg_list.pop_front();
  }

  ++s_hashrate_recalc;
  if (s_hashrate_recalc == 0)
  {
    s_hashrate_summ = 0.0;
    for (auto itt = s_hashrate_avg_list.begin(); itt != s_hashrate_avg_list.end(); ++itt)
      s_hashrate_summ += *itt;
  }

  double avg_hashrate = s_hashrate_summ / (double)s_hashrate_avg_list.size();
  if (avg_hashrate < 0.0)
    avg_hashrate = 0.0;

  if (s_skip_first > 0)
  {
    s_skip_first--;
  } else
  {
    if (avg_hashrate > s_top_hashrate)
    {
      s_top_hashrate = avg_hashrate;
      if (avg_hashrate > 999.9)
        s_hashrate_scale = HashRateScale_9MH;
      else if (avg_hashrate > 99.9)
        s_hashrate_scale = HashRateScale_999KH;
    }
  }

  switch (s_hashrate_scale)
  {
    case HashRateScale_99KH:
      return String(avg_hashrate, 2);
    case HashRateScale_999KH:
      return String(avg_hashrate, 1);
    default:
      return String((int)avg_hashrate );
  }
}

mining_data getMiningData(unsigned long mElapsed)
{
  mining_data data;

  char best_diff_string[16] = {0};
  suffix_string(best_diff, best_diff_string, 16, 0);

  char timeMining[15] = {0};
  uint64_t tm = upTime;
  int secs = tm % 60;
  tm /= 60;
  int mins = tm % 60;
  tm /= 60;
  int hours = tm % 24;
  int days = tm / 24;
  sprintf(timeMining, "%01d  %02d:%02d:%02d", days, hours, mins, secs);

  data.completedShares = shares;
  data.totalMHashes = Mhashes;
  data.totalKHashes = totalKHashes;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  data.templates = templates;
  data.bestDiff = best_diff_string;
  data.timeMining = timeMining;
  data.valids = valids;
  data.temp = String(temperatureRead(), 0);
  data.currentTime = getTime();

  return data;
}

clock_data getClockData(unsigned long mElapsed)
{
  maybeStartMonitorFetch();
  
  clock_data data;

  lockMonitorData();
  data.completedShares = shares;
  data.totalKHashes = totalKHashes;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  
  char price_buffer[16];
  snprintf(price_buffer, sizeof(price_buffer), "$%u", bitcoin_price);
  data.btcPrice = String(price_buffer);
  
  data.blockHeight = current_block;
  data.currentTime = getTime();
  data.currentDate = getDate();
  unlockMonitorData();

  return data;
}

clock_data_t getClockData_t(unsigned long mElapsed)
{
  clock_data_t data;

  data.valids = valids;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  getTime(&data.currentHours, &data.currentMinutes, &data.currentSeconds);

  return data;
}

coin_data getCoinData(unsigned long mElapsed)
{
  maybeStartMonitorFetch();
  
  coin_data data;
  
  lockMonitorData();
  data.completedShares = shares;
  data.totalKHashes = totalKHashes;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  
  char price_buffer[16];
  snprintf(price_buffer, sizeof(price_buffer), "$%u", bitcoin_price);
  data.btcPrice = String(price_buffer);
  
  data.currentTime = getTime();
#ifdef SCREEN_FEES_ENABLE
  data.hourFee = String(gData.hourFee);
  data.fastestFee = String(gData.fastestFee);
  data.economyFee = String(gData.economyFee);
  data.minimumFee = String(gData.minimumFee);
#endif
  data.halfHourFee = String(gData.halfHourFee) + " sat/vB";
  data.netwrokDifficulty = gData.difficulty;
  data.globalHashRate = gData.globalHash;
  data.blockHeight = current_block;
  unlockMonitorData();

  unsigned long currentBlock = data.blockHeight.toInt();
  unsigned long remainingBlocks = (((currentBlock / HALVING_BLOCKS) + 1) * HALVING_BLOCKS) - currentBlock;
  data.progressPercent = (HALVING_BLOCKS - remainingBlocks) * 100 / HALVING_BLOCKS;
  data.remainingBlocks = String(remainingBlocks) + " BLOCKS";

  return data;
}

String getPoolAPIUrl(void) {
    poolAPIUrl = String(getPublicPool);
    if (Settings.PoolAddress == "public-pool.io") {
        poolAPIUrl = "https://public-pool.io:40557/api/client/";
    } 
    else {
        if (Settings.PoolAddress == "pool.nerdminers.org") {
            poolAPIUrl = "https://pool.nerdminers.org/users/";
        }
        else {
            switch (Settings.PoolPort) {
                case 3333:
                    if (Settings.PoolAddress == "pool.sethforprivacy.com")
                        poolAPIUrl = "https://pool.sethforprivacy.com/api/client/";
                    if (Settings.PoolAddress == "pool.solomining.de")
                        poolAPIUrl = "https://pool.solomining.de/api/client/";
                    break;
                case 2018:
                    // Local instance of public-pool.io on Umbrel or Start9
                    poolAPIUrl = "http://" + Settings.PoolAddress + ":2019/api/client/";
                    break;
                default:
                    poolAPIUrl = String(getPublicPool);
                    break;
            }
        }
    }
    return poolAPIUrl;
}

pool_data getPoolData(void){
  maybeStartMonitorFetch();
  pool_data data;
  lockMonitorData();
  data = pData;
  unlockMonitorData();
  return data;
}
