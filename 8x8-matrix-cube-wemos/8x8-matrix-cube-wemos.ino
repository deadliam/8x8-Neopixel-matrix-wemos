#include <Arduino.h>

#include "FastLED.h"
#include <FastLED_GFX.h>

#include <DNSServer.h>
#include "NTPClient.h"
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ESPAsyncTCP.h>

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <WiFiClient.h>
  #include <ESP8266WebServer.h>
#elif defined(ESP32)
  #include <WiFi.h>
  #include <WiFiClient.h>
  #include <WebServer.h>
#elif defined(TARGET_RP2040)
  #include <WiFi.h>
  #include <WebServer.h>
#endif

#include <ElegantOTA.h>

#if defined(ESP8266)
  ESP8266WebServer server(80);
#elif defined(ESP32)
  WebServer server(80);
#elif defined(TARGET_RP2040)
  WebServer server(80);
#endif

unsigned long ota_progress_millis = 0;

// Define NTP Client to get time
WiFiUDP ntpUDP;
const long utcOffsetInSeconds = 19800;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

String ssid = "";
String password = "";

const int ESP_BUILTIN_LED = 2;

FASTLED_USING_NAMESPACE

#if FASTLED_VERSION < 3001000
#error "Requires FastLED 3.1 or later; check github for latest code."
#endif

#define DATA_PIN    D4
#define LED_TYPE    WS2811
#define COLOR_ORDER GRB
#define NUM_LEDS    64
CRGB leds[NUM_LEDS];

#define BRIGHTNESS         30
#define FRAMES_PER_SECOND  120

#define sensor_pin D8

int laststate = LOW;
int currentstate;
int ledstate = LOW;

// Voltage meter
unsigned int raw=0;
float volt=0.0;

// -------------------------------------------------------
#define CANVAS_WIDTH    8
#define CANVAS_HEIGHT   8
#define CHIPSET         WS2811
#define NUM_LEDS        (CANVAS_WIDTH * CANVAS_HEIGHT)
GFXcanvas canvas(CANVAS_WIDTH, CANVAS_HEIGHT);
// -------------------------------------------------------

#define LONG_PRESS_TIME 2000 // holding time in (ms)

unsigned long pressStartTime = 0;
bool isLongPress = false;
bool mode = 0; // 0 - effects, 1 - letters
char currentLetter = 'A'; // start letter


void setup() {
  Serial.begin(9600);
  Serial.println();
  turnOffLeds();
  // ========================================================
  // WiFiManager
  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  // Uncomment and run it once, if you want to erase all the stored information
  // wifiManager.resetSettings();
  
  // set custom ip for portal
  //wifiManager.setAPConfig(IPAddress(10,0,1,1), IPAddress(10,0,1,1), IPAddress(255,255,255,0));
  
  // WiFi.hostname("ledcube");
  wifiManager.setHostname("ledcube");

  // fetches ssid and pass from eeprom and tries to connect
  // if it does not connect it starts an access point with the specified name
  // here  "AutoConnectAP"
  // and goes into a blocking loop awaiting configuration
  wifiManager.autoConnect("AutoConnectAP");
  // or use this for auto generated name ESP + ChipID
  //wifiManager.autoConnect();
  
  // if you get here you have connected to the WiFi
  Serial.println("Connected.");

  // Set up mDNS responder:
  // - first argument is the domain name, in this example
  //   the fully-qualified domain name is "esp8266.local"
  // - second argument is the IP address to advertise
  //   we send our IP address on the WiFi network

  if (MDNS.begin("ledcube")) {
    Serial.println("DNS started, available with: ");
    Serial.println("http://ledcube.local/");
  } else {
    Serial.println("Error setting up MDNS responder!");
    while (1) {
      delay(1000);
    }
  }

  server.onNotFound([](){ 
    server.send(404, "text/plain", "Link was not found!");  
  });
 
  server.on("/", []() {
    server.send(200, "text/plain", "Landing page!");
  });

  server.on("/voltage", handleVoltage);

  Serial.println("mDNS responder started");
  MDNS.addService("http", "tcp", 80);
  
  pinMode(ESP_BUILTIN_LED, OUTPUT);
  digitalWrite(ESP_BUILTIN_LED, 0);

  // ========================================================

  ElegantOTA.begin(&server);    // Start ElegantOTA
  // ElegantOTA callbacks
  ElegantOTA.onStart(onOTAStart);
  ElegantOTA.onProgress(onOTAProgress);
  ElegantOTA.onEnd(onOTAEnd);

  server.begin();
  Serial.println("HTTP server started");

  delay(3000); // 3 second delay for recovery
  // =============================================================
  
  // tell FastLED about the LED strip configuration
  FastLED.addLeds<LED_TYPE,DATA_PIN,COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  //FastLED.addLeds<LED_TYPE,DATA_PIN,CLK_PIN,COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  //--------------------------------------

  // set master brightness control
  // FastLED.setBrightness(BRIGHTNESS);
  // FastLED.clear(true);
  // --------------------------------
  
  // FastLED.addLeds<CHIPSET, DATA_PIN, COLOR_ORDER>(canvas.getBuffer(), NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear(true);
  canvas.setRotation(2);
 
  //-------------------------------
  pinMode(sensor_pin,INPUT);

  // Voltage meter
  pinMode(A0, INPUT);
}

// ===========================================================
// List of patterns to cycle through.  Each is defined as a separate function below.
typedef void (*SimplePatternList[])();
SimplePatternList gPatterns = { rainbow, rainbowWithGlitter, confetti, sinelon, juggle, bpm };

uint8_t gCurrentPatternNumber = 0; // Index number of which pattern is current
uint8_t gHue = 0; // rotating "base color" used by many of the patterns
//========================================================

void loop() {
  server.handleClient();
  MDNS.update();
  ElegantOTA.loop();

  raw = analogRead(A0);
  volt = raw / 1023.0 * 4.495;

  int currentstate = digitalRead(sensor_pin);

  // Определяем начало нажатия
  if (currentstate == HIGH && laststate == LOW) {
    pressStartTime = millis();
    isLongPress = false;
  }

  // Проверяем, стало ли нажатие длительным
  if (currentstate == HIGH && millis() - pressStartTime > LONG_PRESS_TIME) {
    if (!isLongPress) {
      mode = !mode; // Переключаем режим (эффекты ↔ буквы)
      Serial.println(mode ? "Showing letters" : "Showing effects");
      if (mode) {
        FastLED.addLeds<CHIPSET, DATA_PIN, COLOR_ORDER>(canvas.getBuffer(), NUM_LEDS).setCorrection(TypicalLEDStrip);
      } else {
        FastLED.addLeds<LED_TYPE,DATA_PIN,COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
      }
      isLongPress = true;
    }
  }

  // Отпускание кнопки (конец нажатия)
  if (currentstate == LOW && laststate == HIGH) {
    if (!isLongPress) { // Если это было короткое нажатие
      if (mode) { 
        // В режиме букв - переключаем букву
        currentLetter = (currentLetter == 'Z') ? 'A' : currentLetter + 1;
        Serial.print("Next letter: ");
        Serial.println(currentLetter);
      } else { 
        // В режиме эффектов - включаем/выключаем
        ledstate = !ledstate;
        Serial.println(ledstate ? "Effects ON" : "Effects OFF");
        if (!ledstate) turnOffLeds();
      }
    }
  }

  laststate = currentstate;

  if (ledstate) {
    if (!mode) {
      // В режиме эффектов обновляем анимации
      gPatterns[gCurrentPatternNumber]();
      FastLED.show();
      FastLED.delay(1000 / FRAMES_PER_SECOND);
      EVERY_N_MILLISECONDS(20) { gHue++; }
      EVERY_N_SECONDS(10) { nextPattern(); }
    } else {
      // В режиме букв показываем букву
      canvas.fillScreen(CRGB::Black);
      canvas.setTextSize(1);
      canvas.setCursor(1, 1);
      canvas.print(currentLetter);
      FastLED.show();
    }
  }
}

void turnOffLeds() {
  FastLED.clear(true);
}

#define ARRAY_SIZE(A) (sizeof(A) / sizeof((A)[0]))

void nextPattern()
{
  // add one to the current pattern number, and wrap around at the end
  gCurrentPatternNumber = (gCurrentPatternNumber + 1) % ARRAY_SIZE( gPatterns);
}

void rainbow() 
{
  // FastLED's built-in rainbow generator
  fill_rainbow( leds, NUM_LEDS, gHue, 7);
}

void rainbowWithGlitter() 
{
  // built-in FastLED rainbow, plus some random sparkly glitter
  rainbow();
  addGlitter(80);
}

void addGlitter( fract8 chanceOfGlitter) 
{
  if( random8() < chanceOfGlitter) {
    leds[ random16(NUM_LEDS) ] += CRGB::White;
  }
}

void confetti() 
{
  // random colored speckles that blink in and fade smoothly
  fadeToBlackBy( leds, NUM_LEDS, 10);
  int pos = random16(NUM_LEDS);
  leds[pos] += CHSV( gHue + random8(64), 200, 255);
}

void sinelon()
{
  // a colored dot sweeping back and forth, with fading trails
  fadeToBlackBy( leds, NUM_LEDS, 20);
  int pos = beatsin16(13,0,NUM_LEDS);
  leds[pos] += CHSV( gHue, 255, 192);
}

void bpm()
{
  // colored stripes pulsing at a defined Beats-Per-Minute (BPM)
  uint8_t BeatsPerMinute = 62;
  CRGBPalette16 palette = PartyColors_p;
  uint8_t beat = beatsin8( BeatsPerMinute, 64, 255);
  for( int i = 0; i < NUM_LEDS; i++) { //9948
    leds[i] = ColorFromPalette(palette, gHue+(i*2), beat-gHue+(i*10));
  }
}

void juggle() {
  // eight colored dots, weaving in and out of sync with each other
  fadeToBlackBy( leds, NUM_LEDS, 20);
  byte dothue = 0;
  for( int i = 0; i < 8; i++) {
    leds[beatsin16(i+7,0,NUM_LEDS)] |= CHSV(dothue, 200, 255);
    dothue += 32;
  }
}

// =================================================
void onOTAStart() {
  // Log when OTA has started
  Serial.println("OTA update started!");
  // <Add your own code here>
}

void onOTAProgress(size_t current, size_t final) {
  // Log every 1 second
  if (millis() - ota_progress_millis > 1000) {
    ota_progress_millis = millis();
    Serial.printf("OTA Progress Current: %u bytes, Final: %u bytes\n", current, final);
  }
}

void onOTAEnd(bool success) {
  // Log when OTA has finished
  if (success) {
    Serial.println("OTA update finished successfully!");
  } else {
    Serial.println("There was an error during OTA update!");
  }
  // <Add your own code here>
}

void handleVoltage() {
  String stringVoltage = String(volt, 3); // 3.141
  server.send(200, "text/html", stringVoltage);
}

// --------------------------------------------------
