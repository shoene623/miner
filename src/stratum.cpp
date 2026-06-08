#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "stratum.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "utils.h"
#include "version.h"



StaticJsonDocument<BUFFER_JSON_DOC> doc;
unsigned long id = 1;

#ifndef STRATUM_VERBOSE_LOG
#define STRATUM_VERBOSE_LOG 0
#endif

#if STRATUM_VERBOSE_LOG
#define SLOG_PRINT(x) Serial.print(x)
#define SLOG_PRINTLN(x) Serial.println(x)
#define SLOG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define SLOG_PRINT(x) do {} while (0)
#define SLOG_PRINTLN(x) do {} while (0)
#define SLOG_PRINTF(...) do {} while (0)
#endif

//Get next JSON RPC Id
unsigned long getNextId(unsigned long id) {
    if (id == ULONG_MAX) {
      id = 1;
      return id;
    }
    return ++id;
}

//Verify Payload doesn't has zero lenght
bool verifyPayload (String* line){
  if(line->length() == 0) return false;
  line->trim();
  if(line->isEmpty()) return false;
  return true;
  
}

bool checkError(const StaticJsonDocument<BUFFER_JSON_DOC> doc) {
  
  if (!doc.containsKey("error")) return false;
  
  if (doc["error"].size() == 0) return false;

  Serial.printf("ERROR: %d | reason: %s \n", (const int) doc["error"][0], (const char*) doc["error"][1]);

  return true;  
}


// STEP 1: Pool server connection (SUBSCRIBE)
    // Docs: 
    // - https://cs.braiins.com/stratum-v1/docs
    // - https://github.com/aeternity/protocol/blob/master/STRATUM.md#mining-subscribe
bool tx_mining_subscribe(WiFiClient& client, mining_subscribe& mSubscribe)
{
    char payload[BUFFER] = {0};
    
    // Subscribe
    id = 1; //Initialize id messages
    #ifndef HAN
    sprintf(payload, "{\"id\": %u, \"method\": \"mining.subscribe\", \"params\": [\"NerdMinerV2/%s\"]}\n", id, CURRENT_VERSION);
    #else
    sprintf(payload, "{\"id\": %u, \"method\": \"mining.subscribe\", \"params\": [\"HAN_SOLOminer/%s\"]}\n", id, CURRENT_VERSION);
    #endif
    
    SLOG_PRINTF("[WORKER] ==> Mining subscribe\n");
    SLOG_PRINT("  Sending  : "); SLOG_PRINTLN(payload);
    client.print(payload);
    
    vTaskDelay(200 / portTICK_PERIOD_MS); //Small delay
    
    String line = client.readStringUntil('\n');
    if(!parse_mining_subscribe(line, mSubscribe)) return false;

  
    SLOG_PRINT("    sub_details: "); SLOG_PRINTLN(mSubscribe.sub_details);
    SLOG_PRINT("    extranonce1: "); SLOG_PRINTLN(mSubscribe.extranonce1);
    SLOG_PRINT("    extranonce2_size: "); SLOG_PRINTLN(mSubscribe.extranonce2_size);

    if((mSubscribe.extranonce1.length() == 0) ) { 
        Serial.printf("[WORKER] >>>>>>>>> Work aborted\n"); 
        Serial.printf("extranonce1 length: %u \n", mSubscribe.extranonce1.length());
        doc.clear();
        doc.garbageCollect();
        return false; 
    }
    return true;
}

bool parse_mining_subscribe(String line, mining_subscribe& mSubscribe)
{
    if(!verifyPayload(&line)) return false;
    SLOG_PRINT("  Receiving: "); SLOG_PRINTLN(line);
   
    DeserializationError error = deserializeJson(doc, line);

    if (error || checkError(doc)) return false;
    if (!doc.containsKey("result")) return false;

    mSubscribe.sub_details = String((const char*) doc["result"][0][0][1]);
    mSubscribe.extranonce1 = String((const char*) doc["result"][1]);
    mSubscribe.extranonce2_size = doc["result"][2];

    return true;
}

mining_subscribe init_mining_subscribe(void)
{
    mining_subscribe new_mSub;

    new_mSub.extranonce1 = "";
    new_mSub.extranonce2 = "";
    new_mSub.extranonce2_size = 0;
    new_mSub.sub_details = "";


    return new_mSub;
}

// STEP 2: Pool server auth (authorize)
bool tx_mining_auth(WiFiClient& client, const char * user, const char * pass)
{
    char payload[BUFFER] = {0};

    // Authorize
    id = getNextId(id);
    sprintf(payload, "{\"params\": [\"%s\", \"%s\"], \"id\": %u, \"method\": \"mining.authorize\"}\n", 
      user, pass, id);
    
    SLOG_PRINTF("[WORKER] ==> Autorize work\n");
    SLOG_PRINT("  Sending  : "); SLOG_PRINTLN(payload);
    client.print(payload);

    vTaskDelay(200 / portTICK_PERIOD_MS); //Small delay

    //Don't parse here any answer
    //Miner started to receive mining notifications so better parse all at main thread

    return true;
}


stratum_method parse_mining_method(String line)
{
    if(!verifyPayload(&line)) return STRATUM_PARSE_ERROR;
    SLOG_PRINT("  Receiving: "); SLOG_PRINTLN(line);
    
    DeserializationError error = deserializeJson(doc, line);

    if (error || checkError(doc)) return STRATUM_PARSE_ERROR;

    if (!doc.containsKey("method")) {
      // "error":null means success
      if (doc["error"].isNull())
        return STRATUM_SUCCESS;
      else
        return STRATUM_UNKNOWN;
    }
    stratum_method result = STRATUM_UNKNOWN;

    if (strcmp("mining.notify", (const char*) doc["method"]) == 0) {
        result = MINING_NOTIFY;
    } else if (strcmp("mining.set_difficulty", (const char*) doc["method"]) == 0) {
        result = MINING_SET_DIFFICULTY;
    }

    return result;
}

bool parse_mining_notify(String line, mining_job& mJob)
{
    SLOG_PRINTLN("    Parsing Method [MINING NOTIFY]");
    if(!verifyPayload(&line)) return false;
   
    DeserializationError error = deserializeJson(doc, line);

    if (error) return false;
    if (!doc.containsKey("params")) return false;

    mJob.job_id = String((const char*) doc["params"][0]);
    mJob.prev_block_hash = String((const char*) doc["params"][1]);
    mJob.coinb1 = String((const char*) doc["params"][2]);
    mJob.coinb2 = String((const char*) doc["params"][3]);
    mJob.merkle_branch = doc["params"][4];
    mJob.version = String((const char*) doc["params"][5]);
    mJob.nbits = String((const char*) doc["params"][6]);
    mJob.ntime = String((const char*) doc["params"][7]);
    mJob.clean_jobs = doc["params"][8]; //bool

    #ifdef DEBUG_MINING
    Serial.print("    job_id: "); Serial.println(mJob.job_id);
    Serial.print("    prevhash: "); Serial.println(mJob.prev_block_hash);
    Serial.print("    coinb1: "); Serial.println(mJob.coinb1);
    Serial.print("    coinb2: "); Serial.println(mJob.coinb2);
    Serial.print("    merkle_branch size: "); Serial.println(mJob.merkle_branch.size());
    Serial.print("    version: "); Serial.println(mJob.version);
    Serial.print("    nbits: "); Serial.println(mJob.nbits);
    Serial.print("    ntime: "); Serial.println(mJob.ntime);
    Serial.print("    clean_jobs: "); Serial.println(mJob.clean_jobs);
    #endif
    //Check if parameters where correctly received
    if (checkError(doc)) {
      Serial.printf("[WORKER] >>>>>>>>> Work aborted\n"); 
      return false;
    }
    return true;
}


bool tx_mining_submit(WiFiClient& client, mining_subscribe mWorker, mining_job mJob, unsigned long nonce, unsigned long &submit_id)
{
    char payload[BUFFER] = {0};
    char nonce_hex[9] = {0};
    snprintf(nonce_hex, sizeof(nonce_hex), "%08lx", nonce & 0xFFFFFFFFul);

    // Submit
    id = getNextId(id);
    submit_id = id;
    sprintf(payload, "{\"id\":%u,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}\n",
        id,
        mWorker.wName,//"bc1qvv469gmw4zz6qa4u4dsezvrlmqcqszwyfzhgwj", //mWorker.name,
        mJob.job_id.c_str(),
        mWorker.extranonce2.c_str(),
        mJob.ntime.c_str(),
        nonce_hex
        );
    SLOG_PRINT("  Sending  : "); SLOG_PRINT(payload);
    client.print(payload);
    //Serial.print("  Receiving: "); Serial.println(client.readStringUntil('\n'));

    return true;
}

bool parse_mining_set_difficulty(String line, double& difficulty)
{
    SLOG_PRINTLN("    Parsing Method [SET DIFFICULTY]");
    if(!verifyPayload(&line)) return false;
   
    DeserializationError error = deserializeJson(doc, line);

    if (error) return false;
    if (!doc.containsKey("params")) return false;

    SLOG_PRINT("    difficulty: "); SLOG_PRINTLN(String((double)doc["params"][0], 12));
    difficulty = (double)doc["params"][0];

    return true;
}

bool tx_suggest_difficulty(WiFiClient& client, double difficulty)
{
    char payload[BUFFER] = {0};

    id = getNextId(id);
    sprintf(payload, "{\"id\":%d,\"method\":\"mining.suggest_difficulty\",\"params\":[%.10g]}\n", id, difficulty);
    
    SLOG_PRINT("  Sending  : "); SLOG_PRINT(payload);
    return client.print(payload);

}


unsigned long parse_extract_id(const String &line)
{
    DeserializationError error = deserializeJson(doc, line);
    if (error)
        return 0;
    
    if (!doc.containsKey("id"))
        return 0;

    unsigned long id = doc["id"];

    return id;
}