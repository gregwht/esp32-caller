#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Melopero_RV3028.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

// Deep sleep settings
const unsigned long CONFIG_TIMEOUT_MS = 5 * 60UL * 1000UL;  // Go back to sleep after 5 mins of no activity in config mode
const int rtcIntPin = 5;  // RV3028 INT pin - wake-capable on the ESP32-C3 (GPIO2-5 only)
const uint64_t SAFETY_NET_SECONDS = 3600; // Back-up wake in case the RTC alarm logic ever misses

unsigned long configModeStart = 0; // Tracks how long config mode has been active

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

  // Understand why ESP32 woke up
  esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();

  // If a GPIO caused the wake, find out which one(s)
  // This lets us tell the RTC alarm apart from the button, since both wake via the same mechanism
  uint64_t wakeGpioMask = 0;
  if (wakeupReason == ESP_SLEEP_WAKEUP_GPIO){
    wakeGpioMask = esp_sleep_get_gpio_wakeup_status();
  }
  bool wokenByButton = wakeGpioMask & (1ULL << buttonPin);
  bool wokenByRtcAlarm = wakeGpioMask & (1ULL << rtcIntPin);
  bool wokenBySafetyNet = (wakeupReason == ESP_SLEEP_WAKEUP_TIMER);

  // Release the hold placed on the speaker pin before the last deep sleep, so we can drive it again.
  // Harmless to call even on a fresh power-on (nothing held yet).
  gpio_hold_dis((gpio_num_t)speakerPowerPin);

  // Prepare pins (needed on every boot)
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(rtcIntPin, INPUT_PULLUP); // RV3028's INT is open-drain, active LOW, pulled high on the module
  pinMode(speakerPowerPin, OUTPUT);
  // We don't digitalWrite here because checkSchedule(), called below, sets this pin
  // based on the schedule, rather than blindly forcing it off on every wake

  // Initialise RTC clock (needed on every boot)
  Wire.begin(0, 1);
  rtc.initI2C();
  rtc.set24HourMode();

  // Clear the RTC's alarm flag now before it has a chance to re-trigger
  // The INT pin stays LOW until this happens, which would otherwise wake the ESP32 again immediately
  rtc.clearInterruptFlags();

  // Load stored time and duration settings (needed on every boot)
  loadSettings();

  if (wokenByRtcAlarm || wokenBySafetyNet) {
    // Woken just to act on the schedule: do the minimum, then go straight back to sleep
    printTimestamp();
    Serial.println(wokenByRtcAlarm ? "Woke from RTC alarm - checking schedule" : "Woke from safety-net timer - checking schedule");
    checkSchedule();
    goToSleep();
    return;
  }
  // Otherwise if first power on, or woken by the button, enter config mode
  printTimestamp();
  Serial.println("Entering configuration mode");

  // Also apply the schedule immediately, in case we're waking into the middle of an active timeslot
  checkSchedule();

  // Start LittleFS so web files can be read
  if (!LittleFS.begin()) {
    Serial.println("LittleFS failed to mount");
    return;
  }

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

  // Start (or restart) the inactivity timer for config mode
  configModeStart = millis();
}

void loop() {

  server.handleClient();

  checkSchedule();

  // Button logic - pressing it while awake resets the inactivity timer.
  // This means the device can be kepy awake by pressing the button occasionally
  if (digitalRead(buttonPin) == LOW) {
    Serial.println("Button pressed -- staying awake");
    configModeStart = millis();

    // Wait until button is released
    while (digitalRead(buttonPin) == LOW) {
      delay(50);
    }
  }

  // Go back to sleep once nothing has happened for CONFIG_TIMEOUT_MS
  if (millis() - configModeStart > CONFIG_TIMEOUT_MS) {
    Serial.println("Config mode timed out - going back to sleep");
    goToSleep();
  }

  delay(10);
}

// Finds how many minutes from now until the next timeslot start or end,
// across all active slots (for RTC alarm)
uint16_t minutesUntilNextTransition(uint16_t nowMinutes) {

  uint16_t best = 1440; // default to a full day away

  for (uint8_t i = 0; i < numSlots; i++) {
    uint16_t start = timeStringToMinutes(timeslots[i].startTime);
    uint16_t end = (start + timeslots[i].duration) % 1440;

    uint16_t deltaStart = (start + 1440 - nowMinutes) % 1440;
    if (deltaStart == 0) deltaStart = 1440; // already at this boundary, next one is a day away
    if (deltaStart < best) best = deltaStart;

    if (timeslots[i].duration > 0 && timeslots[i].duration < 1440) {
      uint16_t deltaEnd = (end + 1440 - nowMinutes) % 1440;
      if (deltaEnd == 0) deltaEnd = 1440;
      if (deltaEnd < best) best = deltaEnd;
    }
  }

  return best;
}
  
void goToSleep() {

  printTimestamp();
  Serial.println("Going to deep sleep...goodnight");

  // Make sure the speaker is left in the correct state before sleeping
  digitalWrite(speakerPowerPin, speakerOn ? HIGH : LOW);

  // Hold that output level through deep sleep
  // Without this the pin's state isn't guaranteed to survive sleep, which could cut the speaker off mid-timeslot
  gpio_hold_en((gpio_num_t)speakerPowerPin);
  gpio_deep_sleep_hold_en();

  // Work out exactly when the next timeslot starts or ends,
  // and ask the RTC to raise its INT pin at that moment
  CurrentTime now = getCurrentTime();
  uint16_t minutesAhead = minutesUntilNextTransition(now.minutesSinceMidnight);
  uint16_t nextEventMinutes = (now.minutesSinceMidnight + minutesAhead) % 1440;
  uint8_t alarmHour = nextEventMinutes / 60;
  uint8_t alarmMinute = nextEventMinutes % 60;

  Serial.print("Arming RTC alarm for ");
  if (alarmHour < 10) Serial.print("0");
  Serial.print(alarmHour);
  Serial.print(":");
  if (alarmMinute < 10) Serial.print("0");
  Serial.println(alarmMinute);
  Serial.flush();

  // Match on hour + minute every day, ignoring weekday/date entirely (dateAlarm=false)
  // and generate an interrupt on the INT pin when it fires
  rtc.setDateModeForAlarm(false); // weekday mode -- irrelevant since dateAlarm is disabled below
  rtc.enableAlarm(0, alarmHour, alarmMinute, false, true, true, true);

  // Wake source 1: the RTC alarm (primary) and the button (for config mode)
  // Both active LOW
  uint64_t wakePinMask = (1ULL << buttonPin) | (1ULL << rtcIntPin);
  esp_deep_sleep_enable_gpio_wakeup(wakePinMask, ESP_GPIO_WAKEUP_GPIO_LOW);

  // Wake source 2: a long-interval safety net in case the alarm is every misconfigured
  // (Should rarely activate, if ever)
  esp_sleep_enable_timer_wakeup(SAFETY_NET_SECONDS * 1000000ULL);

  esp_deep_sleep_start();
  // Execution never returns here -- the next line of code to run is setup(), after waking
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