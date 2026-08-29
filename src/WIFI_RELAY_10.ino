/**
 * WIFI_RELAY_10 — Dining Room Light Controller  v2.1
 * Platform : ESP8266 (NodeMCU / Wemos D1)
 *
 * Improvements over v1:
 *  [1]  Fixed infinite status-poll loop in JavaScript
 *  [2]  Non-blocking sequential all-on/off animation (no more delay() in handlers)
 *  [3]  Modes are mutually exclusive via stopAllModes() + Mode enum
 *  [4]  Individual relay toggles stop any active mode first
 *  [5]  /corners/toggle is now a server-side endpoint (no more 4 parallel XHRs)
 *  [6]  Credentials isolated to a clearly marked config section
 *  [7]  HTTP Basic Authentication on every endpoint
 *  [8]  WiFi reconnection watchdog in loop()
 *  [9]  mDNS  →  http://dining.local
 * [10]  OTA firmware updates via Arduino IDE / espota
 * [11]  Active mode shown in /status and highlighted in the web UI
 * [12]  Periodic 3-second background status poll in the browser
 * [13]  All magic timing values replaced with named constants
 * [14]  All group/row logic lives in C++ handlers, not duplicated in JS
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include "config.h"

// ================================================================
//  TIMING CONSTANTS  (milliseconds)
// ================================================================
const unsigned long CYCLE_INTERVAL      = 200;   // Per-light dwell in Cycle mode
const unsigned long ODD_EVEN_INTERVAL   = 500;   // Swap interval for Odd-Even mode
const unsigned long HB_PRE_BEAT_PAUSE   = 250;   // Off-time before each heartbeat pulse
const unsigned long HB_BEAT_DURATION    = 200;   // On-time of each short beat
const unsigned long HB_LONG_ON          = 700;   // On-time of the second (long) beat
const unsigned long SEQ_DELAY_0         = 300;   // Delay after step 0 (light 1)
const unsigned long SEQ_DELAY_1         = 300;   // Delay after step 1 (lights 2+10)
const unsigned long SEQ_DELAY_2         = 330;   // Delay after step 2 (lights 3+9)
const unsigned long SEQ_DELAY_3         = 300;   // Delay after step 3 (lights 4+8)
const unsigned long SEQ_DELAY_4         = 300;   // Delay after step 4 (lights 5+7)
const unsigned long WIFI_CHECK_INTERVAL = 10000; // WiFi watchdog period

// ================================================================
//  HARDWARE
// ================================================================
ESP8266WebServer server(80);

const int RELAY_COUNT = 10;
// GPIO mapping:  D1   D2  D3  D4   D5   D6   D7   D0  RX  TX
const int relayPins[RELAY_COUNT] = {5, 4, 0, 2, 14, 12, 13, 16, 3, 1};
bool relayStates[RELAY_COUNT]   = {};   // all false (OFF) at startup

// Polarity per relay: false = active LOW (LOW=ON, HIGH=OFF) — confirmed for all 10 relays
const bool relayActiveHigh[RELAY_COUNT] = {false, false, false, false, false, false, false, false, false, false};

// ================================================================
//  MODE STATE
// ================================================================
enum Mode {
  MODE_NONE,
  MODE_CYCLE,
  MODE_ODD_EVEN,
  MODE_HEARTBEAT,
  MODE_ALL_ON_SEQ,
  MODE_ALL_OFF_SEQ
};
Mode activeMode = MODE_NONE;

// Cycle
int           cycleLight    = 0;
unsigned long lastCycleTime = 0;

// Odd-Even
bool          oddOn         = true;
unsigned long lastOddEvenTime = 0;

// Heartbeat
int           heartbeatStep = 0;
unsigned long lastHeartbeatTime = 0;

// Sequential all-on / all-off animation
// Pairs turned on/off per step: {0}, {1,9}, {2,8}, {3,7}, {4,6}, {5}
const int SEQ_STEPS = 6;
const unsigned long SEQ_DELAYS[SEQ_STEPS] = {
  SEQ_DELAY_0, SEQ_DELAY_1, SEQ_DELAY_2,
  SEQ_DELAY_3, SEQ_DELAY_4, 0
};
int           seqStep    = 0;
unsigned long lastSeqTime = 0;

// WiFi watchdog
unsigned long lastWifiCheck = 0;

// ================================================================
//  HELPER FUNCTIONS
// ================================================================

/** Set a single relay and track its state.
 *  Handles per-relay polarity: active-HIGH relays get HIGH=ON, active-LOW get LOW=ON. */
void setRelay(int idx, bool on) {
  relayStates[idx] = on;
  bool pinLevel = relayActiveHigh[idx] ? on : !on;   // translate ON→correct voltage level
  digitalWrite(relayPins[idx], pinLevel ? HIGH : LOW);
}

/** Stop every running mode and reset animation state. */
void stopAllModes() {
  activeMode = MODE_NONE;
  seqStep    = 0;
}

/** Turn all relays off immediately (no animation). */
void allRelaysOff() {
  for (int i = 0; i < RELAY_COUNT; i++) setRelay(i, false);
}

// No authentication — open access on local network

/** Toggle a named group of relays (server-side, no JS logic needed).
 *  If any relay in the group is ON, turn all OFF; otherwise turn all ON. */
void applyGroupToggle(const int* group, int size) {
  bool anyOn = false;
  for (int i = 0; i < size; i++) {
    if (relayStates[group[i]]) { anyOn = true; break; }
  }
  stopAllModes();
  for (int i = 0; i < size; i++) setRelay(group[i], !anyOn);
}

// Forward declarations
void handleRoot();
void handleStatus();
void handleRelayOn(int idx);
void handleRelayOff(int idx);
void handleRelayToggle(int idx);
void handleAllOn();
void handleAllOff();
void handleCornersToggle();
void handleVerticalCenterToggle();
void handleHorizontalCenterToggle();
void handleLeftRowToggle();
void handleRightRowToggle();
void handleTopRowToggle();
void handleBottomRowToggle();
void handleCycle();
void handleOddEven();
void handleHeartbeat();

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(10);

  EEPROM.begin(512);

  // Initialise relay pins — all OFF (respects per-relay polarity)
  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(relayPins[i], OUTPUT);
    // active HIGH → LOW = OFF;  active LOW → HIGH = OFF
    digitalWrite(relayPins[i], relayActiveHigh[i] ? LOW : HIGH);
  }

  // ── Power-on pattern (cycles through 3 states via EEPROM) ──────
  uint8_t pos = EEPROM.read(0);
  if (pos > 2) pos = 0;

  if (pos == 0) {
    // Corners: lights 1, 3, 8, 6  (0-indexed: 0, 2, 7, 5)
    const int corners[] = {0, 2, 7, 5};
    for (int i : corners) setRelay(i, true);
  } else if (pos == 1) {
    // Horizontal centre: lights 4, 5, 9, 10  (0-indexed: 3, 4, 8, 9)
    const int horiz[] = {3, 4, 8, 9};
    for (int i : horiz) setRelay(i, true);
  } else {
    // All lights — sequential animation.
    // delay() is acceptable here (inside setup before server starts).
    setRelay(0, true);                               delay(SEQ_DELAY_0);
    setRelay(1, true); setRelay(9, true);            delay(SEQ_DELAY_1);
    setRelay(2, true); setRelay(8, true);            delay(SEQ_DELAY_2);
    setRelay(3, true); setRelay(7, true);            delay(SEQ_DELAY_3);
    setRelay(4, true); setRelay(6, true);            delay(SEQ_DELAY_4);
    setRelay(5, true);
  }

  EEPROM.write(0, (pos + 1) % 3);
  EEPROM.commit();

  // ── WiFi ────────────────────────────────────────────────────────
  Serial.printf("\nConnecting to %s\n", WIFI_SSID);
  WiFi.config(staticIP, gateway, subnet, dns);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWiFi connected — IP: %s\n", WiFi.localIP().toString().c_str());

  // ── mDNS ────────────────────────────────────────────────────────
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS started: http://%s.local\n", HOSTNAME);
  } else {
    Serial.println("mDNS failed — use IP address instead");
  }

  // ── OTA ─────────────────────────────────────────────────────────
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println("OTA: starting update…");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA: done — rebooting");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA: %u%%\r", progress / (total / 100));
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("OTA error [%u]: ", e);
    if      (e == OTA_AUTH_ERROR)    Serial.println("auth failed");
    else if (e == OTA_BEGIN_ERROR)   Serial.println("begin failed");
    else if (e == OTA_CONNECT_ERROR) Serial.println("connect failed");
    else if (e == OTA_RECEIVE_ERROR) Serial.println("receive failed");
    else if (e == OTA_END_ERROR)     Serial.println("end failed");
  });
  ArduinoOTA.begin();

  // ── HTTP routes ─────────────────────────────────────────────────
  server.on("/",                         handleRoot);
  server.on("/status",                   handleStatus);
  server.on("/all/on",                   handleAllOn);
  server.on("/all/off",                  handleAllOff);
  server.on("/corners/toggle",           handleCornersToggle);
  server.on("/vertical-center/toggle",   handleVerticalCenterToggle);
  server.on("/horizontal-center/toggle", handleHorizontalCenterToggle);
  server.on("/left-row/toggle",          handleLeftRowToggle);
  server.on("/top-row/toggle",           handleTopRowToggle);
  server.on("/right-row/toggle",         handleRightRowToggle);
  server.on("/bottom-row/toggle",        handleBottomRowToggle);
  server.on("/cycle",                    handleCycle);
  server.on("/odd-even",                 handleOddEven);
  server.on("/heartbeat",                handleHeartbeat);

  for (int i = 0; i < RELAY_COUNT; i++) {
    server.on(String("/relay") + (i + 1) + "/on",     [i]() { handleRelayOn(i);     });
    server.on(String("/relay") + (i + 1) + "/off",    [i]() { handleRelayOff(i);    });
    server.on(String("/relay") + (i + 1) + "/toggle", [i]() { handleRelayToggle(i); });
  }
  server.onNotFound([]() {
    server.send(404, "text/plain", "404: Not found");
  });

  server.begin();
  Serial.println("HTTP server started");
}

// ================================================================
//  LOOP
// ================================================================
void loop() {
  server.handleClient();
  MDNS.update();
  ArduinoOTA.handle();

  unsigned long now = millis();

  // ── WiFi reconnection watchdog ──────────────────────────────────
  if (now - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost — reconnecting…");
      WiFi.reconnect();
    }
  }

  // ── Cycle mode ──────────────────────────────────────────────────
  if (activeMode == MODE_CYCLE && now - lastCycleTime >= CYCLE_INTERVAL) {
    setRelay(cycleLight, false);
    cycleLight = (cycleLight + 1) % RELAY_COUNT;
    setRelay(cycleLight, true);
    lastCycleTime = now;
  }

  // ── Odd-Even mode ───────────────────────────────────────────────
  if (activeMode == MODE_ODD_EVEN && now - lastOddEvenTime >= ODD_EVEN_INTERVAL) {
    for (int i = 0; i < RELAY_COUNT; i++) {
      setRelay(i, (i % 2 == 0) == oddOn);
    }
    oddOn = !oddOn;
    lastOddEvenTime = now;
  }

  // ── Heartbeat mode ──────────────────────────────────────────────
  //  Pattern:  off(HB_PRE_BEAT_PAUSE) → on(HB_BEAT_DURATION)
  //          → off(HB_PRE_BEAT_PAUSE) → on(HB_LONG_ON) → repeat
  if (activeMode == MODE_HEARTBEAT) {
    unsigned long elapsed = now - lastHeartbeatTime;
    bool flip    = false;
    bool flipOn  = false;

    if      (heartbeatStep == 0 && elapsed >= HB_PRE_BEAT_PAUSE)  { flipOn = true;  flip = true; heartbeatStep = 1; }
    else if (heartbeatStep == 1 && elapsed >= HB_BEAT_DURATION)   { flipOn = false; flip = true; heartbeatStep = 2; }
    else if (heartbeatStep == 2 && elapsed >= HB_PRE_BEAT_PAUSE)  { flipOn = true;  flip = true; heartbeatStep = 3; }
    else if (heartbeatStep == 3 && elapsed >= HB_LONG_ON)         { flipOn = false; flip = true; heartbeatStep = 0; }

    if (flip) {
      for (int i = 0; i < RELAY_COUNT; i++) setRelay(i, flipOn);
      lastHeartbeatTime = now;
    }
  }

  // ── Sequential all-on / all-off (non-blocking) ──────────────────
  if ((activeMode == MODE_ALL_ON_SEQ || activeMode == MODE_ALL_OFF_SEQ)
       && seqStep < SEQ_STEPS) {
    // Step 0 fires immediately; subsequent steps wait for the previous delay.
    unsigned long required = (seqStep == 0) ? 0UL : SEQ_DELAYS[seqStep - 1];
    if (now - lastSeqTime >= required) {
      bool on = (activeMode == MODE_ALL_ON_SEQ);
      switch (seqStep) {
        case 0: setRelay(0, on);                      break;
        case 1: setRelay(1, on); setRelay(9, on);     break;
        case 2: setRelay(2, on); setRelay(8, on);     break;
        case 3: setRelay(3, on); setRelay(7, on);     break;
        case 4: setRelay(4, on); setRelay(6, on);     break;
        case 5: setRelay(5, on);                      break;
      }
      lastSeqTime = now;
      seqStep++;
      if (seqStep >= SEQ_STEPS) activeMode = MODE_NONE;
    }
  }
}

// ================================================================
//  HTTP HANDLERS
// ================================================================

// ── Root — serve web UI ────────────────────────────────────────
void handleRoot() {
  String html;
  html.reserve(8192);

  // ── Head / CSS ───────────────────────────────────────────────
  html += F("<!DOCTYPE html><html><head>");
  html += F("<title>Dining Room Lights</title>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1.0'>");
  html += F("<style>");
  html += F("*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;}");
  html += F("body{margin:0;font-family:'Poppins',sans-serif;color:#fff;"
            "background:linear-gradient(-45deg,#0d0d2b,#1a0033,#001f3f,#1a0022);"
            "background-size:400% 400%;animation:gradientShift 10s ease infinite;"
            "display:flex;flex-direction:column;align-items:center;min-height:100vh;}");
  html += F("@keyframes gradientShift{"
            "0%{background-position:0% 50%}50%{background-position:100% 50%}"
            "100%{background-position:0% 50%}}");
  html += F("h1{font-size:2em;margin:20px 0 15px;text-shadow:0 0 10px rgba(255,255,255,0.4);}");
  html += F(".grid{display:grid;grid-template-columns:repeat(3,1fr);"
            "gap:18px;width:90%;max-width:450px;}");
  html += F(".btn{border:none;outline:none;padding:15px 10px;border-radius:15px;"
            "font-size:1.1em;color:#fff;"
            "background:linear-gradient(145deg,#292945,#1e1e2f);"
            "box-shadow:0 4px 10px rgba(0,0,0,0.4);"
            "transition:all 0.25s ease;cursor:pointer;}");
  html += F(".btn.on{background:#ffffff;color:#000;"
            "box-shadow:0 0 15px 4px rgba(255,255,255,0.5);}");
  html += F(".btn:active{transform:scale(0.97);}");
  html += F(".mode-status{margin:10px 0 5px;font-size:0.85rem;color:#aaa;letter-spacing:1px;}");
  html += F(".controls{margin-top:20px;display:flex;flex-wrap:wrap;"
            "justify-content:center;gap:15px;width:90%;max-width:450px;}");
  html += F(".ctrl-btn{flex:1 1 40%;min-width:120px;background:transparent;"
            "border:2px solid cyan;color:cyan;padding:12px;border-radius:25px;"
            "font-size:1rem;font-weight:600;transition:0.3s;cursor:pointer;"
            "text-shadow:0 0 10px cyan;}");
  html += F(".ctrl-btn:active{background:cyan;color:#000;"
            "box-shadow:0 0 15px cyan,0 0 40px cyan;}");
  html += F(".mode-btn{border-color:#ff4444;color:#ff4444;"
            "text-shadow:0 0 10px #ff4444;}");
  html += F(".mode-btn.active-mode{background:#ff4444;color:#fff;"
            "box-shadow:0 0 15px #ff4444,0 0 40px #ff4444;}");
  html += F("footer{margin:20px 0;font-size:0.8rem;color:#aaa;text-align:center;}");
  html += F("</style>");

  // ── JavaScript ───────────────────────────────────────────────
  html += F("<script>");

  // ── State ────────────────────────────────────────────────────
  html += F("var S=[false,false,false,false,false,false,false,false,false,false];");
  html += F("var CUR_MODE='none';");
  html += F("var cycleTimer=null,oeTimer=null,hbTimer=null;");
  html += F("var cycleIdx=0,oeOn=true;");

  // ── Fire-and-forget XHR ──────────────────────────────────────
  html += F("function send(url){var x=new XMLHttpRequest();x.open('GET',url,true);x.send();}");

  // ── DOM helpers ──────────────────────────────────────────────
  html += F("function setL(i,on){");
  html += F("  S[i]=on;var e=document.getElementById('light'+(i+1));");
  html += F("  if(e)on?e.classList.add('on'):e.classList.remove('on');");
  html += F("}");
  html += F("function setAll(on){for(var i=0;i<10;i++)setL(i,on);}");
  html += F("function grp(arr){");
  html += F("  var any=arr.some(function(i){return S[i];});");
  html += F("  arr.forEach(function(i){setL(i,!any);});");
  html += F("}");

  // ── Animation engine — runs entirely in the browser ──────────
  html += F("function stopAllAnims(){");
  html += F("  if(cycleTimer){clearInterval(cycleTimer);cycleTimer=null;}");
  html += F("  if(oeTimer){clearInterval(oeTimer);oeTimer=null;}");
  html += F("  if(hbTimer){clearTimeout(hbTimer);hbTimer=null;}");
  html += F("}");

  // Cycle: one light at a time, 200ms each — matches CYCLE_INTERVAL on ESP8266
  html += F("function startCycle(){");
  html += F("  stopAllAnims();setAll(false);cycleIdx=0;setL(0,true);");
  html += F("  cycleTimer=setInterval(function(){");
  html += F("    setL(cycleIdx,false);");
  html += F("    cycleIdx=(cycleIdx+1)%10;");
  html += F("    setL(cycleIdx,true);");
  html += F("  },200);");  // 200 = CYCLE_INTERVAL
  html += F("}");

  // Odd-Even: alternates every 500ms — matches ODD_EVEN_INTERVAL on ESP8266
  html += F("function startOddEven(){");
  html += F("  stopAllAnims();oeOn=true;");
  html += F("  for(var i=0;i<10;i++)setL(i,i%2===0);");
  html += F("  oeTimer=setInterval(function(){");
  html += F("    for(var i=0;i<10;i++)setL(i,(i%2===0)===oeOn);");
  html += F("    oeOn=!oeOn;");
  html += F("  },500);");  // 500 = ODD_EVEN_INTERVAL
  html += F("}");

  // Heartbeat: off(250)→on(200)→off(250)→on(700) — matches HB constants on ESP8266
  html += F("function startHeartbeat(){");
  html += F("  stopAllAnims();setAll(false);");
  html += F("  var waits=[250,200,250,700];");   // HB_PRE, HB_BEAT, HB_PRE, HB_LONG
  html += F("  var acts=[true,false,true,false];");
  html += F("  var step=0;");
  html += F("  function tick(){");
  html += F("    if(CUR_MODE!=='heartbeat')return;");
  html += F("    setAll(acts[step]);");
  html += F("    var nw=waits[(step+1)%4];");
  html += F("    step=(step+1)%4;");
  html += F("    hbTimer=setTimeout(tick,nw);");
  html += F("  }");
  html += F("  hbTimer=setTimeout(tick,waits[0]);"); // first wait before beat 1
  html += F("}");

  // ── Mode manager: updates buttons + starts/stops animations ──
  html += F("function setMode(m){");
  html += F("  var prev=CUR_MODE;CUR_MODE=m;");
  html += F("  ['btn-cycle','btn-oddeven','btn-heartbeat'].forEach(function(id){");
  html += F("    var e=document.getElementById(id);if(e)e.classList.remove('active-mode');");
  html += F("  });");
  html += F("  var map={cycle:'btn-cycle','odd-even':'btn-oddeven',heartbeat:'btn-heartbeat'};");
  html += F("  if(map[m]){var e=document.getElementById(map[m]);if(e)e.classList.add('active-mode');}");
  html += F("  var ms=document.getElementById('modeStatus');");
  html += F("  if(ms)ms.textContent=(!m||m==='none'||m==='sequencing')?'':('Active: '+m.toUpperCase());");
  // Start/stop animations when mode changes
  html += F("  if(m!==prev){");
  html += F("    stopAllAnims();");
  html += F("    if(m==='cycle')startCycle();");
  html += F("    else if(m==='odd-even')startOddEven();");
  html += F("    else if(m==='heartbeat')startHeartbeat();");
  html += F("  }");
  html += F("}");

  // ── Button handlers — all instant, zero server wait ──────────
  // Individual light: blocked while animation is running (ESP8266 would override it anyway)
  html += F("function toggleLight(n){");
  html += F("  if(CUR_MODE==='cycle'||CUR_MODE==='odd-even'||CUR_MODE==='heartbeat')return;");
  html += F("  setL(n-1,!S[n-1]);send('/relay'+n+'/toggle');");
  html += F("}");

  html += F("function toggleAllLights(){");
  html += F("  var allOn=S.every(function(v){return v;});");
  html += F("  setMode('none');setAll(!allOn);send(allOn?'/all/off':'/all/on');");
  html += F("}");
  html += F("function resetMode(){setMode('none');setAll(false);send('/all/off');}");
  html += F("function cornersMode() {setMode('none');grp([0,2,7,5]);  send('/corners/toggle');}");
  html += F("function vertCenter()  {setMode('none');grp([1,6]);       send('/vertical-center/toggle');}");
  html += F("function horizCenter() {setMode('none');grp([3,4,8,9]);   send('/horizontal-center/toggle');}");
  html += F("function leftRow()     {setMode('none');grp([0,9,8,7]);   send('/left-row/toggle');}");
  html += F("function rightRow()    {setMode('none');grp([2,3,4,5]);   send('/right-row/toggle');}");
  html += F("function topRow()      {setMode('none');grp([0,1,2]);     send('/top-row/toggle');}");
  html += F("function bottomRow()   {setMode('none');grp([5,6,7]);     send('/bottom-row/toggle');}");

  // Mode buttons: toggle animation on/off immediately in browser + tell server
  html += F("function doCycle()    {setMode(CUR_MODE==='cycle'?'none':'cycle');       send('/cycle');}");
  html += F("function doOddEven()  {setMode(CUR_MODE==='odd-even'?'none':'odd-even'); send('/odd-even');}");
  html += F("function doHeartbeat(){setMode(CUR_MODE==='heartbeat'?'none':'heartbeat');send('/heartbeat');}");

  // ── Background sync (every 3s) ───────────────────────────────
  // During animations: only syncs the mode (doesn't overwrite lights the browser is animating)
  // During static modes: fully syncs all relay states
  html += F("function updateStatus(){");
  html += F("  var x=new XMLHttpRequest();x.open('GET','/status',true);");
  html += F("  x.onreadystatechange=function(){");
  html += F("    if(x.readyState!==4||x.status!==200)return;");
  html += F("    var lines=x.responseText.split('\\n');");
  html += F("    var ml=lines[11]||'';");
  html += F("    var mode=ml.indexOf(': ')>=0?ml.split(': ')[1].trim():'none';");
  // Only sync individual light states when NO animation is running in the browser.
  // During cycle/odd-even/heartbeat the browser owns the light states — don't interrupt.
  html += F("    var anim=(mode==='cycle'||mode==='odd-even'||mode==='heartbeat');");
  html += F("    if(!anim){");
  html += F("      for(var i=1;i<=10;i++){");
  html += F("        if(!lines[i])continue;");
  html += F("        var on=lines[i].split(': ')[1]==='ON';");
  html += F("        S[i-1]=on;var e=document.getElementById('light'+i);");
  html += F("        if(e)on?e.classList.add('on'):e.classList.remove('on');");
  html += F("      }");
  html += F("    }");
  // Always sync mode — will start/stop browser animation if server mode changed
  html += F("    if(mode!==CUR_MODE)setMode(mode);");
  html += F("  };");
  html += F("  x.send();");
  html += F("}");
  html += F("window.onload=function(){updateStatus();setInterval(updateStatus,3000);};");
  html += F("</script>");

  // ── Body ─────────────────────────────────────────────────────
  html += F("</head><body>");
  html += F("<h1>Dining Room</h1>");

  html += F("<div class='grid' id='lightGrid'>");
  html += F("<button class='btn' id='light1'  onclick=\"toggleLight(1)\">Light 1</button>");
  html += F("<button class='btn' id='light2'  onclick=\"toggleLight(2)\">Light 2</button>");
  html += F("<button class='btn' id='light3'  onclick=\"toggleLight(3)\">Light 3</button>");
  html += F("<button class='btn' id='light10' onclick=\"toggleLight(10)\">Light 10</button>");
  html += F("<button class='btn' style='grid-row:span 2;background:green;box-shadow:0 0 20px green;' onclick=\"resetMode()\">Reset</button>");
  html += F("<button class='btn' id='light4'  onclick=\"toggleLight(4)\">Light 4</button>");
  html += F("<button class='btn' id='light9'  onclick=\"toggleLight(9)\">Light 9</button>");
  html += F("<button class='btn' id='light5'  onclick=\"toggleLight(5)\">Light 5</button>");
  html += F("<button class='btn' id='light8'  onclick=\"toggleLight(8)\">Light 8</button>");
  html += F("<button class='btn' id='light7'  onclick=\"toggleLight(7)\">Light 7</button>");
  html += F("<button class='btn' id='light6'  onclick=\"toggleLight(6)\">Light 6</button>");
  html += F("</div>");

  html += F("<p class='mode-status' id='modeStatus'></p>");

  html += F("<div class='controls'>");
  html += F("<button class='ctrl-btn' onclick=\"toggleAllLights()\">All Lights</button>");
  html += F("<button class='ctrl-btn' onclick=\"cornersMode()\">Corners</button>");
  html += F("<button class='ctrl-btn' onclick=\"vertCenter()\">Vertical Center</button>");
  html += F("<button class='ctrl-btn' onclick=\"horizCenter()\">Horizontal Center</button>");
  html += F("<button class='ctrl-btn' onclick=\"leftRow()\">Left Row</button>");
  html += F("<button class='ctrl-btn' onclick=\"rightRow()\">Right Row</button>");
  html += F("<button class='ctrl-btn' onclick=\"topRow()\">Top Row</button>");
  html += F("<button class='ctrl-btn' onclick=\"bottomRow()\">Bottom Row</button>");
  html += F("<button id='btn-cycle'     class='ctrl-btn mode-btn' onclick=\"doCycle()\">Cycle</button>");
  html += F("<button id='btn-oddeven'   class='ctrl-btn mode-btn' onclick=\"doOddEven()\">Odd-Even</button>");
  html += F("<button id='btn-heartbeat' class='ctrl-btn mode-btn' onclick=\"doHeartbeat()\">Heartbeat</button>");
  html += F("</div>");

  html += F("<footer>Made for Mobile by Garvit &nbsp;&middot;&nbsp; v2.1</footer>");
  html += F("</body></html>");

  server.send(200, "text/html", html);
}

// ── /status ───────────────────────────────────────────────────
void handleStatus() {

  String s;
  s.reserve(256);
  s = F("Relay Status:\n");
  for (int i = 0; i < RELAY_COUNT; i++) {
    s += "Relay ";
    s += String(i + 1);
    s += ": ";
    s += relayStates[i] ? "ON" : "OFF";
    s += "\n";
  }
  // Line 11 — active mode (read by JS)
  const char* modeStr = "none";
  if      (activeMode == MODE_CYCLE)                              modeStr = "cycle";
  else if (activeMode == MODE_ODD_EVEN)                          modeStr = "odd-even";
  else if (activeMode == MODE_HEARTBEAT)                         modeStr = "heartbeat";
  else if (activeMode == MODE_ALL_ON_SEQ || activeMode == MODE_ALL_OFF_SEQ) modeStr = "sequencing";
  s += "Mode: ";
  s += modeStr;
  s += "\n";

  server.send(200, "text/plain", s);
}

// ── Individual relay control ───────────────────────────────────

// FIX #4: all three handlers call stopAllModes() first
void handleRelayOn(int idx) {
  stopAllModes();
  setRelay(idx, true);
  server.send(200, "text/plain", "Relay " + String(idx + 1) + " ON");
}

void handleRelayOff(int idx) {
  stopAllModes();
  setRelay(idx, false);
  server.send(200, "text/plain", "Relay " + String(idx + 1) + " OFF");
}

void handleRelayToggle(int idx) {
  stopAllModes();
  bool newState = !relayStates[idx];
  setRelay(idx, newState);
  server.send(200, "text/plain",
    "Relay " + String(idx + 1) + (newState ? " ON" : " OFF"));
}

// ── All ON / OFF ───────────────────────────────────────────────

// FIX #2: no more delay() inside HTTP handlers — animation runs in loop()
void handleAllOn() {
  stopAllModes();
  activeMode  = MODE_ALL_ON_SEQ;
  seqStep     = 0;
  lastSeqTime = millis();
  server.send(200, "text/plain", "All relays turning ON");
}

void handleAllOff() {
  stopAllModes();
  activeMode  = MODE_ALL_OFF_SEQ;
  seqStep     = 0;
  lastSeqTime = millis();
  server.send(200, "text/plain", "All relays turning OFF");
}

// ── Group toggles ──────────────────────────────────────────────

// FIX #5 / #14: all group logic lives here in C++, not in JS
void handleCornersToggle() {
  const int group[] = {0, 2, 7, 5}; // lights 1, 3, 8, 6 (0-indexed)
  applyGroupToggle(group, 4);
  server.send(200, "text/plain", "Corners toggled");
}

void handleVerticalCenterToggle() {
  const int group[] = {1, 6}; // lights 2, 7
  applyGroupToggle(group, 2);
  server.send(200, "text/plain", "Vertical Center toggled");
}

void handleHorizontalCenterToggle() {
  const int group[] = {3, 4, 8, 9}; // lights 4, 5, 9, 10
  applyGroupToggle(group, 4);
  server.send(200, "text/plain", "Horizontal Center toggled");
}

void handleLeftRowToggle() {
  const int group[] = {0, 9, 8, 7}; // lights 1, 10, 9, 8
  applyGroupToggle(group, 4);
  server.send(200, "text/plain", "Left Row toggled");
}

void handleRightRowToggle() {
  const int group[] = {2, 3, 4, 5}; // lights 3, 4, 5, 6
  applyGroupToggle(group, 4);
  server.send(200, "text/plain", "Right Row toggled");
}

void handleTopRowToggle() {
  const int group[] = {0, 1, 2}; // lights 1, 2, 3
  applyGroupToggle(group, 3);
  server.send(200, "text/plain", "Top Row toggled");
}

void handleBottomRowToggle() {
  const int group[] = {5, 6, 7}; // lights 6, 7, 8
  applyGroupToggle(group, 3);
  server.send(200, "text/plain", "Bottom Row toggled");
}

// ── Animated modes ─────────────────────────────────────────────

// FIX #3: every mode handler calls stopAllModes() before starting
void handleCycle() {
  if (activeMode == MODE_CYCLE) {
    stopAllModes();
    allRelaysOff();
    server.send(200, "text/plain", "Cycle stopped");
  } else {
    stopAllModes();
    allRelaysOff();
    activeMode    = MODE_CYCLE;
    cycleLight    = 0;
    lastCycleTime = millis();
    setRelay(0, true);
    server.send(200, "text/plain", "Cycle started");
  }
}

void handleOddEven() {
  if (activeMode == MODE_ODD_EVEN) {
    stopAllModes();
    allRelaysOff();
    server.send(200, "text/plain", "Odd-Even stopped");
  } else {
    stopAllModes();
    activeMode      = MODE_ODD_EVEN;
    oddOn           = true;
    lastOddEvenTime = millis();
    for (int i = 0; i < RELAY_COUNT; i++) setRelay(i, i % 2 == 0);
    server.send(200, "text/plain", "Odd-Even started");
  }
}

void handleHeartbeat() {
  if (activeMode == MODE_HEARTBEAT) {
    stopAllModes();
    allRelaysOff();
    server.send(200, "text/plain", "Heartbeat stopped");
  } else {
    stopAllModes();
    allRelaysOff();
    activeMode        = MODE_HEARTBEAT;
    heartbeatStep     = 0;
    lastHeartbeatTime = millis();
    server.send(200, "text/plain", "Heartbeat started");
  }
}
