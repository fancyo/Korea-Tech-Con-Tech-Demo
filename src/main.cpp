/*
  ESP32: LED controller + Multiple Alarms + Timer (rings a buzzer)
  - Configurable pins at top
  - Web UI (add/remove alarms client-side)
  - Alarms stored in Preferences (non-volatile)
  - Alarms validated and sorted
  - NTP time sync used to trigger alarms (HH:MM)
  - Timer counts down and rings buzzer when finished
  - Supports active and passive buzzers (see buzzerIsPassive)

  Author: Oday A. Rabaiah + fixes
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <vector>
#include <algorithm>

// ------------------- CONFIG -------------------
const char* ssid = "ESP32";
const char* password = "33n&00d1";

// Pins
const uint8_t LED1pin = 5;         // GPIO for LED1
const uint8_t LED2pin = 4;         // GPIO for LED2
const uint8_t buzzerPin = 12;      // GPIO for buzzer
const bool buzzerIsPassive = false; // false = active buzzer, true = passive (tone)

// Timer tone parameters (for passive buzzer)
const int buzzerToneHz = 2000;     
const int buzzerDurationMs = 1800; // total duration for timer finish (ms)

// Alarm storage
const char* prefsNamespace = "alarms"; 
const char* prefsKey = "alarm_csv";   
const uint8_t maxAlarms = 20;         

// NTP config
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;        
const int daylightOffset_sec = 0;

// ------------------- GLOBALS -------------------
WebServer server(80);
Preferences prefs;

bool LED1status = false;
bool LED2status = false;

// Timer state
bool timerRunning = false;
uint64_t timerTargetMs = 0;

// Buzzer non-blocking state
bool buzzerActive = false;
uint64_t buzzerEndMs = 0;

// Alarms in RAM
std::vector<String> alarms;

// Track last minute checked to avoid repeated alarm triggers
int lastCheckedMinute = -1;

// For passive buzzer on ESP32 we'll use ledc (if passive)
const int buzzerLedcChannel = 0;
const int buzzerLedcFreq = 2000;
const int buzzerLedcResolution = 8; // bits

// ------------------- HTML -------------------
String createHTML() {
  String str = "<!DOCTYPE html><html>";
  str += "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=no\">";
  str += "<style>";
  str += "body{font-family:Arial,sans-serif;color:#444;text-align:center;margin:0;padding:0 10px;}";
  str += ".title{font-size:28px;font-weight:bold;letter-spacing:2px;margin:40px 0 20px;}";
  str += ".led-control{display:flex;align-items:center;justify-content:center;margin:20px 0;gap:20px;}";
  str += ".led-label{font-size:20px;width:80px;text-align:left;padding-left:10px;}";
  str += ".toggle-switch{width:120px;height:60px;position:relative;}";
  str += ".slider{position:absolute;width:120px;height:60px;background-color:#f1f1f1;transition:.4s;border-radius:60px;border:1px solid #ddd;}";
  str += ".slider:before{content:'';position:absolute;height:52px;width:52px;left:4px;top:4px;background-color:white;transition:.4s;border-radius:50%;box-shadow:0 2px 5px rgba(0,0,0,.3);}";
  str += ".slider.on{background-color:#4285f4;border:none;}.slider.on:before{transform:translateX(60px);}";
  str += "a{display:block;height:100%;width:100%;text-decoration:none;color:inherit;}";
  str += ".section{margin-top:30px;padding-bottom:20px;border-bottom:1px solid #eee;}";
  str += ".section-title{font-size:20px;margin-bottom:12px;}";
  str += ".input-time{font-size:18px;padding:8px 10px;border-radius:8px;border:1px solid #ccc;margin:6px 0;}";
  str += ".btn{margin-top:10px;padding:10px 16px;font-size:16px;background-color:#4285f4;border:none;color:white;border-radius:10px;cursor:pointer;}";
  str += ".btn.red{background-color:#e53935;}";
  str += ".timer-input{width:70px;font-size:18px;padding:8px;border:1px solid #ccc;border-radius:8px;margin:6px;}";
  str += ".alarm-item{margin:6px 0;display:flex;gap:8px;align-items:center;justify-content:center;}";
  str += ".alarm-item input{font-size:18px;padding:6px;border-radius:8px;border:1px solid #ccc;}";
  str += ".small-btn{padding:6px 8px;font-size:14px;border-radius:8px;border:none;background:#777;color:white;cursor:pointer;}";
  str += "</style></head><body>";
  str += "<h1 class='title'>LED CONTROLLER</h1>";

  // LED1
  str += "<div class='led-control'><span class='led-label'>LED 1</span><div class='toggle-switch'>";
  str += LED1status ? "<a href='/led1off'><div class='slider on'></div></a>" : "<a href='/led1on'><div class='slider'></div></a>";
  str += "</div></div>";

  // LED2
  str += "<div class='led-control'><span class='led-label'>LED 2</span><div class='toggle-switch'>";
  str += LED2status ? "<a href='/led2off'><div class='slider on'></div></a>" : "<a href='/led2on'><div class='slider'></div></a>";
  str += "</div></div>";

  // Alarms section
  str += "<div class='section'><div class='section-title'>Alarms</div>";
  str += "<form id='alarmsForm' action='/setAlarms' method='GET'>";
  str += "<div id='alarmList'>";
  for (size_t i = 0; i < alarms.size(); ++i) {
    str += "<div class='alarm-item'><input type='time' name='alarm" + String(i) + "' value='" + alarms[i] + "' required>";
    str += "<button type='button' class='small-btn' onclick='removeAlarm(" + String(i) + ")'>Delete</button></div>";
  }
  if (alarms.size() == 0) {
    str += "<div class='alarm-item'><input type='time' name='alarm0' class='input-time' required></div>";
  }
  str += "</div>";
  str += "<button type='button' class='btn' onclick='addAlarm()'>Add Alarm</button> &nbsp;";
  str += "<button type='submit' class='btn'>Save Alarms</button> &nbsp;";
  str += "<button type='button' class='btn red' onclick='clearAlarms()'>Clear All</button>";
  str += "</form></div>";

  // Timer section
  str += "<div class='section'><div class='section-title'>Timer (rings buzzer)</div>";
  str += "<form action='/startTimer' method='GET'>";
  str += "<input type='number' name='hours' class='timer-input' placeholder='HH' min='0' max='23'>";
  str += "<input type='number' name='minutes' class='timer-input' placeholder='MM' min='0' max='59'>";
  str += "<input type='number' name='seconds' class='timer-input' placeholder='SS' min='0' max='59'><br>";
  str += "<button type='submit' class='btn'>Start Timer</button></form>";
  str += "<form action='/stopTimer' method='GET' style='margin-top:8px;'><button type='submit' class='btn red'>Stop Timer</button></form>";

  // Status area
  str += "<div style='margin-top:16px;font-size:16px;' id='statusArea'></div>";

  // Scripts
  str += "<script>";
  str += "function addAlarm(){var list=document.getElementById('alarmList');var idx=list.children.length;var div=document.createElement('div');div.className='alarm-item';var input=document.createElement('input');input.type='time';input.name='alarm'+idx;input.required=true;var btn=document.createElement('button');btn.type='button';btn.className='small-btn';btn.innerText='Delete';btn.onclick=function(){div.remove();renumberAlarms();};div.appendChild(input);div.appendChild(btn);list.appendChild(div);}";
  str += "function renumberAlarms(){var list=document.getElementById('alarmList');for(var i=0;i<list.children.length;i++){var inp=list.children[i].querySelector('input');if(inp) inp.name='alarm'+i;var btn=list.children[i].querySelector('button');if(btn) btn.setAttribute('onclick','removeAlarm('+i+')');}}";
  str += "function removeAlarm(i){var list=document.getElementById('alarmList');if(list.children[i]) list.children[i].remove();renumberAlarms();}";
  str += "function clearAlarms(){fetch('/clearAlarms').then(()=>location.reload());}";
  str += "function fetchStatus(){fetch('/status').then(r=>r.json()).then(j=>{var s=document.getElementById('statusArea');var txt='';txt+='Timer: '+(j.timerRunning?('running, remaining: '+j.remaining):'stopped')+'<br>';txt+='Alarms stored: '+j.alarmsCount+'<br>';txt+='LED1: '+(j.led1?'ON':'OFF')+' | LED2: '+(j.led2?'ON':'OFF')+'<br>';s.innerHTML=txt;}).catch(e=>{});} setInterval(fetchStatus,2000);fetchStatus();";
  str += "</script>";

  str += "</body></html>";
  return str;
}

// ------------------- HELPERS -------------------
void saveAlarmsToPrefs() {
  std::sort(alarms.begin(), alarms.end());
  String csv = "";
  for (size_t i = 0; i < alarms.size() && i < maxAlarms; ++i) {
    if (i) csv += ",";
    csv += alarms[i];
  }
  prefs.putString(prefsKey, csv);
}

void loadAlarmsFromPrefs() {
  alarms.clear();
  String csv = prefs.getString(prefsKey, "");
  if (csv.length() == 0) return;
  int start = 0;
  while (start < (int)csv.length()) {
    int comma = csv.indexOf(',', start);
    if (comma == -1) comma = csv.length();
    String token = csv.substring(start, comma);
    token.trim();
    if (token.length() == 5 && token.charAt(2) == ':') {
      int h = token.substring(0,2).toInt();
      int m = token.substring(3,5).toInt();
      if (h < 24 && m < 60) alarms.push_back(token);
    }
    start = comma + 1;
    if (alarms.size() >= maxAlarms) break;
  }
}

void triggerBuzzer(uint32_t durationMs) {
  buzzerActive = true;
  buzzerEndMs = millis() + durationMs;
  if (!buzzerIsPassive) digitalWrite(buzzerPin, HIGH);
  else ledcWriteTone(buzzerLedcChannel, buzzerToneHz);
}

void stopBuzzer() {
  buzzerActive = false;
  digitalWrite(buzzerPin, LOW);
  if (buzzerIsPassive) ledcWriteTone(buzzerLedcChannel, 0);
}

String getCurrentHHMM() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return String("");
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  return String(buf);
}

// ------------------- HTTP Handlers -------------------
void handleRoot() { server.send(200, "text/html", createHTML()); }
void handleStatus() {
  String remaining = "0s";
  if (timerRunning) {
    long msLeft = (long)(timerTargetMs > millis() ? (timerTargetMs - millis()) : 0);
    long s = msLeft / 1000;
    remaining = String(s / 3600) + "h " + String((s % 3600) / 60) + "m " + String(s % 60) + "s";
  }
  String json = "{";
  json += "\"timerRunning\":" + String(timerRunning ? "true" : "false") + ",";
  json += "\"remaining\":\"" + remaining + "\",";
  json += "\"alarmsCount\":" + String(alarms.size()) + ",";
  json += "\"led1\":" + String(LED1status ? "true" : "false") + ",";
  json += "\"led2\":" + String(LED2status ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleLed1On() { LED1status=true; digitalWrite(LED1pin,HIGH); server.sendHeader("Location","/"); server.send(302,"text/plain",""); }
void handleLed1Off(){ LED1status=false; digitalWrite(LED1pin,LOW); server.sendHeader("Location","/"); server.send(302,"text/plain",""); }
void handleLed2On() { LED2status=true; digitalWrite(LED2pin,HIGH); server.sendHeader("Location","/"); server.send(302,"text/plain",""); }
void handleLed2Off(){ LED2status=false; digitalWrite(LED2pin,LOW); server.sendHeader("Location","/"); server.send(302,"text/plain",""); }

void handleSetAlarms() {
  alarms.clear();
  for (uint8_t i = 0; i < server.args(); ++i) {
    String name = server.argName(i);
    String val = server.arg(i);
    if (name.startsWith("alarm")) {
      if (val.length() == 5 && val.charAt(2) == ':') {
        int h = val.substring(0,2).toInt();
        int m = val.substring(3,5).toInt();
        if (h<24 && m<60) alarms.push_back(val);
      }
    }
    if (alarms.size()>=maxAlarms) break;
  }
  saveAlarmsToPrefs();
  server.sendHeader("Location","/");
  server.send(302,"text/plain","");
}

void handleClearAlarms() {
  alarms.clear();
  prefs.remove(prefsKey);
  server.send(200,"text/plain","OK");
}

void handleStartTimer() {
  int hours=0,minutes=0,seconds=0;
  if(server.hasArg("hours")) hours=server.arg("hours").toInt();
  if(server.hasArg("minutes")) minutes=server.arg("minutes").toInt();
  if(server.hasArg("seconds")) seconds=server.arg("seconds").toInt();
  if(hours<0) hours=0; if(minutes<0) minutes=0; if(seconds<0) seconds=0;
  uint64_t totalSec=(uint64_t)hours*3600+(uint64_t)minutes*60+(uint64_t)seconds;
  if(totalSec==0){ timerRunning=false; server.sendHeader("Location","/"); server.send(302,"text/plain",""); return;}
  timerTargetMs=millis()+totalSec*1000ULL;
  timerRunning=true;
  server.sendHeader("Location","/"); server.send(302,"text/plain","");
}

void handleStopTimer(){ timerRunning=false; stopBuzzer(); server.sendHeader("Location","/"); server.send(302,"text/plain",""); }
void handleNotFound(){ server.send(404,"text/plain","Not found"); }

// ------------------- SETUP -------------------
void setup() {
  Serial.begin(115200);
  delay(100);
  pinMode(LED1pin, OUTPUT); pinMode(LED2pin, OUTPUT); pinMode(buzzerPin, OUTPUT);
  digitalWrite(LED1pin, LOW); digitalWrite(LED2pin, LOW); digitalWrite(buzzerPin, LOW);

  if(buzzerIsPassive){ ledcSetup(buzzerLedcChannel,buzzerLedcFreq,buzzerLedcResolution); ledcAttachPin(buzzerPin,buzzerLedcChannel); ledcWriteTone(buzzerLedcChannel,0); }

  Serial.printf("Starting AP: %s\n", ssid);
  WiFi.softAP(ssid,password);
  Serial.println(WiFi.softAPIP());

  // NTP
  configTime(gmtOffset_sec,daylightOffset_sec,ntpServer);

  // Preferences
  prefs.begin(prefsNamespace,false);
  loadAlarmsFromPrefs();

  // HTTP
  server.on("/",HTTP_GET,handleRoot);
  server.on("/status",HTTP_GET,handleStatus);
  server.on("/led1on",HTTP_GET,handleLed1On);
  server.on("/led1off",HTTP_GET,handleLed1Off);
  server.on("/led2on",HTTP_GET,handleLed2On);
  server.on("/led2off",HTTP_GET,handleLed2Off);
  server.on("/setAlarms",HTTP_GET,handleSetAlarms);
  server.on("/clearAlarms",HTTP_GET,handleClearAlarms);
  server.on("/startTimer",HTTP_GET,handleStartTimer);
  server.on("/stopTimer",HTTP_GET,handleStopTimer);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

// ------------------- LOOP -------------------
void loop() {
  server.handleClient();

  // Timer handling
  if(timerRunning && millis()>=timerTargetMs){
    timerRunning=false;
    triggerBuzzer(buzzerDurationMs);
  }

  // Non-blocking buzzer
  if(buzzerActive && millis()>=buzzerEndMs) stopBuzzer();

  // Alarm checking once per minute
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)){
    int curMinute = timeinfo.tm_min;
    if(curMinute!=lastCheckedMinute){
      lastCheckedMinute=curMinute;
      String nowHHMM=getCurrentHHMM();
      for(size_t i=0;i<alarms.size();++i){
        if(alarms[i]==nowHHMM){ triggerBuzzer(buzzerDurationMs); break; }
      }
    }
  }
  delay(10);
}

