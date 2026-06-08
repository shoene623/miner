#include <Arduino.h>
#include <WiFi.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#pragma GCC optimize("O3,unroll-loops")

// =============================================================
// Dual-core ESP32 (Xtensa LX6) mining topology demo
// =============================================================
// - Worker0 pinned to core 0, Worker1 pinned to core 1
// - Both workers run at configMAX_PRIORITIES - 1
// - Midstate for first 64-byte chunk is precomputed
// - Nonce loop only mutates 4-byte nonce in chunk #2
// - Worker loop does not call into Wi-Fi/UI code
// - vTaskDelay(1) only when share is found/submitted or new work arrives
// =============================================================

namespace {

constexpr uint32_t NONCE_BATCH = 2048;
constexpr size_t SHARE_QUEUE_LEN = 16;
#ifndef DEMO_FAKE_POOL
#define DEMO_FAKE_POOL 0
#endif

alignas(4) static const uint32_t K256[64] = {
  0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
  0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
  0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
  0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
  0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
  0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
  0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
  0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u
};

constexpr uint32_t SHA256_IV[8] = {
  0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
  0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
};

struct MidstateJob {
  uint32_t midstate[8];  // SHA-256 state after first 64 bytes.
  uint32_t tail_w0;      // Last 4 bytes of merkle root (header bytes 64..67).
  uint32_t tail_time;    // nTime (header bytes 68..71).
  uint32_t tail_bits;    // nBits (header bytes 72..75).
  uint32_t target_be[8]; // Big-endian target words for final hash compare.
  volatile uint32_t generation;
};

struct ShareResult {
  uint32_t worker;
  uint32_t nonce;
  uint32_t hash_be[8];
  uint32_t generation;
};

MidstateJob g_work{};
QueueHandle_t g_shareQueue = nullptr;
volatile uint32_t g_nonceCursor = 0;
volatile uint32_t g_totalHashes = 0;

static inline uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static inline void store_be32(uint8_t* out, uint32_t v) {
  out[0] = uint8_t(v >> 24);
  out[1] = uint8_t(v >> 16);
  out[2] = uint8_t(v >> 8);
  out[3] = uint8_t(v);
}

IRAM_ATTR static inline uint32_t rotr32(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32u - n));
}

IRAM_ATTR static inline uint32_t s0(uint32_t x) { return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3); }
IRAM_ATTR static inline uint32_t s1(uint32_t x) { return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10); }
IRAM_ATTR static inline uint32_t S0(uint32_t x) { return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22); }
IRAM_ATTR static inline uint32_t S1(uint32_t x) { return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25); }
IRAM_ATTR static inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) { return z ^ (x & (y ^ z)); }
IRAM_ATTR static inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (z & (x | y)); }

IRAM_ATTR static inline void sha256_compress_words(const uint32_t init[8], uint32_t W[64], uint32_t out[8]) {
  uint32_t a = init[0], b = init[1], c = init[2], d = init[3];
  uint32_t e = init[4], f = init[5], g = init[6], h = init[7];

  #pragma GCC unroll 64
  for (int i = 0; i < 64; ++i) {
    if (i >= 16) {
      W[i] = s1(W[i - 2]) + W[i - 7] + s0(W[i - 15]) + W[i - 16];
    }
    const uint32_t t1 = h + S1(e) + Ch(e, f, g) + K256[i] + W[i];
    const uint32_t t2 = S0(a) + Maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  out[0] = init[0] + a;
  out[1] = init[1] + b;
  out[2] = init[2] + c;
  out[3] = init[3] + d;
  out[4] = init[4] + e;
  out[5] = init[5] + f;
  out[6] = init[6] + g;
  out[7] = init[7] + h;
}

IRAM_ATTR static inline bool hash_meets_target(const uint32_t hash_be[8], const uint32_t target_be[8]) {
  #pragma GCC unroll 8
  for (int i = 0; i < 8; ++i) {
    if (hash_be[i] < target_be[i]) return true;
    if (hash_be[i] > target_be[i]) return false;
  }
  return true;
}

IRAM_ATTR static inline bool double_sha256_midstate_nonce(
    const MidstateJob& work,
    uint32_t nonce,
    uint32_t hash_be[8]) {
  uint32_t W1[64] = {0};
  W1[0] = work.tail_w0;
  W1[1] = work.tail_time;
  W1[2] = work.tail_bits;
  W1[3] = __builtin_bswap32(nonce);
  W1[4] = 0x80000000u;
  W1[15] = 640u;

  uint32_t first_hash[8];
  sha256_compress_words(work.midstate, W1, first_hash);

  uint32_t W2[64] = {0};
  W2[0] = first_hash[0];
  W2[1] = first_hash[1];
  W2[2] = first_hash[2];
  W2[3] = first_hash[3];
  W2[4] = first_hash[4];
  W2[5] = first_hash[5];
  W2[6] = first_hash[6];
  W2[7] = first_hash[7];
  W2[8] = 0x80000000u;
  W2[15] = 256u;

  uint32_t second_hash[8];
  sha256_compress_words(SHA256_IV, W2, second_hash);

  hash_be[0] = __builtin_bswap32(second_hash[0]);
  hash_be[1] = __builtin_bswap32(second_hash[1]);
  hash_be[2] = __builtin_bswap32(second_hash[2]);
  hash_be[3] = __builtin_bswap32(second_hash[3]);
  hash_be[4] = __builtin_bswap32(second_hash[4]);
  hash_be[5] = __builtin_bswap32(second_hash[5]);
  hash_be[6] = __builtin_bswap32(second_hash[6]);
  hash_be[7] = __builtin_bswap32(second_hash[7]);

  return hash_meets_target(hash_be, work.target_be);
}

IRAM_ATTR static inline void compute_midstate_first64(const uint8_t header[80], uint32_t midstate[8]) {
  uint32_t W[64] = {0};
  for (int i = 0; i < 16; ++i) {
    W[i] = be32(header + (i * 4));
  }
  sha256_compress_words(SHA256_IV, W, midstate);
}

// Converts compact nBits (big-endian word) into 256-bit target words (big-endian).
static void target_from_nbits_be(uint32_t nbits_be, uint32_t target_be[8]) {
  uint8_t target[32] = {0};
  const uint8_t exp = uint8_t(nbits_be >> 24);
  const uint32_t mant = nbits_be & 0x007FFFFFu;

  if (exp >= 3 && exp <= 32) {
    const int idx = int(exp) - 3;
    if (idx >= 0 && idx < 32) target[idx] = uint8_t((mant >> 16) & 0xFFu);
    if (idx + 1 >= 0 && idx + 1 < 32) target[idx + 1] = uint8_t((mant >> 8) & 0xFFu);
    if (idx + 2 >= 0 && idx + 2 < 32) target[idx + 2] = uint8_t(mant & 0xFFu);
  }

  for (int i = 0; i < 8; ++i) {
    target_be[i] = be32(&target[i * 4]);
  }
}

static void load_new_work(const uint8_t header[80]) {
  MidstateJob next{};
  compute_midstate_first64(header, next.midstate);
  next.tail_w0 = be32(header + 64);
  next.tail_time = be32(header + 68);
  next.tail_bits = be32(header + 72);
  target_from_nbits_be(next.tail_bits, next.target_be);

  // Easier demo target if the pool target is extremely hard for test traffic.
  if (next.target_be[0] == 0 && next.target_be[1] == 0) {
    next.target_be[0] = 0x0000FFFFu;
    next.target_be[1] = 0xFFFFFFFFu;
    next.target_be[2] = 0xFFFFFFFFu;
    next.target_be[3] = 0xFFFFFFFFu;
    next.target_be[4] = 0xFFFFFFFFu;
    next.target_be[5] = 0xFFFFFFFFu;
    next.target_be[6] = 0xFFFFFFFFu;
    next.target_be[7] = 0xFFFFFFFFu;
  }

  const uint32_t new_generation = g_work.generation + 1;
  next.generation = new_generation;

  g_work = next;
  __atomic_store_n(&g_nonceCursor, 0u, __ATOMIC_RELAXED);
}

// External integration hook: call this from your Stratum notify handler with an 80-byte header.
void submitNewStratumHeader(const uint8_t* header80) {
  if (header80 == nullptr) {
    return;
  }
  load_new_work(header80);
}

void miningWorkerTask(void* param) {
  const uint32_t workerId = reinterpret_cast<uint32_t>(param);
  uint32_t localGeneration = 0;

  Serial.printf("[MINER-%u] started on core %d\n", workerId, xPortGetCoreID());

  for (;;) {
    const uint32_t gen = g_work.generation;
    if (gen == 0) {
      continue;
    }

    if (gen != localGeneration) {
      localGeneration = gen;
      vTaskDelay(1); // Allowed watchdog feed/yield path: only on new work.
    }

    const uint32_t base = __atomic_fetch_add(&g_nonceCursor, NONCE_BATCH, __ATOMIC_RELAXED);
    const uint32_t end = base + NONCE_BATCH;
    __atomic_add_fetch(&g_totalHashes, NONCE_BATCH, __ATOMIC_RELAXED);

    for (uint32_t nonce = base; nonce < end; ++nonce) {
      if (g_work.generation != localGeneration) {
        break;
      }

      uint32_t hash_be[8];
      if (double_sha256_midstate_nonce(g_work, nonce, hash_be)) {
        ShareResult share{};
        share.worker = workerId;
        share.nonce = nonce;
        share.generation = localGeneration;
        memcpy(share.hash_be, hash_be, sizeof(hash_be));

        (void)xQueueSend(g_shareQueue, &share, 0);
        vTaskDelay(1); // Allowed watchdog feed/yield path: only on share submit path.
      }
    }
  }
}

#if DEMO_FAKE_POOL
// Demo-only fake pool input. Production should call submitNewStratumHeader().
static void fake_pool_work_generator() {
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (now - lastMs < 10000) {
    return;
  }
  lastMs = now;

  uint8_t header[80] = {0};
  // version
  header[0] = 0x20;
  // timestamp (big-endian bytes in this local representation)
  const uint32_t t = now / 1000u;
  store_be32(header + 68, t);
  // compact target (easy-ish demo): 0x1f03ffff
  store_be32(header + 72, 0x1f03ffffu);

  load_new_work(header);
}
#endif

void managerTask(void* /*param*/) {
  Serial.printf("[MANAGER] started on core %d\n", xPortGetCoreID());

  uint32_t lastHashSample = 0;
  uint32_t lastStatMs = millis();

  for (;;) {
    // Decoupled comm/UI side: run at low priority, never in mining worker loops.
    #if DEMO_FAKE_POOL
    fake_pool_work_generator();
    #endif

    ShareResult share{};
    while (xQueueReceive(g_shareQueue, &share, 0) == pdTRUE) {
      Serial.printf("[SHARE] worker=%u nonce=0x%08x gen=%u hash0=%08x\n",
                    share.worker, share.nonce, share.generation, share.hash_be[0]);
      // TODO: submit share via Stratum socket from this manager thread.
    }

    const uint32_t now = millis();
    if (now - lastStatMs >= 1000) {
      const uint32_t total = __atomic_load_n(&g_totalHashes, __ATOMIC_RELAXED);
      const uint32_t delta = total - lastHashSample;
      lastHashSample = total;
      lastStatMs = now;
      Serial.printf("[STATS] %.2f kH/s\n", float(delta) / 1000.0f);
    }

    // TODO: perform display/SPI/I2C updates here (not in mining workers).
    // TODO: perform Wi-Fi/socket polling here (not in mining workers).

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.mode(WIFI_STA);

  g_shareQueue = xQueueCreate(SHARE_QUEUE_LEN, sizeof(ShareResult));

  // Low-priority manager for network + UI + share submit handling.
  xTaskCreatePinnedToCore(
    managerTask,
    "Manager",
    6144,
    nullptr,
    1,
    nullptr,
    1
  );

  // Required architecture: dual workers pinned to explicit cores, max-1 priority.
  xTaskCreatePinnedToCore(
    miningWorkerTask,
    "Worker0",
    6144,
    reinterpret_cast<void*>(0),
    configMAX_PRIORITIES - 1,
    nullptr,
    0
  );

  xTaskCreatePinnedToCore(
    miningWorkerTask,
    "Worker1",
    6144,
    reinterpret_cast<void*>(1),
    configMAX_PRIORITIES - 1,
    nullptr,
    1
  );
}

void loop() {
  // Everything runs in RTOS tasks.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
