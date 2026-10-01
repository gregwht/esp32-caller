#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Melopero_RV3028.h>

// Global objects
WebServer server(80);  // Web Server
Melopero_RV3028 rtc;   // Real Time Clock
Preferences prefs;     // Set up Preferences (for storing information in flash storage to survive power cycles)

// Global Settings

// A single on/off window for the speaker
struct Timeslot {
  String startTime;    // e.g. "09:00"
  uint16_t duration;   // in minutes 
};

const uint8_t MAX_SLOTS = 10;  // Arbitrarily set to 10, increase if more timeslots are needed
Timeslot timeslots[MAX_SLOTS];
uint8_t numSlots = 1;          // How many of the timeslots are actually in use (derived from timeslot rows in web UI )

bool speakerOn = false;

// Set up WiFi
const char* ssid = "Caller01";
const char* password = "password";

// Set up pins
const int buttonPin = 3;
const int speakerPowerPin = 10;

// Create struct for the current time
struct CurrentTime {
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint16_t minutesSinceMidnight;
  String formatted;
};

void setup() {

  // Start Serial
  Serial.begin(115200);
  delay(1000);

  // Initialise RTC clock
  Wire.begin(0, 1);
  rtc.initI2C();
  rtc.set24HourMode();

  // Start LittleFS so web files can be read
  if (!LittleFS.begin()) {
    Serial.println("LittleFS failed to mount");
    return;
  }

  // Load stored time and duration settings
  loadSettings();

  // Configure WiFi, server, load web files
  prepareWeb();

  // Tell server what to do when Save button is pressed
  enableSaving();

  // Get latest time and duration settings
  enableSettingsEndpoint();
  // Create endpoint for RTC time
  enableStatusEndpoint();
  enableRTCSettings();

  // Start web server
  server.begin();
  printTimestamp();
  Serial.println("Web server started.");
  Serial.println();

  // Prepare pins
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(speakerPowerPin, OUTPUT);
  digitalWrite(speakerPowerPin, LOW); // LOW prevents speaker powering on when ESP32 starts
}

void loop() {

  server.handleClient();

  checkSchedule();

  // Button logic
  if (digitalRead(buttonPin) == LOW) {
    Serial.println("Button pressed!");
    startConfigurationMode();

    // Wait until button is released
    while (digitalRead(buttonPin) == LOW) {
      delay(50);
    }
  }

  delay(10);
}

void startConfigurationMode() {
  Serial.println("Configuration Mode activated.");
}

void loadSettings() {

  // Open the flash storage for saving time and duration settings, in read/write mode
  prefs.begin("settings", false);

  // Get how many slots were saved last time (defaults to 1)
  numSlots = prefs.getUChar("numSlots", 1);
  if (numSlots < 1) numSlots = 1;
  if (numSlots > MAX_SLOTS) numSlots = MAX_SLOTS;

  Serial.println();
  printTimestamp();
  Serial.println("Current Settings:");

  for (uint8_t i = 0; i < numSlots; i++) {
    String timeKey = "time" + String(i);
    String durKey = "duration" + String(i);

    // Set default timeslot
    String defaultTime = (i == 0) ? "09:00" : "00:00";
    uint16_t defaultDuration = (i == 0) ? 60 : 0;

    timeslots[i].startTime = prefs.getString(timeKey.c_str(), defaultTime);
    timeslots[i].duration = prefs.getUShort(durKey.c_str(), defaultDuration);
  
    Serial.print("Slot ");
    Serial.print(i);
    Serial.print(": start=");
    Serial.print(timeslots[i].startTime);
    Serial.print(" duration=");
    Serial.print(timeslots[i].duration);
    Serial.println(" min");
  }
  Serial.println();
}

void prepareWeb() {

  // Start WiFi access point
  printTimestamp();
  Serial.println("Starting WiFi...");
  WiFi.softAP(ssid, password);
  Serial.println("WiFi started");
  Serial.print("IP address: ");
  Serial.println(WiFi.softAPIP());

  // Define what happens when someone visits /
  server.serveStatic("/", LittleFS, "/index.html");
  server.serveStatic("/style.css", LittleFS, "/style.css");
  server.serveStatic("/script.js", LittleFS, "/script.js");

  // Check website files have loaded:
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.print(file.name());
    Serial.println(" loaded successfully.");
    file = root.openNextFile();
  }
  Serial.println();
}

void enableSaving() {
  // Tell server what to do when Save button is pressed

  server.on("/save", []() {
    // The web page sends how many slots it's submitting, plus a time and duration for each
    uint8_t submittedSlots = server.arg("numSlots").toInt();

    if (submittedSlots < 1) submittedSlots = 1;
    if (submittedSlots > MAX_SLOTS) submittedSlots = MAX_SLOTS;

    numSlots = submittedSlots;

    for (uint8_t i = 0; i < numSlots; i++) {

      String timeKey = "time" + String(i);
      String durKey = "duration" + String(i);

      timeslots[i].startTime = server.arg(timeKey);
      timeslots[i].duration = server.arg(durKey).toInt();

      // Ensure a submitted duration can't be over 24 hours
      if (timeslots[i].duration > 1440){
        timeslots[i].duration = 1440;
      }

      // Save this slot in flash storage
      prefs.putString(timeKey.c_str(), timeslots[i].startTime);
      prefs.putUShort(durKey.c_str(), timeslots[i].duration);

    }

    // Save how many slots are in use
    prefs.putUChar("numSlots", numSlots);

   
    // Print arguments received and their values
    Serial.println("Settings received:");
    for (int i = 0; i < server.args(); i++) {
      Serial.print(server.argName(i));
      Serial.print(" = ");
      Serial.println(server.arg(i));
    }
    Serial.println();

    Serial.println("Saved! Checking flash...");
    for (uint8_t i=0; i < numSlots; i++) {
      String timeKey = "time" + String(i);
      String durKey = "duration" + String(i);
      Serial.print("Stored slot ");
      Serial.print(i);
      Serial.print(": ");
      Serial.print(prefs.getString(timeKey.c_str(), "missing"));
      Serial.print(" / ");
      Serial.println(prefs.getUShort(durKey.c_str(), 0));
    }
    Serial.println();
    server.send(200, "text/plain", "Saved");
  });
}

void enableSettingsEndpoint() {
  // When visiting https://192.168.4.1/settings, load the latest time and duration settings

  server.on("/settings", []() {
    
    StaticJsonDocument<1024> doc;

    doc["numSlots"] = numSlots;
    JsonArray slots = doc.createNestedArray("timeslots");

    for (uint8_t i = 0; i < numSlots; i++) {
      JsonObject slot = slots.createNestedObject();
      slot["time"] = timeslots[i].startTime;
      slot["duration"] = timeslots[i].duration;
    }

    String response;

    serializeJson(doc, response);

    server.send(200, "application/json", response);
  });
}

void enableStatusEndpoint() {

  server.on("/status", []() {
    CurrentTime now = getCurrentTime();

    StaticJsonDocument<200> doc;

    doc["time"] = now.formatted;
    doc["minutes"] = now.minutesSinceMidnight;

    String response;

    serializeJson(doc, response);

    server.send(200, "application/json", response);
  });
}

void enableRTCSettings() {

  server.on("/setRTC", []() {
    int year = server.arg("year").toInt();
    int month = server.arg("month").toInt();
    int weekday = server.arg("weekday").toInt();
    int day = server.arg("day").toInt();
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    int second = server.arg("second").toInt();

    rtc.setTime(year, month, weekday, day, hour, minute, second);
    CurrentTime now = getCurrentTime();

    Serial.print("RTC updated: ");
    Serial.println(now.formatted);

    server.send(200, "text/plain", "RTC update");
  });
}

CurrentTime getCurrentTime() {
  // Get the current time from the RTC

  CurrentTime t;

  t.hour = rtc.getHour();
  t.minute = rtc.getMinute();
  t.second = rtc.getSecond();

  t.minutesSinceMidnight = t.hour * 60 + t.minute;

  t.formatted = "";

  if (t.hour < 10) t.formatted += "0";
  t.formatted += String(t.hour);
  t.formatted += ":";

  if (t.minute < 10) t.formatted += "0";
  t.formatted += String(t.minute);
  t.formatted += ":";

  if (t.second < 10) t.formatted += "0";
  t.formatted += String(t.second);

  return t;
}

void printTimestamp() {
  // Format a timestamp for debugging purposes

  CurrentTime now = getCurrentTime();
  Serial.print("==========(");
  Serial.print(now.formatted);
  Serial.println(")==========");
}

uint16_t timeStringToMinutes(const String& time) {
// Used to convert start times from strings (e.g. "09:00")
// into minutes since midnight as an interger (e.g. 540)

  int colon = time.indexOf(':');

  uint8_t hours = time.substring(0, colon).toInt();
  uint8_t minutes = time.substring(colon + 1).toInt();

  return (hours * 60) + minutes;
}

bool isWithinSlot(const Timeslot& slot, uint16_t nowMinutes){

  uint16_t start = timeStringToMinutes(slot.startTime);
  uint16_t end = start + slot.duration;

  // For a slot running 24 hours a day
  if (slot.duration >= 1440) {
    return true;
  }
  // For a slot which doesn't cross midnight
  else if (end <= 1440) {
    return nowMinutes >= start && nowMinutes < end;
  }
  // For a slot which crosses midnight
  else {
    uint16_t endNextDay = end - 1440;
    return nowMinutes >= start || nowMinutes < endNextDay;
  }
}


void checkSchedule() {
  // Logic which determines if the speaker should be playing or silent
  // The speaker should be on if the current time falls inside any of the active slots

  CurrentTime now = getCurrentTime();

  bool shouldBeOn = false;

  for (uint8_t i = 0; i < numSlots; i++) {
    if (isWithinSlot(timeslots[i], now.minutesSinceMidnight)) {
      shouldBeOn = true;
      break;  // Once we find an active slot, we don't need to check for any more
    }
  }

  // Only react when the state changes
  if (shouldBeOn && !speakerOn) {

    speakerOn = true;
    printTimestamp();
    Serial.println("Speaker should be ON");
    setSpeaker(true);

  }
  else if (!shouldBeOn && speakerOn) {

    speakerOn = false;
    printTimestamp();
    Serial.println("Speaker should be OFF");
    setSpeaker(false);
  }
}

void setSpeaker(bool state) {

  digitalWrite(speakerPowerPin, state ? HIGH : LOW);

  if (state) {
    Serial.println("Turning speaker power GPIO ON");
    digitalWrite(speakerPowerPin, HIGH);
  }
  else {
    Serial.println("Turning speaker power GPIO OFF");
    digitalWrite(speakerPowerPin, LOW);
  }
}