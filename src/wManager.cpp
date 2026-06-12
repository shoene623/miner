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

// Structures copied from display driver to access caches
struct CandlePoint {
  float open = 0.0f;
  float high = 0.0f;
  float low = 0.0f;
  float close = 0.0f;
  float volume = 0.0f;
};

constexpr int CANDLE_MAX_POINTS = 24;
struct CandleSeries {
  String price;
  String direction;
  String move;
  int count = 0;
  CandlePoint points[CANDLE_MAX_POINTS];
};

struct TickerData {
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
  String greeting;
  String updated;
  String status;
};

struct SportsData {
  String shortName = "";
  String status = "";
  String stlScore = "";
  String oppScore = "";
  String oppAbbrev = "";
  bool isStlHome = false;
  bool hasGame = false;
  unsigned long lastFetchMs = 0;
};

// Define WiFiManager Object
WiFiManager wm;
extern monitor_data mMonitor;
extern volatile double currentPoolDifficultyLive;
extern pool_data pData;

// Fallback weak definitions so environments without display-based tickers can compile cleanly.
TickerData tickerCache __attribute__((weak));
SportsData mlbCache __attribute__((weak));
SportsData nhlCache __attribute__((weak));
SemaphoreHandle_t tickerDataMutex __attribute__((weak)) = nullptr;
SemaphoreHandle_t sportsDataMutex __attribute__((weak)) = nullptr;

extern unsigned int bitcoin_price;
extern String current_block;
extern global_data gData;

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
    html.reserve(7500); // Expanded capacity for beautiful styling & dashboard widgets
    html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>Patrick's Bitcoin Miner Hub</title>";
    html += "<link href='https://fonts.googleapis.com/css2?family=Outfit:wght@400;600;800&family=Share+Tech+Mono&display=swap' rel='stylesheet'>";
    html += "<style>"
            "body{background:#04070c;background-image:radial-gradient(circle at 50% 30%,#091322 0%,#04070c 80%);color:#f1f5f9;font-family:'Outfit',sans-serif;margin:0;padding:0;min-height:100vh}"
            "main{max-width:1200px;margin:0 auto;padding:24px}"
            "header{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid rgba(0,240,255,0.15);padding-bottom:16px;margin-bottom:24px;flex-wrap:wrap;gap:12px}"
            "h1{font-size:26px;font-weight:800;margin:0;background:linear-gradient(135deg,#00f0ff 0%,#00ff9f 100%);-webkit-background-clip:text;-webkit-text-fill-color:transparent;text-shadow:0 0 20px rgba(0,240,255,0.25)}"
            ".status-badge{display:flex;align-items:center;gap:8px;background:rgba(0,255,159,0.05);border:1px solid rgba(0,255,159,0.25);padding:6px 14px;border-radius:20px;font-size:13px;font-weight:600;color:#00ff9f}"
            ".pulse{width:8px;height:8px;background-color:#00ff9f;border-radius:50%;box-shadow:0 0 10px #00ff9f;animation:pulse-anim 2s infinite}"
            "@keyframes pulse-anim{0%{transform:scale(0.95);box-shadow:0 0 0 0 rgba(0,255,159,0.7)}70%{transform:scale(1);box-shadow:0 0 0 8px rgba(0,255,159,0)}100%{transform:scale(0.95);box-shadow:0 0 0 0 rgba(0,255,159,0)}}"
            ".btn-group{display:flex;gap:12px;margin-bottom:24px}"
            ".btn{display:inline-block;padding:10px 20px;border-radius:8px;border:1px solid rgba(0,240,255,0.2);background:rgba(0,240,255,0.03);color:#e2e8f0;text-decoration:none;font-weight:700;font-size:14px;transition:all 0.2s ease;cursor:pointer;outline:none}"
            ".btn:hover{background:#00f0ff;color:#04070c;box-shadow:0 0 15px rgba(0,240,255,0.4);border-color:#00f0ff;transform:translateY(-1px)}"
            ".btn.p{background:rgba(0,255,159,0.08);border-color:rgba(0,255,159,0.25);color:#00ff9f}"
            ".btn.p:hover{background:#00ff9f;color:#04070c;box-shadow:0 0 15px rgba(0,255,159,0.4);border-color:#00ff9f}"
            ".section-title{font-size:13px;font-weight:800;text-transform:uppercase;letter-spacing:2px;color:#00f0ff;margin:28px 0 16px;display:flex;align-items:center;gap:10px}"
            ".section-title::after{content:'';flex-grow:1;height:1px;background:linear-gradient(90deg,rgba(0,240,255,0.15) 0%,transparent 100%)}"
            ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:16px}"
            ".card{background:rgba(13,20,30,0.45);backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);border:1px solid rgba(0,240,255,0.1);border-radius:14px;padding:16px;transition:all 0.3s ease;position:relative}"
            ".card:hover{border-color:rgba(0,240,255,0.3);box-shadow:0 0 20px rgba(0,240,255,0.12);transform:translateY(-2px)}"
            ".card-k{font-size:11px;font-weight:700;color:#94a3b8;text-transform:uppercase;letter-spacing:1px}"
            ".card-v{margin-top:6px;font-size:22px;font-weight:800;font-family:'Share Tech Mono',monospace;color:#f8fafc}"
            ".card-s{margin-top:6px;font-size:12px;color:#64748b;display:flex;justify-content:space-between}"
            ".green{color:#00ff9f}.blue{color:#00f0ff}.pink{color:#ff007f}.orange{color:#ffd280}.red{color:#f87171}"
            ".scoreboard{display:flex;flex-direction:column;gap:10px}"
            ".matchup{display:flex;justify-content:space-between;align-items:center;padding:6px 0}"
            ".team{display:flex;align-items:center;gap:8px;font-weight:700;font-size:16px}"
            ".team.active{color:#00ff9f}"
            ".score{font-family:'Share Tech Mono',monospace;font-size:20px;font-weight:800}"
            ".match-status{font-size:12px;color:#94a3b8;text-align:right}"
            ".footer{margin-top:40px;padding-top:16px;border-top:1px solid rgba(0,240,255,0.1);color:#64748b;font-size:12px;display:flex;justify-content:space-between;flex-wrap:wrap;gap:8px}"
            "</style></head><body>";
    html += "<main>";
    html += "<header><h1 id='welcomeGreeting'>Welcome, Patrick!</h1>";
    html += "<div class='status-badge'><div class='pulse'></div><span id='ip'>CONNECTING...</span></div></header>";
    html += "<div class='btn-group'>";
    html += "<a class='btn' href='/settings'>System Configuration</a>";
    html += "<a class='btn' href='/api/status' target='_blank'>Developer JSON API</a>";
    html += "<button class='btn p' id='refresh'>Sync Core Metrics</button>";
    html += "</div>";

    // Section 1: Hashing Status
    html += "<div class='section-title'>System Mining Engine</div>";
    html += "<div class='grid'>";
    html += "<div class='card'><div class='card-k'>Hashrate</div><div class='card-v blue' id='hashrate'>--</div><div class='card-s'><span>Performance Speed</span></div></div>";
    html += "<div class='card'><div class='card-k'>Shares / Valids</div><div class='card-v green' id='shares'>--</div><div class='card-s'><span>NVS Pool Submissions</span></div></div>";
    html += "<div class='card'><div class='card-k'>Local Best Diff</div><div class='card-v orange' id='bestLocal'>--</div><div class='card-s'><span>Nerdminer Best Job</span></div></div>";
    html += "<div class='card'><div class='card-k'>Engine Uptime</div><div class='card-v' id='uptime'>--</div><div class='card-s'><span>Days  HH:MM:SS</span></div></div>";
    html += "</div>";

    // Section 2: Blockchain Stats
    html += "<div class='section-title'>Bitcoin Network Core</div>";
    html += "<div class='grid'>";
    html += "<div class='card'><div class='card-k'>Bitcoin Price</div><div class='card-v green' id='btcPrice'>--</div><div class='card-s'><span>CoinGecko Live API</span></div></div>";
    html += "<div class='card'><div class='card-k'>Tip Block Height</div><div class='card-v blue' id='blockHeight'>--</div><div class='card-s'><span>Mempool Tip height</span></div></div>";
    html += "<div class='card'><div class='card-k'>Half-Hour Gas Fee</div><div class='card-v pink' id='feeRate'>--</div><div class='card-s'><span>Recommended Sat/vB</span></div></div>";
    html += "<div class='card'><div class='card-k'>Halving Status</div><div class='card-v orange' id='halvingText'>--</div><div class='card-s'><span id='halvingPct'>--</span></div></div>";
    html += "</div>";

    // Section 3: Sentiment & Sports (Double Column Flex Grid)
    html += "<div class='section-title'>Geopolitical Intelligence & Sports hub</div>";
    html += "<div class='grid'>";
    // Geopolitical/Market Sentiment Card
    html += "<div class='card' style='grid-column: span 1'>"
            "<div class='card-k' style='margin-bottom:12px'>Crypto Market Sentiment</div>"
            "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:12px'>"
              "<span style='font-size:15px;font-weight:600' id='sentimentCat'>--</span>"
              "<span class='card-v blue' style='font-size:26px;margin:0' id='sentimentScore'>--</span>"
            "</div>"
            "<div style='font-size:13px;line-height:1.4;color:#cbd5e1;background:rgba(0,240,255,0.04);border:1px solid rgba(0,240,255,0.1);border-radius:8px;padding:8px;margin-bottom:12px' id='sentimentRec'>--</div>"
            "<div style='display:grid;grid-template-columns:1fr 1fr;gap:10px;font-size:12px'>"
              "<div style='background:rgba(0,255,159,0.04);border:1px solid rgba(0,255,159,0.15);border-radius:6px;padding:6px'>"
                "<div style='font-weight:700;color:#00ff9f'>BTC Trade Setup</div>"
                "<div style='margin-top:4px'>Entry: <span id='btcEntry'>--</span></div>"
                "<div>Target: <span id='btcTarget'>--</span></div>"
              "</div>"
              "<div style='background:rgba(255,0,127,0.04);border:1px solid rgba(255,0,127,0.15);border-radius:6px;padding:6px'>"
                "<div style='font-weight:700;color:#ff007f'>XRP Trade Setup</div>"
                "<div style='margin-top:4px'>Entry: <span id='xrpEntry'>--</span></div>"
                "<div>Target: <span id='xrpTarget'>--</span></div>"
              "</div>"
            "</div>"
          "</div>";
    // St. Louis Sports Card
    html += "<div class='card' style='grid-column: span 1'>"
            "<div class='card-k' style='margin-bottom:12px'>St. Louis Live Scoreboard</div>"
            "<div class='scoreboard'>"
              // MLB
              "<div style='border-bottom:1px solid rgba(255,255,255,0.06);padding-bottom:10px'>"
                "<div style='display:flex;justify-content:space-between;align-items:center;font-size:11px;font-weight:700;color:#00f0ff;margin-bottom:6px'><span>MLB - CARDINALS</span><span class='match-status' id='mlbStatus'>--</span></div>"
                "<div class='matchup'>"
                  "<div class='team' id='mlbAwayName'>STL</div>"
                  "<div class='score' id='mlbAwayScore'>--</div>"
                "</div>"
                "<div class='matchup'>"
                  "<div class='team' id='mlbHomeName'>OPP</div>"
                  "<div class='score' id='mlbHomeScore'>--</div>"
                "</div>"
              "</div>"
              // NHL
              "<div>"
                "<div style='display:flex;justify-content:space-between;align-items:center;font-size:11px;font-weight:700;color:#ff007f;margin-bottom:6px'><span>NHL - BLUES</span><span class='match-status' id='nhlStatus'>--</span></div>"
                "<div class='matchup'>"
                  "<div class='team' id='nhlAwayName'>STL</div>"
                  "<div class='score' id='nhlAwayScore'>--</div>"
                "</div>"
                "<div class='matchup'>"
                  "<div class='team' id='nhlHomeName'>OPP</div>"
                  "<div class='score' id='nhlHomeScore'>--</div>"
                "</div>"
              "</div>"
            "</div>"
          "</div>";
    html += "</div>";

    // Section 4: System Resource Diagnostics
    html += "<div class='section-title'>System Resource Diagnostics</div>";
    html += "<div class='grid'>";
    html += "<div class='card'><div class='card-k'>Free RAM / Heap</div><div class='card-v blue' id='freeHeap'>--</div><div class='card-s'><span>Available heap memory</span></div></div>";
    html += "<div class='card'><div class='card-k'>Min Free RAM (Watermark)</div><div class='card-v pink' id='minFreeHeap'>--</div><div class='card-s'><span>Lowest RAM reached</span></div></div>";
    html += "<div class='card'><div class='card-k'>Max Contiguous Block</div><div class='card-v green' id='maxAllocHeap'>--</div><div class='card-s'><span>Largest alloc block</span></div></div>";
    html += "<div class='card'><div class='card-k'>Total Heap Size</div><div class='card-v' id='heapSize'>--</div><div class='card-s'><span>Device boot memory</span></div></div>";
    html += "</div>";

    // Footer & Credits
    html += "<div class='footer'>"
            "<span>NerdMiner v2 - Active Hub</span>"
            "<span id='stamp'>Syncing telemetry...</span>"
            "</div>";
    html += "</main>";

    // JS Telemetry Sync Engine
    html += "<script>"
            "let refreshing=false;"
            "async function refresh(){"
              "if(refreshing)return;"
              "refreshing=true;"
              "document.getElementById('refresh').textContent='Syncing...';"
              "try{"
                "const r=await fetch('/api/status',{cache:'no-store'});"
                "const d=await r.json();"
                "const m=d.mining||{}, p=d.pool||{}, bc=d.blockchain||{}, s=d.sentiment||{}, sp=d.sports||{}, sys=d.system||{};"
                // System IP
                "document.getElementById('ip').textContent=d.ip||'--';"
                "document.getElementById('welcomeGreeting').textContent=s.greeting||'Welcome, Patrick!';"
                // Hashing
                "document.getElementById('hashrate').textContent=(m.hashrate||'0.00')+' KH/s';"
                "document.getElementById('shares').textContent=(m.shares||'0')+' / '+(m.valids||'0');"
                "document.getElementById('bestLocal').textContent=m.bestDiff||'--';"
                "document.getElementById('uptime').textContent=m.uptime||'--';"
                // System Diagnostics
                "document.getElementById('freeHeap').textContent=sys.freeHeap ? (Number(sys.freeHeap)/1024).toFixed(1)+' KB' : '--';"
                "document.getElementById('minFreeHeap').textContent=sys.minFreeHeap ? (Number(sys.minFreeHeap)/1024).toFixed(1)+' KB' : '--';"
                "document.getElementById('maxAllocHeap').textContent=sys.maxAllocHeap ? (Number(sys.maxAllocHeap)/1024).toFixed(1)+' KB' : '--';"
                "document.getElementById('heapSize').textContent=sys.heapSize ? (Number(sys.heapSize)/1024).toFixed(1)+' KB' : '--';"
                // Blockchain
                "document.getElementById('btcPrice').textContent=bc.btcPrice ? ('$'+Number(bc.btcPrice).toLocaleString()) : '--';"
                "document.getElementById('blockHeight').textContent=bc.blockHeight||'--';"
                "document.getElementById('feeRate').textContent=bc.halfHourFee ? (bc.halfHourFee+' sat/vB') : '--';"
                "if(bc.remainingBlocks){"
                  "document.getElementById('halvingText').textContent=bc.remainingBlocks+' blocks';"
                  "document.getElementById('halvingPct').textContent='Difficulty: '+(bc.difficulty||'--');"
                "} else {"
                  "document.getElementById('halvingText').textContent='--';"
                  "document.getElementById('halvingPct').textContent='--';"
                "}"
                // Sentiment
                "document.getElementById('sentimentScore').textContent=s.score||'--';"
                "document.getElementById('sentimentCat').textContent=s.category + ' (' + s.direction + ')';"
                "document.getElementById('sentimentRec').textContent=s.recommendation||'--';"
                "document.getElementById('btcEntry').textContent=s.btcEntry ? ('$'+Number(s.btcEntry).toLocaleString()) : '--';"
                "document.getElementById('btcTarget').textContent=s.btcTakeProfit ? ('$'+Number(s.btcTakeProfit).toLocaleString()) : '--';"
                "document.getElementById('xrpEntry').textContent=s.xrpEntry ? ('$'+parseFloat(s.xrpEntry).toFixed(3)) : '--';"
                "document.getElementById('xrpTarget').textContent=s.xrpTakeProfit ? ('$'+parseFloat(s.xrpTakeProfit).toFixed(3)) : '--';"
                // Sports - MLB Cardinals
                "let mlbAway = document.getElementById('mlbAwayName');"
                "let mlbHome = document.getElementById('mlbHomeName');"
                "mlbAway.className = 'team';"
                "mlbHome.className = 'team';"
                "if(sp.mlb && sp.mlb.hasGame){"
                  "document.getElementById('mlbStatus').textContent=sp.mlb.status||'--';"
                  "if(sp.mlb.isStlHome){"
                    "mlbAway.textContent=sp.mlb.oppAbbrev||'OPP';"
                    "document.getElementById('mlbAwayScore').textContent=sp.mlb.oppScore!=='' ? sp.mlb.oppScore : '0';"
                    "mlbHome.textContent='STL';"
                    "mlbHome.classList.add('green');"
                    "document.getElementById('mlbHomeScore').textContent=sp.mlb.stlScore!=='' ? sp.mlb.stlScore : '0';"
                  "} else {"
                    "mlbAway.textContent='STL';"
                    "mlbAway.classList.add('green');"
                    "document.getElementById('mlbAwayScore').textContent=sp.mlb.stlScore!=='' ? sp.mlb.stlScore : '0';"
                    "mlbHome.textContent=sp.mlb.oppAbbrev||'OPP';"
                    "document.getElementById('mlbHomeScore').textContent=sp.mlb.oppScore!=='' ? sp.mlb.oppScore : '0';"
                  "}"
                "} else {"
                  "document.getElementById('mlbStatus').textContent='No Game';"
                  "mlbAway.textContent='STL';"
                  "document.getElementById('mlbAwayScore').textContent='--';"
                  "mlbHome.textContent='OPP';"
                  "document.getElementById('mlbHomeScore').textContent='--';"
                "}"
                // Sports - NHL Blues
                "let nhlAway = document.getElementById('nhlAwayName');"
                "let nhlHome = document.getElementById('nhlHomeName');"
                "nhlAway.className = 'team';"
                "nhlHome.className = 'team';"
                "if(sp.nhl && sp.nhl.hasGame){"
                  "document.getElementById('nhlStatus').textContent=sp.nhl.status||'--';"
                  "if(sp.nhl.isStlHome){"
                    "nhlAway.textContent=sp.nhl.oppAbbrev||'OPP';"
                    "document.getElementById('nhlAwayScore').textContent=sp.nhl.oppScore!=='' ? sp.nhl.oppScore : '0';"
                    "nhlHome.textContent='STL';"
                    "nhlHome.classList.add('pink');"
                    "document.getElementById('nhlHomeScore').textContent=sp.nhl.stlScore!=='' ? sp.nhl.stlScore : '0';"
                  "} else {"
                    "nhlAway.textContent='STL';"
                    "nhlAway.classList.add('pink');"
                    "document.getElementById('nhlAwayScore').textContent=sp.nhl.stlScore!=='' ? sp.nhl.stlScore : '0';"
                    "nhlHome.textContent=sp.nhl.oppAbbrev||'OPP';"
                    "document.getElementById('nhlHomeScore').textContent=sp.nhl.oppScore!=='' ? sp.nhl.oppScore : '0';"
                  "}"
                "} else {"
                  "document.getElementById('nhlStatus').textContent='No Game';"
                  "nhlAway.textContent='STL';"
                  "document.getElementById('nhlAwayScore').textContent='--';"
                  "nhlHome.textContent='OPP';"
                  "document.getElementById('nhlHomeScore').textContent='--';"
                "}"
                "document.getElementById('stamp').textContent='Telemetry synced at '+new Date().toLocaleTimeString();"
              "}catch(e){"
                "document.getElementById('stamp').textContent='Telemetry Sync Failed: '+e;"
              "}finally{"
                "refreshing=false;"
                "document.getElementById('refresh').textContent='Sync Core Metrics';"
              "}"
            "}"
            "document.getElementById('refresh').addEventListener('click',refresh);"
            "refresh();"
            "setInterval(refresh,15000);" // Auto-refresh telemetry every 15 seconds
            "</script></body></html>";
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
    html += "<label for='autoScrollInterval'>Screen Rotation Interval (seconds)</label>";
    html += "<input id='autoScrollInterval' name='autoScrollInterval' type='text' maxlength='4' value='" + String(Settings.autoScrollInterval) + "'>";
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
        if (address.length() > 79) address = address.substring(0, 79);
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
        if (pass.length() > 79) pass = pass.substring(0, 79);
        strncpy(Settings.PoolPassword, pass.c_str(), sizeof(Settings.PoolPassword) - 1);
        Settings.PoolPassword[sizeof(Settings.PoolPassword) - 1] = '\0';
    }

    if (settingsWebServer.hasArg("btcWallet")) {
        String wallet = settingsWebServer.arg("btcWallet");
        wallet.trim();
        if (wallet.length() > 79) wallet = wallet.substring(0, 79);
        strncpy(Settings.BtcWallet, wallet.c_str(), sizeof(Settings.BtcWallet) - 1);
        Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';
    }

    if (settingsWebServer.hasArg("timezone")) {
        int tz = settingsWebServer.arg("timezone").toInt();
        if (tz < -12) tz = -12;
        if (tz > 14) tz = 14;
        Settings.Timezone = tz;
    }

    if (settingsWebServer.hasArg("autoScrollInterval")) {
        int interval = settingsWebServer.arg("autoScrollInterval").toInt();
        if (interval < 3) interval = 3;
        if (interval > 3600) interval = 3600;
        Settings.autoScrollInterval = interval;
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

    // Safely copy sports and sentiment data under mutex
    TickerData localTicker;
    SportsData localMlb;
    SportsData localNhl;

    if (tickerDataMutex != nullptr && xSemaphoreTake(tickerDataMutex, 5 / portTICK_PERIOD_MS) == pdTRUE) {
        localTicker = tickerCache;
        xSemaphoreGive(tickerDataMutex);
    }
    if (sportsDataMutex != nullptr && xSemaphoreTake(sportsDataMutex, 5 / portTICK_PERIOD_MS) == pdTRUE) {
        localMlb = mlbCache;
        localNhl = nhlCache;
        xSemaphoreGive(sportsDataMutex);
    }

    DynamicJsonDocument doc(2048); // Increased size for additional nested sections
    doc["ip"] = getUiIpAddress();

    JsonObject mining = doc.createNestedObject("mining");
    mining["hashrate"] = sanitizeMetric(miningData.currentHashRate, "0.00");
    mining["shares"] = miningData.completedShares;
    mining["valids"] = miningData.valids;
    mining["uptime"] = miningData.timeMining;
    mining["bestDiff"] = miningData.bestDiff;
    mining["submitAttempts"] = miningData.completedShares;
    mining["poolDifficulty"] = String(currentPoolDifficultyLive, 6);

    JsonObject pool = doc.createNestedObject("pool");
    pool["workersCount"] = pData.workersCount;
    pool["workersHash"] = sanitizeMetric(pData.workersHash, "--");
    pool["bestDifficulty"] = sanitizeMetric(pData.bestDifficulty, "--");
    pool["apiUrl"] = getPoolAPIUrl();

    JsonObject blockchain = doc.createNestedObject("blockchain");
    blockchain["btcPrice"] = bitcoin_price;
    blockchain["blockHeight"] = current_block;
    blockchain["difficulty"] = sanitizeMetric(gData.difficulty, "--");
    blockchain["halfHourFee"] = gData.halfHourFee;
    blockchain["remainingBlocks"] = gData.remainingBlocks;
    blockchain["progressPercent"] = gData.progressPercent;

    JsonObject sentiment = doc.createNestedObject("sentiment");
    sentiment["greeting"] = sanitizeMetric(localTicker.greeting, "Welcome, Patrick!");
    sentiment["score"] = sanitizeMetric(localTicker.sentiment, "--");
    sentiment["direction"] = sanitizeMetric(localTicker.direction, "--");
    sentiment["category"] = sanitizeMetric(localTicker.category, "--");
    sentiment["recommendation"] = sanitizeMetric(localTicker.recommendation, "--");
    sentiment["btcDirection"] = sanitizeMetric(localTicker.btcDirection, "--");
    sentiment["btcEntry"] = sanitizeMetric(localTicker.btcEntry, "--");
    sentiment["btcTakeProfit"] = sanitizeMetric(localTicker.btcTakeProfit, "--");
    sentiment["xrpDirection"] = sanitizeMetric(localTicker.xrpDirection, "--");
    sentiment["xrpEntry"] = sanitizeMetric(localTicker.xrpEntry, "--");
    sentiment["xrpTakeProfit"] = sanitizeMetric(localTicker.xrpTakeProfit, "--");

    JsonObject sports = doc.createNestedObject("sports");
    JsonObject mlb = sports.createNestedObject("mlb");
    mlb["hasGame"] = localMlb.hasGame;
    mlb["shortName"] = sanitizeMetric(localMlb.shortName, "");
    mlb["status"] = sanitizeMetric(localMlb.status, "");
    mlb["stlScore"] = sanitizeMetric(localMlb.stlScore, "");
    mlb["oppScore"] = sanitizeMetric(localMlb.oppScore, "");
    mlb["oppAbbrev"] = sanitizeMetric(localMlb.oppAbbrev, "");
    mlb["isStlHome"] = localMlb.isStlHome;

    JsonObject nhl = sports.createNestedObject("nhl");
    nhl["hasGame"] = localNhl.hasGame;
    nhl["shortName"] = sanitizeMetric(localNhl.shortName, "");
    nhl["status"] = sanitizeMetric(localNhl.status, "");
    nhl["stlScore"] = sanitizeMetric(localNhl.stlScore, "");
    nhl["oppScore"] = sanitizeMetric(localNhl.oppScore, "");
    nhl["oppAbbrev"] = sanitizeMetric(localNhl.oppAbbrev, "");
    nhl["isStlHome"] = localNhl.isStlHome;

    JsonObject sys = doc.createNestedObject("system");
    sys["freeHeap"] = ESP.getFreeHeap();
    sys["minFreeHeap"] = ESP.getMinFreeHeap();
    sys["heapSize"] = ESP.getHeapSize();
    sys["maxAllocHeap"] = ESP.getMaxAllocHeap();

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
    settingsWebServer.onNotFound([]() {
        settingsWebServer.sendHeader("Location", "/", true);
        settingsWebServer.send(302, "text/plain", "");
    });
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
            if (Settings.PoolAddress.length() > 79) Settings.PoolAddress = Settings.PoolAddress.substring(0, 79);
            Settings.PoolPort = atoi(port_text_box_num.getValue());
            normalizePoolEndpoint(Settings.PoolAddress, Settings.PoolPort);
            strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword) - 1);
            Settings.PoolPassword[sizeof(Settings.PoolPassword) - 1] = '\0';
            strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet) - 1);
            Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';
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
                if (Settings.PoolAddress.length() > 79) Settings.PoolAddress = Settings.PoolAddress.substring(0, 79);
                Settings.PoolPort = atoi(port_text_box_num.getValue());
                normalizePoolEndpoint(Settings.PoolAddress, Settings.PoolPort);
                strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword) - 1);
                Settings.PoolPassword[sizeof(Settings.PoolPassword) - 1] = '\0';
                strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet) - 1);
                Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';
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
        WiFi.setSleep(false); // Disable WiFi sleep mode for connection stability



        // Lets deal with the user config values

        // Copy the string value
        Settings.PoolAddress = pool_text_box.getValue();
        if (Settings.PoolAddress.length() > 79) Settings.PoolAddress = Settings.PoolAddress.substring(0, 79);
        Serial.print("PoolString: ");
        Serial.println(Settings.PoolAddress);

        //Convert the number value
        Settings.PoolPort = atoi(port_text_box_num.getValue());
        normalizePoolEndpoint(Settings.PoolAddress, Settings.PoolPort);
        Serial.print("portNumber: ");
        Serial.println(Settings.PoolPort);

        // Copy the string value
        strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword) - 1);
        Settings.PoolPassword[sizeof(Settings.PoolPassword) - 1] = '\0';
        Serial.print("poolPassword: ");
        Serial.println(Settings.PoolPassword);

        // Copy the string value
        strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet) - 1);
        Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';
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
    if (Settings.PoolAddress.length() > 79) Settings.PoolAddress = Settings.PoolAddress.substring(0, 79);
    Serial.print("PoolString: ");
    Serial.println(Settings.PoolAddress);

    //Convert the number value
    Settings.PoolPort = atoi(port_text_box_num.getValue());
    normalizePoolEndpoint(Settings.PoolAddress, Settings.PoolPort);
    Serial.print("portNumber: ");
    Serial.println(Settings.PoolPort);

    // Copy the string value
    strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword) - 1);
    Settings.PoolPassword[sizeof(Settings.PoolPassword) - 1] = '\0';
    Serial.print("poolPassword: ");
    Serial.println(Settings.PoolPassword);

    // Copy the string value
    strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet) - 1);
    Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';
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
            WiFi.setSleep(false); // Disable WiFi sleep mode for connection stability
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
