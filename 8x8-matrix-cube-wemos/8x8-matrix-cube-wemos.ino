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

#define ARRAY_SIZE(A) (sizeof(A) / sizeof((A)[0]))

int laststate = LOW;
int currentstate;
int ledstate = LOW;

// Sound detection
unsigned int soundValue = 0;
unsigned int soundIntensity = 0;
unsigned int lastSoundIntensity = 0;
const int SOUND_THRESHOLD = 10;
const float SMOOTHING_FACTOR = 0.2; // Smoothing factor for sound intensity (0.0-1.0)
unsigned int soundPeak = 0;
const unsigned long PEAK_DECAY = 50; // Peak decay rate in milliseconds
unsigned long lastPeakTime = 0;
// -------------------------------------------------------
#define CANVAS_WIDTH    8
#define CANVAS_HEIGHT   8
#define CHIPSET         WS2811
GFXcanvas canvas(CANVAS_WIDTH, CANVAS_HEIGHT);

// Define an array of colors
CRGB colors[] = {CRGB::Red, CRGB::Green, CRGB::Blue, CRGB::Yellow, CRGB::Purple, CRGB::Cyan, CRGB::Orange};
int colorIndex = 0; // Index for cycling through colors

unsigned long lastScrollTime = 0;
int scrollPos = CANVAS_WIDTH;
// -------------------------------------------------------

#define LONG_PRESS_TIME 2000 // holding time in (ms)

unsigned long pressStartTime = 0;
bool isLongPress = false;
bool mode = 0; // 0 - effects, 1 - letters
char currentLetter = 'A'; // start with letter A, will cycle through A-Z then 0-9


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

  // Sound detection sensor
  pinMode(A0, INPUT);
}

// ===========================================================
// List of patterns to cycle through.  Each is defined as a separate function below.
typedef void (*SimplePatternList[])();
SimplePatternList gPatterns = { 
  rainbow, rainbowWithGlitter, confetti, sinelon, 
  juggle, bpm, fire, cylon, meteor, twinkle, 
  colorWaves, pulse, randomFlicker, sparkle,
  matrixRain, ripple, expandingSquares, vuMeter 
};


uint8_t gCurrentPatternNumber = 0; // Index number of which pattern is current
uint8_t gHue = 0; // rotating "base color" used by many of the patterns
//========================================================

void loop() {
  server.handleClient();
  MDNS.update();
  ElegantOTA.loop();

  // Read and process sound input with improved peak detection
  soundValue = analogRead(A0);
  int rawIntensity = map(soundValue, 0, 1023, 0, 255);
  
  // Update peak value
  if (rawIntensity > soundPeak) {
      soundPeak = rawIntensity;
      lastPeakTime = millis();
  } else if (millis() - lastPeakTime > PEAK_DECAY) {
      if (soundPeak > 0) soundPeak--;
  }
  
  // Combine current value with peak for more dynamic response
  rawIntensity = max((unsigned int)rawIntensity, (soundPeak * 3) / 4);
  
  // Apply smoothing to prevent erratic pattern changes
  soundIntensity = (SMOOTHING_FACTOR * rawIntensity) + ((1.0 - SMOOTHING_FACTOR) * lastSoundIntensity);
  lastSoundIntensity = soundIntensity;
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
        if (currentLetter == 'Z') {
          currentLetter = '0';
        } else if (currentLetter == '9') {
          currentLetter = 'A';
        } else {
          currentLetter = currentLetter + 1;
        }
        
        // Cycle to the next color for each new letter
        colorIndex = (colorIndex + 1) % (sizeof(colors) / sizeof(colors[0]));
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
      // Select pattern based on sound intensity
      if (soundIntensity > SOUND_THRESHOLD) {
        // For louder sounds, choose more active patterns
        if (soundIntensity > 200) {
            gCurrentPatternNumber = ARRAY_SIZE(gPatterns) - 1; // vuMeter
        } else if (soundIntensity > 150) {
            gCurrentPatternNumber = 2; // confetti
        } else if (soundIntensity > 100) {
            gCurrentPatternNumber = 3; // sinelon
        } else {
            gCurrentPatternNumber = 9; // twinkle
        }
        
        // Adjust brightness based on sound intensity and peak
        int brightness = map(max(soundIntensity, soundPeak), 0, 255, 30, 255);
        FastLED.setBrightness(brightness);
      }
      
      gPatterns[gCurrentPatternNumber]();
      FastLED.show();
      FastLED.delay(1000 / FRAMES_PER_SECOND);
      EVERY_N_MILLISECONDS(20) { gHue++; }
      
      // Only auto-change patterns when there's no sound
      if (soundIntensity <= SOUND_THRESHOLD) {
        EVERY_N_SECONDS(10) { nextPattern(); }
      }
    } else {
      // В режиме букв показываем букву
      canvas.fillScreen(CRGB::Black);
      canvas.setTextSize(1);
      canvas.setCursor(1, 1);
      canvas.setTextColor(colors[colorIndex]);
      canvas.print(currentLetter);
      FastLED.show();
    }
  }
}


void turnOffLeds() {
  FastLED.clear(true);
}

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

#define COOLING  60     // How quickly the fire cools down (higher = less flickering)
#define SPARKING 100    // Chance of a new spark appearing at the bottom (higher = more flickering)
#define NUM_SPARKS 3    // Number of random sparks at the bottom

void fire() {
  static byte heat[NUM_LEDS];

  // Step 1: Cool down the heat of every cell slightly
  for (int i = 0; i < NUM_LEDS; i++) {
    heat[i] = qsub8(heat[i], random8(0, ((COOLING * 10) / NUM_LEDS) + 2));
  }

  // Step 2: Spread heat upward, making it look like flames moving up
  for (int i = NUM_LEDS - 1; i >= 2; i--) {
    heat[i] = (heat[i - 1] + heat[i - 2] + heat[i - 3]) / 3;
  }

  // Step 3: Randomly ignite new sparks at the bottom
  for (int i = 0; i < NUM_SPARKS; i++) {
    if (random8() < SPARKING) {
      int pos = random8(3);  // Randomly pick one of the first few LEDs
      heat[pos] = qadd8(heat[pos], random8(160, 255)); 
    }
  }

  // Step 4: Map heat values to flame colors
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = HeatColor(heat[i]); 
  }

  FastLED.show();
  delay(30);  // Control animation speed
}

void cylon() {
  // A bouncing light similar to the Cylon eye or KITT from Knight Rider
  static int pos = 0;
  static int direction = 1;
  fadeToBlackBy(leds, NUM_LEDS, 20);
  leds[pos] = CHSV(gHue, 255, 255);
  pos += direction;
  if (pos == NUM_LEDS - 1 || pos == 0) direction = -direction;
}

void meteor() {
  // A single bright pixel leaving a fading trail
  fadeToBlackBy(leds, NUM_LEDS, 30);
  int pos = beatsin16(15, 0, NUM_LEDS - 1);
  leds[pos] += CHSV(gHue, 255, 255);
}

void twinkle() {
  // Random twinkling stars effect
  fadeToBlackBy(leds, NUM_LEDS, 10);
  if (random8() < 50) {
    int pos = random16(NUM_LEDS);
    leds[pos] += CHSV(random8(), 200, 255);
  }
}

void colorWaves() {
  // Smooth color wave effect
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CHSV(gHue + sin8(i * 10), 255, 255);
  }
}

void pulse() {
  static uint8_t pulseValue = 0;
  static bool increasing = true;

  if (increasing) {
    pulseValue++;
    if (pulseValue == 255) {
      increasing = false;
    }
  } else {
    pulseValue--;
    if (pulseValue == 0) {
      increasing = true;
    }
  }

  fill_solid(leds, NUM_LEDS, CHSV(gHue, 255, pulseValue));
}

void randomFlicker() {
  for (int i = 0; i < NUM_LEDS; i++) {
    if (random8() < 30) {
      leds[i] = CHSV(random8(), 255, random8(128, 255));
    } else {
      leds[i] = CRGB::Black;
    }
  }
  FastLED.show();
  delay(50);
}

void sparkle() {
  fadeToBlackBy(leds, NUM_LEDS, 10);
  int pos = random16(NUM_LEDS);
  leds[pos] = CHSV(random8(), 255, 255);  // Sparkle with random color
  FastLED.show();
  delay(30);
}

uint16_t XY(uint8_t x, uint8_t y) {
  return (y * 8) + x;
}

void matrixRain() {
  // Matrix-style falling characters
  fadeToBlackBy(leds, NUM_LEDS, 40);
  
  static byte drops[8] = {0,0,0,0,0,0,0,0};
  static byte dropBrightness[8] = {0,0,0,0,0,0,0,0};
  
  // Move drops down and add new ones
  for(int i = 0; i < 8; i++) {
    if(drops[i] == 0 && random8() < 40) {
      drops[i] = 1;
      dropBrightness[i] = 255;
    }
    
    if(drops[i] > 0 && drops[i] <= 8) {
      leds[XY(i, drops[i]-1)] = CHSV(96, 255, dropBrightness[i]);
      dropBrightness[i] = qadd8(dropBrightness[i], 10);
      if(random8() < 30) {
        drops[i]++;
        if(drops[i] > 8) drops[i] = 0;
      }
    }
  }
}

void ripple() {
  static int center_x = 3;
  static int center_y = 3;
  static int wave = 0;
  
  fadeToBlackBy(leds, NUM_LEDS, 64);
  
  for(int x = 0; x < 8; x++) {
    for(int y = 0; y < 8; y++) {
      int distance = sqrt(pow(x - center_x, 2) + pow(y - center_y, 2));
      if(distance == wave / 32) {
        leds[XY(x, y)] = CHSV(gHue, 255, 255);
      }
    }
  }
  
  wave += 8;
  if(wave >= 256) {
    wave = 0;
    center_x = random8(1, 7);
    center_y = random8(1, 7);
  }
}

void expandingSquares() {
  static int size = 0;
  static int color = 0;
  
  fadeToBlackBy(leds, NUM_LEDS, 64);
  
  for(int x = 3-size; x <= 4+size; x++) {
    if(x >= 0 && x < 8) {
      if(3-size >= 0) leds[XY(x, 3-size)] = CHSV(color, 255, 255);
      if(4+size < 8) leds[XY(x, 4+size)] = CHSV(color, 255, 255);
    }
  }
  
  for(int y = 3-size; y <= 4+size; y++) {
    if(y >= 0 && y < 8) {
      if(3-size >= 0) leds[XY(3-size, y)] = CHSV(color, 255, 255);
      if(4+size < 8) leds[XY(4+size, y)] = CHSV(color, 255, 255);
    }
  }
  
  EVERY_N_MILLISECONDS(100) {
    size++;
    if(size >= 4) {
      size = 0;
      color = random8();
    }
  }
}

void vuMeter() {
  // Create a VU meter effect based on sound intensity
  fadeToBlackBy(leds, NUM_LEDS, 60);
  
  // Calculate how many LEDs should be lit based on sound intensity
  int ledsToLight = map(soundIntensity, 0, 255, 0, NUM_LEDS);
  
  // Create gradient effect
  for(int i = 0; i < ledsToLight; i++) {
    if(i < NUM_LEDS/3) {
      // Green for lower levels
      leds[i] = CRGB::Green;
    } else if(i < (NUM_LEDS * 2/3)) {
      // Yellow for medium levels
      leds[i] = CRGB::Yellow;
    } else {
      // Red for high levels
      leds[i] = CRGB::Red;
    }
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


// --------------------------------------------------
