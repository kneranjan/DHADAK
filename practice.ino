#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "secrets.h"          // Your AWS Certificates & WiFi credentials
#include <ESP8266WiFi.h>      
#include <PubSubClient.h>     
#include <WiFiClientSecure.h> 
#include <ArduinoJson.h>      
#include <TimeLib.h>          

// ============================================
// AWS IOT TOPICS & CONFIG 
// ============================================
#define AWS_IOT_PUBLISH_TOPIC   "ppg_device_v1/pub" 
#define AWS_IOT_SUBSCRIBE_TOPIC "ppg_device_v1/sub" 

WiFiClientSecure net; 
PubSubClient client(net); 

// Global Pointers for AWS Certs (Prevents Memory Crashes)
BearSSL::X509List *caCert;
BearSSL::X509List *clientCert;
BearSSL::PrivateKey *clientKey;

// ============================================
// SENSOR & OLED CONFIGURATION
// ============================================
#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 

#define SAMPLE_RATE 50            // 50 Hz output
#define DURATION_SECONDS 10       // 10 seconds of data collection
#define BUFFER_SIZE 500           // 50Hz * 10s = 500 samples
#define COOLDOWN_SECONDS 10       // Wait time before next scan

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1); 
MAX30105 particleSensor; 

// ============================================
// STATE MACHINE & BUFFERS
// ============================================
enum State { WAITING, COLLECTING, PROCESSING, DONE }; 
State currentState = WAITING; 

// Raw arrays for AWS Cloud Payload
long irRaw[BUFFER_SIZE]; 
long redRaw[BUFFER_SIZE];

// Buffers for Local Edge Processing
float filteredBuffer[BUFFER_SIZE];    // IR Filtered
float filteredRedBuffer[BUFFER_SIZE]; // RED Filtered
int bufferIndex = 0; 

unsigned long resultDisplayTime = 0; 
int finalHR = 0; 
int finalSpO2 = 0; 

// ============================================
// AWS MESSAGE HANDLER
// ============================================
void messageHandler(char* topic, byte* payload, unsigned int length) { 
  Serial.print("Incoming: "); 
  Serial.println(topic); 
  StaticJsonDocument<200> doc; 
  deserializeJson(doc, payload); 
  const char* message = doc["message"]; 
  Serial.println(message); 
}

// ============================================
// AWS RECONNECT FUNCTION
// ============================================
void connectAWS() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    return;
  }
  
  if (!client.connected()) {
    Serial.print("Connecting to AWS IoT...");
    if (client.connect(THINGNAME)) {
      Serial.println(" Connected!");
      client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC);
    } else {
      Serial.print(" Failed! State: ");
      Serial.println(client.state());
    }
  }
}

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200); 
  Wire.begin(D2, D1); 

  // 1. Initialize OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println("OLED failed"); 
    while (1); 
  }
  
  Wire.setClock(400000); // OVERDRIVE I2C SPEED to prevent dropped samples

  display.clearDisplay(); 
  display.setTextColor(SSD1306_WHITE); 
  display.setTextSize(1);
  display.setCursor(0, 45); 
  display.println("Connecting WiFi..."); 
  display.display(); 

  // 2. Initialize WiFi
  WiFi.mode(WIFI_STA); 
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD); 
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print("."); 
  }

  // 3. NTP Time Sync (Required for AWS TLS)
  display.clearDisplay();
  display.println("Syncing Time...");
  display.display();

  configTime(5*3600 + 30*60, 0, "pool.ntp.org", "time.nist.gov"); 
  time_t now = time(nullptr); 
  int ntp_retries = 0; 
  while (now < 1700000000 && ntp_retries < 30) { 
      delay(1000); 
      now = time(nullptr); 
      ntp_retries++; 
  }

  // NTP Safety Net
  if (now < 1700000000) {
      display.clearDisplay();
      display.setCursor(0,45);
      display.print("NTP FAILED - HALTING");
      display.display();
      while(1); 
  }

  // 4. AWS Secure Setup (Heap Allocated)
  caCert = new BearSSL::X509List(AWS_CERT_CA);
  clientCert = new BearSSL::X509List(AWS_CERT_CRT);
  clientKey = new BearSSL::PrivateKey(AWS_CERT_PRIVATE);

  net.setTrustAnchors(caCert);
  net.setClientRSACert(clientCert, clientKey);
  net.setBufferSizes(1024, 512); 

  client.setServer(AWS_IOT_ENDPOINT, 8883); 
  client.setCallback(messageHandler); 
  
  // INCREASE MQTT BUFFER to hold the large JSON payload safely
  client.setBufferSize(10240); 

  // 5. Initialize Sensor
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) { 
    display.clearDisplay(); 
    display.setCursor(0, 45); 
    display.println("MAX30105 Error!"); 
    display.display(); 
    while (1); 
  }

  byte ledBrightness = 0x1F; 
  byte sampleAverage = 4; 
  byte ledMode       = 2;  
  int  sampleRate    = 200; 
  int  pulseWidth    = 411; 
  int  adcRange      = 4096; 

  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange); 
  particleSensor.setPulseAmplitudeRed(ledBrightness); // SpO2 needs Red ON
  particleSensor.setPulseAmplitudeGreen(0); 

  display.clearDisplay(); 
}

// ============================================
// AWS PUBLISH FUNCTION
// ============================================
void publishBatch() { 
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 45);
  display.print("Sending to AWS...");
  display.display();

  DynamicJsonDocument doc(10240); // INCREASED BUFFER SIZE

  JsonArray red_array = doc.createNestedArray("ppg_red"); 
  JsonArray ir_array  = doc.createNestedArray("ppg_ir"); 

  for (int i = 0; i < BUFFER_SIZE; i++) { 
    red_array.add(redRaw[i]); 
    ir_array.add(irRaw[i]); 
  }

  doc["device_id"]    = THINGNAME; 
  doc["sampling_rate"] = SAMPLE_RATE; 

  String payload; 
  serializeJson(doc, payload); 

  if (client.publish(AWS_IOT_PUBLISH_TOPIC, payload.c_str())) { 
    Serial.println("Batch published successfully."); 
  } else { 
    Serial.println("Publish FAILED."); 
  }
}

// ============================================
// MAIN LOOP
// ============================================
void loop() {
  // 1. Safe MQTT Keep-Alive
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) connectAWS();
    client.loop(); 
  }

  // ---------------------------------------------------------
  // STATE 1: WAITING FOR FINGER
  // ---------------------------------------------------------
  if (currentState == WAITING) { 
    long irValue = particleSensor.getIR(); 
    
    display.clearDisplay(); 
    display.setTextSize(1); 
    display.setCursor(0, 40); 
    
    if (irValue < 50000) { 
      display.println("Place finger..."); 
      display.setCursor(0, 50);
      display.print("AWS: ");
      display.print(client.connected() ? "Connected" : "Offline");
    } else {
      display.println("Finger detected!"); 
      display.println("Hold still..."); 
      delay(1500); 
      
      // Clear FIFO before starting to prevent stale data
      particleSensor.clearFIFO(); 
      bufferIndex = 0; 
      currentState = COLLECTING; 
    }
    display.display(); 
  }

  // ---------------------------------------------------------
  // STATE 2: COLLECTING DATA (FIFO SAFE)
  // ---------------------------------------------------------
  else if (currentState == COLLECTING) { 
    
    // Pull new samples from the sensor's hardware buffer
    particleSensor.check(); 

    while (particleSensor.available()) {
      long irValue = particleSensor.getFIFOIR();
      long redValue = particleSensor.getFIFORed();
      particleSensor.nextSample(); 

      if (irValue < 50000) { 
        currentState = WAITING; // Finger removed
        return; 
      }

      irRaw[bufferIndex] = irValue;
      redRaw[bufferIndex] = redValue;
      bufferIndex++;

      // Update OLED progress once per second
      if (bufferIndex % 50 == 0) { 
        int secondsLeft = DURATION_SECONDS - (bufferIndex / SAMPLE_RATE); 
        display.clearDisplay(); 
        display.setTextSize(2); 
        display.setCursor(0, 10); 
        display.print("Reading..."); 
        display.setTextSize(3); 
        display.setCursor(40, 35); 
        display.print(secondsLeft); 
        display.display(); 
      }

      if (bufferIndex >= BUFFER_SIZE) { 
        currentState = PROCESSING; 
        break; 
      }
    }
  }

  // ---------------------------------------------------------
  // STATE 3: LOCAL PROCESSING & AWS PUBLISH
  // ---------------------------------------------------------
  else if (currentState == PROCESSING) { 
    display.clearDisplay(); 
    display.setTextSize(2); 
    display.setCursor(0, 25); 
    display.print("Filtering..."); 
    display.display(); 

    // --- 1. DC Removal ---
    float meanIR = 0, meanRed = 0; 
    for (int i = 0; i < BUFFER_SIZE; i++) {
      meanIR += irRaw[i]; 
      meanRed += redRaw[i];
    }
    meanIR /= BUFFER_SIZE; 
    meanRed /= BUFFER_SIZE;
    
    for (int i = 0; i < BUFFER_SIZE; i++) {
        filteredBuffer[i] = -(irRaw[i] - meanIR);
        filteredRedBuffer[i] = -(redRaw[i] - meanRed);
    }

    // --- 2. High-Pass Filter (Corrected Math) ---
    float alpha_hp = 0.94; 
    float x_prev_ir = filteredBuffer[0];
    float x_prev_red = filteredRedBuffer[0];

    for (int i = 1; i < BUFFER_SIZE; i++) { 
      // IR
      float x_curr_ir = filteredBuffer[i];
      filteredBuffer[i] = alpha_hp * (filteredBuffer[i - 1] + x_curr_ir - x_prev_ir); 
      x_prev_ir = x_curr_ir;

      // RED
      float x_curr_red = filteredRedBuffer[i];
      filteredRedBuffer[i] = alpha_hp * (filteredRedBuffer[i - 1] + x_curr_red - x_prev_red); 
      x_prev_red = x_curr_red;
    }

    // --- 3. Low-Pass Filter ---
    float alpha_lp = 0.334; 
    float prev_lp_ir = filteredBuffer[0]; 
    float prev_lp_red = filteredRedBuffer[0]; 

    for (int i = 1; i < BUFFER_SIZE; i++) { 
      float curr_lp_ir = prev_lp_ir + alpha_lp * (filteredBuffer[i] - prev_lp_ir); 
      filteredBuffer[i] = curr_lp_ir; 
      prev_lp_ir = curr_lp_ir; 
      
      float curr_lp_red = prev_lp_red + alpha_lp * (filteredRedBuffer[i] - prev_lp_red); 
      filteredRedBuffer[i] = curr_lp_red; 
      prev_lp_red = curr_lp_red; 
    }

    int ignoreSamples = SAMPLE_RATE * 2;  
    int validSamples = BUFFER_SIZE - ignoreSamples;

    // --- 4. HR Peak Detection ---
    float maxVal = 0;
    for (int i = ignoreSamples; i < BUFFER_SIZE; i++) {
      if (filteredBuffer[i] > maxVal) maxVal = filteredBuffer[i];
    }

    float prominenceThreshold = maxVal * 0.5;  
    int minDistance = SAMPLE_RATE * 0.35;  

    int numPeaks = 0; 
    int lastPeakIndex = -minDistance; 

    for (int i = ignoreSamples; i < BUFFER_SIZE - 1; i++) { 
      if (filteredBuffer[i] > filteredBuffer[i - 1] && 
          filteredBuffer[i] > filteredBuffer[i + 1] && 
          filteredBuffer[i] > prominenceThreshold && 
          (i - lastPeakIndex) >= minDistance) { 
        numPeaks++; 
        lastPeakIndex = i; 
      }
    }

    float validDuration = DURATION_SECONDS - 2.0; 
    finalHR = (numPeaks / validDuration) * 60; 

    // --- 5. Medical-Grade SpO2 Calculation ---
    float acIR = 0, acRed = 0;
    float meanFilteredIR = 0, meanFilteredRed = 0;

    for (int i = ignoreSamples; i < BUFFER_SIZE; i++) {
      meanFilteredIR += filteredBuffer[i];
      meanFilteredRed += filteredRedBuffer[i];
    }
    meanFilteredIR /= validSamples;
    meanFilteredRed /= validSamples;

    for (int i = ignoreSamples; i < BUFFER_SIZE; i++) {
      acIR += pow(filteredBuffer[i] - meanFilteredIR, 2);
      acRed += pow(filteredRedBuffer[i] - meanFilteredRed, 2);
    }
    acIR = sqrt(acIR / validSamples);
    acRed = sqrt(acRed / validSamples);

    if (acIR < 10.0 || acRed < 10.0) {
      finalSpO2 = 0;
      finalHR = 0;
    } 
    else if (meanIR > 0 && meanRed > 0) {
      float R = (acRed / meanRed) / (acIR / meanIR);
      float calculatedSpO2 = 104.0 - (17.0 * R) - (5.0 * R * R);
      
      float calibrationOffset = 2.0; 
      finalSpO2 = (int)(calculatedSpO2 + calibrationOffset); 
      
      if (finalSpO2 > 100) finalSpO2 = 100;
      if (finalSpO2 < 50) finalSpO2 = 0; 
    } else {
      finalSpO2 = 0;
    }

    // --- 6. AWS Publish ---
    if(client.connected()) {
       publishBatch(); 
    }

    resultDisplayTime = millis(); 
    currentState = DONE; 
  }

  // ---------------------------------------------------------
  // STATE 4: DISPLAY RESULT
  // ---------------------------------------------------------
  else if (currentState == DONE) { 
    long elapsedSinceDone = millis() - resultDisplayTime;
    int timeLeft = COOLDOWN_SECONDS - (elapsedSinceDone / 1000);

    display.clearDisplay(); 
    
    // Top Half: Heart Rate & SpO2
    display.setTextSize(2); 
    display.setCursor(0, 0); 
    display.print("HR: ");
    display.println(finalHR); 
    
    display.setCursor(0, 20); 
    display.print("O2: ");
    display.print(finalSpO2); 
    display.println("%"); 

    // Bottom Half: Countdown
    display.setTextSize(1);
    display.setCursor(0, 45);
    display.print("Restarting in:");
    
    display.setTextSize(2);
    display.setCursor(85, 45);
    display.print(timeLeft);
    display.print("s");
    
    display.display(); 

    if (timeLeft <= 0) { 
      currentState = WAITING; 
    }
    
    delay(10); 
  }
}