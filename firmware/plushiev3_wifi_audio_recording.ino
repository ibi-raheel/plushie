/*
 * PlushieAI ESP32-S3 WiFi Manager & Backend Connection with Audio Playback + Voice Recording
 * 
 * Required Libraries (install via Arduino Library Manager or PlatformIO):
 * - ESPAsyncWebServer: https://github.com/me-no-dev/ESPAsyncWebServer
 * - AsyncTCP: https://github.com/me-no-dev/AsyncTCP
 * - DNSServer: Built-in with ESP32 core
 * - Preferences: Built-in with ESP32 core
 * - HTTPClient: Built-in with ESP32 core
 * - WiFi: Built-in with ESP32 core
 * - FFat: Built-in with ESP32 core
 * - I2S: Built-in with ESP32 core
 * 
 * Features:
 * - WiFi management with captive portal
 * - Audio playback on backend connection
 * - Press-to-record voice capture with PSRAM buffering
 * - ADPCM compression and HTTP upload
 * - 
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <FFat.h>
#include <driver/i2s.h>
#include <vector>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#define USE_ADPCM_COMPRESSION true

// Compile-time check for ESP32-S3
#if !CONFIG_IDF_TARGET_ESP32S3
#error "This code is designed specifically for ESP32-S3"
#endif

// Hardware pin definitions
#define BUTTON_PIN          21
#define MIC_POWER_PIN       2

// I2S ports
#define MIC_I2S_PORT        I2S_NUM_0
#define SPK_I2S_PORT        I2S_NUM_1

// Microphone pins
#define I2S_MIC_WS          15
#define I2S_MIC_SCK         18
#define I2S_MIC_SD          6

// Speaker pins
#define I2S_SPK_WS          4
#define I2S_SPK_SCK         5
#define I2S_SPK_DIN         17

// Constants
#define LED_BUILTIN         RGB_BUILTIN
#define WIFI_TIMEOUT        10000  // 10 seconds
#define SERVER_RETRY_DELAY  5000   // 5 seconds
#define AP_NAME             "PlushieAI-Setup"
#define BACKEND_HOST        "http://18.190.135.144"
#define WAKE_FILE_PATH      "/wake.wav"
#define MIN_FILE_SIZE       1024   // 1KB minimum file size

// Voice recording constants
#define SAMPLE_RATE         16000
#define BITS_PER_SAMPLE     16
#define DEBOUNCE_TIME_MS    50
#define INITIAL_BUFFER_SIZE (1024 * 512)  // 512KB initial
#define BUFFER_GROW_SIZE    (1024 * 256)  // 256KB growth chunks
#define MAX_PSRAM_USAGE     0.9f          // 90% of available PSRAM
#define RECORDING_GAIN      2.5f          // Volume boost multiplier
#define SOFT_LIMIT_THRESH   0.5f          // Threshold for soft limiting (50% of max)
#define LOW_PASS_ALPHA      0.15f         // Low-pass filter coefficient (0.1-0.3, lower = more filtering)

// Audio compression constants
#ifdef USE_ADPCM_COMPRESSION
#define ADPCM_BLOCK_SIZE    256           // Process in 256 sample blocks
#else
#endif

// Global objects
AsyncWebServer server(80);
DNSServer dnsServer;
Preferences preferences;
HTTPClient http;

// State variables
enum SystemState {
  STATE_BOOT,
  STATE_CONNECTING,
  STATE_PROVISIONING,
  STATE_CONNECTED,
  STATE_SERVER_CHECK,
  STATE_AUDIO_CHECK,
  STATE_AUDIO_DOWNLOAD,
  STATE_AUDIO_DOWNLOAD_RETRY,
  STATE_AUDIO_PLAY,
  STATE_RECORDING,
  STATE_ENCODING,
  STATE_UPLOADING
};

// Button debounce states
enum ButtonState {
  BUTTON_IDLE,
  BUTTON_PRESSED_DEBOUNCE,
  BUTTON_PRESSED,
  BUTTON_RELEASED_DEBOUNCE
};

SystemState currentState = STATE_BOOT;
unsigned long lastRetryTime = 0;
bool serverConnected = false;
bool fatfsReady = false;
bool i2sReady = false;
bool audioPlaying = false;
bool wasEverDisconnected = false;
bool isFirstConnection = true;
unsigned long lastBlinkTime = 0;
bool blinkState = false;

// Voice recording state variables
ButtonState buttonState = BUTTON_IDLE;
unsigned long buttonStateChangeTime = 0;
unsigned long lastButtonRead = 0;
bool lastButtonValue = HIGH;
bool psramOverflow = false;
bool micI2SInitialized = false;
unsigned long recordingStartTime = 0;

// PSRAM audio buffer
std::vector<int16_t> audioBuffer;
size_t totalSamples = 0;
bool bufferOverflow = false;

// Real-time voice amplification variables
float agcGain = 2.0f;              // Starting gain
float targetLevel = 0.3f;          // Target RMS level (30% of max)
float agcAttack = 0.01f;           // Fast attack for loud signals
float agcRelease = 0.001f;         // Slow release for smooth gain changes
float rmsLevel = 0.0f;             // Running RMS level
float lowPassPrev = 0.0f;          // Previous sample for low-pass filter

// Audio compression variables
#ifdef USE_ADPCM_COMPRESSION
std::vector<uint8_t> compressedData;
bool compressionInitialized = false;

// ADPCM state variables
struct ADPCMState {
  int16_t prevSample;
  int16_t stepIndex;
};
ADPCMState adpcmState;
#else
#endif

// Multi-network storage with smart management
#define MAX_NETWORKS 5
#define PROTECTED_SLOTS 2  // Slots 0-1 are protected for frequently used networks
#define PROMOTION_THRESHOLD 3  // Usage count needed to become protected

struct NetworkCredentials {
  String ssid;
  String password;
  bool isValid;
  int usageCount;
  unsigned long lastUsed;
};
NetworkCredentials storedNetworks[MAX_NETWORKS];
int currentNetworkIndex = 0;
unsigned long disconnectStartTime = 0;
bool autoProvisioningActive = false;
String tempSSID = "";
String tempPassword = "";

// HTML page embedded in PROGMEM
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PlushieAI Setup</title>
    <style>
        * { box-sizing: border-box; }
        
        body { 
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; 
            margin: 0; 
            padding: 20px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            color: #fff;
        }
        
        .container { 
            max-width: 400px; 
            margin: 0 auto; 
            background: rgba(255,255,255,0.95); 
            padding: 30px; 
            border-radius: 15px; 
            box-shadow: 0 8px 32px rgba(0,0,0,0.2);
            color: #333;
            backdrop-filter: blur(10px);
        }
        
        h1 { 
            text-align: center; 
            color: #4a54e1; 
            margin-bottom: 30px;
            font-size: 1.8em;
            font-weight: 600;
        }
        
        .form-group { 
            margin-bottom: 20px; 
        }
        
        label { 
            display: block; 
            margin-bottom: 8px; 
            font-weight: 500;
            color: #555;
            font-size: 14px;
        }
        
        input, select, button { 
            width: 100%; 
            padding: 12px; 
            border: 2px solid #e1e8ff; 
            border-radius: 8px; 
            font-size: 16px;
            transition: border-color 0.2s ease;
        }
        
        input:focus, select:focus {
            outline: none;
            border-color: #4a54e1;
        }
        
        button { 
            background: #4a54e1; 
            color: white; 
            border: none; 
            cursor: pointer; 
            margin-top: 10px;
            font-weight: 500;
            border-radius: 8px;
        }
        
        button:hover { 
            background: #3d47cc;
        }
        
        button:disabled { 
            background: #ccc; 
            cursor: not-allowed;
        }
        
        .checkbox-group { 
            display: flex; 
            align-items: center; 
            padding: 8px 0;
        }
        
        .checkbox-group input[type="checkbox"] { 
            width: 18px;
            height: 18px;
            margin-right: 10px;
            accent-color: #4a54e1;
        }
        
        .checkbox-group label {
            margin: 0;
            cursor: pointer;
            font-size: 14px;
        }
        
        #status { 
            margin-top: 20px; 
            padding: 12px; 
            border-radius: 8px; 
            text-align: center; 
            font-weight: 500;
        }
        
        .status-connecting { 
            background: #fff3cd; 
            color: #856404;
            border: 1px solid #ffeaa7;
        }
        
        .status-error { 
            background: #f8d7da; 
            color: #721c24;
            border: 1px solid #f5c6cb;
        }
        
        .status-success { 
            background: #d4edda; 
            color: #155724;
            border: 1px solid #c3e6cb;
        }
        
        #networks { 
            background: white;
        }
        
        #manualSsid {
            display: none;
            margin-top: 10px;
        }
        
        .or-divider {
            text-align: center;
            margin: 15px 0;
            color: #666;
            font-size: 14px;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>PlushieAI WiFi Setup</h1>
        
        <div class="form-group">
            <button id="scanBtn" onclick="scanNetworks()">Scan Networks</button>
        </div>
        
        <div id="storedNetworks" style="margin-bottom: 15px; padding: 10px; background: rgba(74, 84, 225, 0.1); border-radius: 8px; font-size: 14px;">
            <strong>Known Networks:</strong> <span id="networkCount">Loading...</span>
        </div>
        
        <div class="form-group">
            <label for="networks">Network:</label>
            <select id="networks" onchange="selectNetwork()">
                <option value="">Select a network...</option>
                <option value="__manual__">Enter network name manually...</option>
            </select>
            <input type="text" id="manualSsid" placeholder="Enter network name (SSID)" onchange="selectManualNetwork()">
        </div>
        
        <div class="form-group">
            <label for="password">Password:</label>
            <input type="password" id="password" placeholder="Enter WiFi password">
        </div>
        
        <div class="form-group">
            <div class="checkbox-group">
                <input type="checkbox" id="showPassword" onchange="togglePassword()">
                <label for="showPassword">Show password</label>
            </div>
        </div>
        
        <div class="form-group">
            <button id="connectBtn" onclick="connectWifi()" disabled>Connect</button>
        </div>
        
        <div id="status"></div>
    </div>

    <script>
        let selectedSSID = '';
        
        function scanNetworks() {
            const scanBtn = document.getElementById('scanBtn');
            
            scanBtn.disabled = true;
            scanBtn.textContent = 'Scanning...';
            
            fetch('/scan')
                .then(response => response.json())
                .then(data => {
                    const select = document.getElementById('networks');
                    select.innerHTML = '<option value="">Select a network...</option>';
                    
                    data.networks.forEach(network => {
                        const option = document.createElement('option');
                        option.value = network.ssid;
                        option.textContent = `${network.ssid} (${network.rssi} dBm)`;
                        select.appendChild(option);
                    });
                    
                    // Re-add manual option after scan
                    const manualOption = document.createElement('option');
                    manualOption.value = '__manual__';
                    manualOption.textContent = 'Enter network name manually...';
                    select.appendChild(manualOption);
                    
                    scanBtn.disabled = false;
                    scanBtn.textContent = 'Scan Networks';
                })
                .catch(error => {
                    console.error('Scan failed:', error);
                    scanBtn.disabled = false;
                    scanBtn.textContent = 'Scan Networks';
                });
        }
        
        function selectNetwork() {
            const select = document.getElementById('networks');
            const manualSsidInput = document.getElementById('manualSsid');
            const connectBtn = document.getElementById('connectBtn');
            
            if (select.value === '__manual__') {
                manualSsidInput.style.display = 'block';
                manualSsidInput.focus();
                selectedSSID = '';
                connectBtn.disabled = true;
            } else {
                manualSsidInput.style.display = 'none';
                selectedSSID = select.value;
                connectBtn.disabled = !selectedSSID;
            }
        }
        
        function selectManualNetwork() {
            const manualSsidInput = document.getElementById('manualSsid');
            const connectBtn = document.getElementById('connectBtn');
            
            selectedSSID = manualSsidInput.value.trim();
            connectBtn.disabled = !selectedSSID;
        }
        
        function togglePassword() {
            const passwordField = document.getElementById('password');
            const checkbox = document.getElementById('showPassword');
            passwordField.type = checkbox.checked ? 'text' : 'password';
        }
        
        function connectWifi() {
            if (!selectedSSID) return;
            
            const password = document.getElementById('password').value;
            const statusDiv = document.getElementById('status');
            
            statusDiv.className = 'status-connecting';
            statusDiv.textContent = 'Connecting...';
            
            document.getElementById('connectBtn').disabled = true;
            
            fetch('/save', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: `ssid=${encodeURIComponent(selectedSSID)}&password=${encodeURIComponent(password)}`
            })
            .then(response => response.text())
            .then(data => {
                if (data === 'OK') {
                    checkStatus();
                } else {
                    statusDiv.className = 'status-error';
                    statusDiv.textContent = 'Failed to save credentials';
                    document.getElementById('connectBtn').disabled = false;
                }
            })
            .catch(error => {
                statusDiv.className = 'status-error';
                statusDiv.textContent = 'Connection failed';
                document.getElementById('connectBtn').disabled = false;
            });
        }
        
        function checkStatus() {
            fetch('/status')
                .then(response => response.text())
                .then(status => {
                    const statusDiv = document.getElementById('status');
                    
                    if (status === 'connecting') {
                        statusDiv.className = 'status-connecting';
                        statusDiv.textContent = 'Connecting...';
                        setTimeout(checkStatus, 1000);
                    } else if (status === 'connected') {
                        statusDiv.className = 'status-success';
                        statusDiv.textContent = 'Connected! Testing backend connection...';
                    } else if (status === 'wrong_password') {
                        statusDiv.className = 'status-error';
                        statusDiv.textContent = 'Wrong password - please try again';
                        document.getElementById('connectBtn').disabled = false;
                    } else if (status === 'failed') {
                        statusDiv.className = 'status-error';
                        statusDiv.textContent = 'Network not found or connection failed';
                        document.getElementById('connectBtn').disabled = false;
                    } else if (status === 'timeout') {
                        statusDiv.className = 'status-error';
                        statusDiv.textContent = 'Connection timeout - check password and try again';
                        document.getElementById('connectBtn').disabled = false;
                    } else {
                        setTimeout(checkStatus, 1000);
                    }
                })
                .catch(error => {
                    setTimeout(checkStatus, 1000);
                });
        }
        
        function loadStoredNetworks() {
            fetch('/networks')
                .then(response => response.json())
                .then(data => {
                    const countSpan = document.getElementById('networkCount');
                    if (data.count > 0) {
                        countSpan.textContent = data.count + ' (' + data.networks + ')';
                    } else {
                        countSpan.textContent = 'None - this will be your first network';
                    }
                })
                .catch(error => {
                    document.getElementById('networkCount').textContent = 'Error loading';
                });
        }
        
        // Auto-scan and load stored networks on page load
        window.onload = function() {
            loadStoredNetworks();
            scanNetworks();
        };
    </script>
</body>
</html>
)rawliteral";

// Function declarations
void optimizeTCPForUploads();

// Network management functions
void loadStoredNetworks() {
  Serial.println("Loading stored networks...");
  for (int i = 0; i < MAX_NETWORKS; i++) {
    String ssidKey = "net" + String(i) + "_ssid";
    String passKey = "net" + String(i) + "_pass";
    String countKey = "net" + String(i) + "_count";
    String lastKey = "net" + String(i) + "_last";
    
    storedNetworks[i].ssid = preferences.getString(ssidKey.c_str(), "");
    storedNetworks[i].password = preferences.getString(passKey.c_str(), "");
    storedNetworks[i].usageCount = preferences.getInt(countKey.c_str(), 0);
    storedNetworks[i].lastUsed = preferences.getULong(lastKey.c_str(), 0);
    storedNetworks[i].isValid = (storedNetworks[i].ssid.length() > 0);
    
    if (storedNetworks[i].isValid) {
      String slotType = (i < PROTECTED_SLOTS) ? " [PROTECTED]" : " [ROTATING]";
      Serial.printf("Loaded network %d: %s (used %d times)%s\n", 
                    i, storedNetworks[i].ssid.c_str(), storedNetworks[i].usageCount, slotType.c_str());
    }
  }
}

void saveNetworkToPreferences(int slot) {
  String ssidKey = "net" + String(slot) + "_ssid";
  String passKey = "net" + String(slot) + "_pass";
  String countKey = "net" + String(slot) + "_count";
  String lastKey = "net" + String(slot) + "_last";
  
  preferences.putString(ssidKey.c_str(), storedNetworks[slot].ssid);
  preferences.putString(passKey.c_str(), storedNetworks[slot].password);
  preferences.putInt(countKey.c_str(), storedNetworks[slot].usageCount);
  preferences.putULong(lastKey.c_str(), storedNetworks[slot].lastUsed);
}

void promoteToProtectedSlot(int sourceSlot) {
  Serial.printf("Promoting network '%s' to protected slot\n", storedNetworks[sourceSlot].ssid.c_str());
  
  // Find least frequently used protected slot or oldest protected slot
  int targetSlot = 0;
  for (int i = 1; i < PROTECTED_SLOTS; i++) {
    if (!storedNetworks[i].isValid || 
        storedNetworks[i].usageCount < storedNetworks[targetSlot].usageCount ||
        (storedNetworks[i].usageCount == storedNetworks[targetSlot].usageCount && 
         storedNetworks[i].lastUsed < storedNetworks[targetSlot].lastUsed)) {
      targetSlot = i;
    }
  }
  
  // Move network to protected slot
  NetworkCredentials temp = storedNetworks[sourceSlot];
  storedNetworks[sourceSlot] = storedNetworks[targetSlot];
  storedNetworks[targetSlot] = temp;
  
  // Save both slots
  saveNetworkToPreferences(sourceSlot);
  saveNetworkToPreferences(targetSlot);
  
  Serial.printf("Network promoted to protected slot %d\n", targetSlot);
}

void addNetwork(String ssid, String password) {
  Serial.printf("Adding/updating network: %s\n", ssid.c_str());
  
  // Check if network already exists
  for (int i = 0; i < MAX_NETWORKS; i++) {
    if (storedNetworks[i].isValid && storedNetworks[i].ssid == ssid) {
      // Update existing network
      storedNetworks[i].password = password;
      storedNetworks[i].usageCount++;
      storedNetworks[i].lastUsed = millis();
      saveNetworkToPreferences(i);
      
      Serial.printf("Updated network in slot %d (used %d times)\n", i, storedNetworks[i].usageCount);
      
      // Check if it should be promoted to protected slot
      if (i >= PROTECTED_SLOTS && storedNetworks[i].usageCount >= PROMOTION_THRESHOLD) {
        promoteToProtectedSlot(i);
      }
      return;
    }
  }
  
  // This is a new network - add it
  int targetSlot = -1;
  
  // First, try to find an empty slot
  for (int i = 0; i < MAX_NETWORKS; i++) {
    if (!storedNetworks[i].isValid) {
      targetSlot = i;
      break;
    }
  }
  
  // If no empty slot, find the least valuable rotating slot to replace
  if (targetSlot == -1) {
    targetSlot = PROTECTED_SLOTS; // Start with first rotating slot
    for (int i = PROTECTED_SLOTS + 1; i < MAX_NETWORKS; i++) {
      // Replace slot with lowest usage count or oldest last used time
      if (storedNetworks[i].usageCount < storedNetworks[targetSlot].usageCount ||
          (storedNetworks[i].usageCount == storedNetworks[targetSlot].usageCount && 
           storedNetworks[i].lastUsed < storedNetworks[targetSlot].lastUsed)) {
        targetSlot = i;
      }
    }
    Serial.printf("Replacing least valuable network in slot %d\n", targetSlot);
  }
  
  // Add new network
  storedNetworks[targetSlot].ssid = ssid;
  storedNetworks[targetSlot].password = password;
  storedNetworks[targetSlot].usageCount = 1;
  storedNetworks[targetSlot].lastUsed = millis();
  storedNetworks[targetSlot].isValid = true;
  
  saveNetworkToPreferences(targetSlot);
  
  String slotType = (targetSlot < PROTECTED_SLOTS) ? "PROTECTED" : "ROTATING";
  Serial.printf("Added new network to %s slot %d\n", slotType.c_str(), targetSlot);
}

bool tryStoredNetworks() {
  Serial.println("Trying stored networks...");
  
  // Ensure WiFi is in a clean state
  WiFi.disconnect(true);
  delay(1000);
  
  for (int i = 0; i < MAX_NETWORKS; i++) {
    if (!storedNetworks[i].isValid) continue;
    
    Serial.printf("Trying network %d: %s\n", i, storedNetworks[i].ssid.c_str());
    
    // Ensure clean state before each attempt
    WiFi.disconnect(true);
    delay(1000); // Wait longer for WiFi to properly disconnect
    
    // Wait for WiFi to be in a proper state before starting connection
    unsigned long waitStart = millis();
    while (WiFi.status() == WL_CONNECT_FAILED && millis() - waitStart < 2000) {
      delay(100);
    }
    
    WiFi.begin(storedNetworks[i].ssid.c_str(), storedNetworks[i].password.c_str());
    
    // Wait up to 10 seconds for this network
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
      delay(500);
      Serial.print(".");
      
      // Check if WiFi got stuck - but don't restart immediately
      if (WiFi.status() == WL_DISCONNECTED) {
        Serial.print("D"); // Indicate disconnected state
        // Don't restart connection here - let the timeout handle it
      }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nConnected to: %s\n", storedNetworks[i].ssid.c_str());
      currentNetworkIndex = i;
      
      // Update usage statistics
      storedNetworks[i].usageCount++;
      storedNetworks[i].lastUsed = millis();
      saveNetworkToPreferences(i);
      
      // Check if it should be promoted to protected slot
      if (i >= PROTECTED_SLOTS && storedNetworks[i].usageCount >= PROMOTION_THRESHOLD) {
        promoteToProtectedSlot(i);
      }
      
      return true;
    } else {
      Serial.printf("\nFailed to connect to: %s (status: %d)\n", storedNetworks[i].ssid.c_str(), WiFi.status());
      WiFi.disconnect(true);
      delay(1000); // Give time for clean disconnect
    }
  }
  
  Serial.println("No stored networks available");
  WiFi.disconnect(true);
  return false;
}

// Audio and filesystem functions
bool initializeFilesystem() {
  Serial.println("Initializing FATFS...");
  if (!FFat.begin(true)) {
    Serial.println("FATFS mount failed");
    return false;
  }
  
  Serial.println("FATFS mounted successfully");
  Serial.printf("Total space: %u bytes\n", FFat.totalBytes());
  Serial.printf("Used space: %u bytes\n", FFat.usedBytes());
  
  fatfsReady = true;
  return true;
}

bool initializeI2S() {
  Serial.println("Initializing I2S for audio playback...");
  
  i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Mono to match server
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SPK_SCK,
    .ws_io_num = I2S_SPK_WS, 
    .data_out_num = I2S_SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  
  if (i2s_driver_install(SPK_I2S_PORT, &i2s_config, 0, NULL) != ESP_OK) {
    Serial.println("I2S driver install failed");
    return false;
  }
  
  if (i2s_set_pin(SPK_I2S_PORT, &pin_config) != ESP_OK) {
    Serial.println("I2S pin config failed");
    return false;
  }
  
  Serial.println("I2S initialized successfully");
  i2sReady = true;
  return true;
}

bool checkWakeFile() {
  if (!fatfsReady) return false;
  
  File file = FFat.open(WAKE_FILE_PATH, "r");
  if (!file) {
    Serial.println("Wake file not found");
    return false;
  }
  
  size_t fileSize = file.size();
  file.close();
  
  if (fileSize < MIN_FILE_SIZE) {
    Serial.printf("Wake file too small: %d bytes (minimum %d)\n", fileSize, MIN_FILE_SIZE);
    return false;
  }
  
  Serial.printf("Wake file found: %d bytes\n", fileSize);
  return true;
}

bool downloadWakeFile() {
  if (!fatfsReady || WiFi.status() != WL_CONNECTED) return false;
  
  Serial.println("Downloading wake file...");
  setLEDColor(0, 0, 255); // Blue LED during download
  
  http.begin(String(BACKEND_HOST) + "/wakeup");
  http.setTimeout(60000); // 60 seconds for large file download
  http.setConnectTimeout(10000); // 10 second connection timeout
  http.setReuse(false); // Don't reuse connections
  
  Serial.println("Starting HTTP GET request...");
  int httpCode = http.GET();
  Serial.printf("HTTP response code: %d\n", httpCode);
  
  if (httpCode != 200) {
    Serial.printf("Download failed: HTTP %d\n", httpCode);
    if (httpCode > 0) {
      String response = http.getString();
      Serial.printf("Server response: %s\n", response.c_str());
    }
    http.end();
    
    // Blink red on failure
    for (int i = 0; i < 3; i++) {
      setLEDColor(255, 0, 0);
      delay(250);
      setLEDColor(0, 0, 0);
      delay(250);
    }
    return false;
  }
  
  // Get content length if available
  int contentLength = http.getSize();
  Serial.printf("Content-Length: %d bytes\n", contentLength);
  
  // Check if we're still connected
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected during download");
    http.end();
    return false;
  }
  
  WiFiClient* stream = http.getStreamPtr();
  File file = FFat.open(WAKE_FILE_PATH, "w");
  
  if (!file) {
    Serial.println("Failed to create wake file");
    http.end();
    return false;
  }
  
  size_t totalBytes = 0;
  uint8_t buffer[4096]; // Larger buffer for faster download
  unsigned long lastProgressTime = millis();
  
  Serial.println("Starting file download...");
  Serial.printf("WiFi RSSI: %d dBm\n", WiFi.RSSI());
  
  // Continue reading until no more data or timeout
  unsigned long downloadStartTime = millis();
  
  while (http.connected() || stream->available()) {
    // Check WiFi connection during download
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected during download");
      file.close();
      FFat.remove(WAKE_FILE_PATH); // Remove partial file
      http.end();
      return false;
    }
    
    // Check for timeout (60 seconds total)
    if (millis() - downloadStartTime > 60000) {
      Serial.println("Download timeout");
      file.close();
      FFat.remove(WAKE_FILE_PATH);
      http.end();
      return false;
    }
    
    if (stream->available()) {
      size_t bytesRead = stream->readBytes(buffer, sizeof(buffer));
      if (bytesRead > 0) {
        size_t bytesWritten = file.write(buffer, bytesRead);
        if (bytesWritten != bytesRead) {
          Serial.printf("Write error: tried %d, wrote %d\n", bytesRead, bytesWritten);
          file.close();
          FFat.remove(WAKE_FILE_PATH);
          http.end();
          return false;
        }
        totalBytes += bytesRead;
        
        // Progress update every 2 seconds
        if (millis() - lastProgressTime > 2000) {
          float progressKB = totalBytes / 1024.0f;
          if (contentLength > 0) {
            float progress = (float)totalBytes / contentLength * 100.0f;
            Serial.printf("Downloaded: %.1f KB / %.1f KB (%.1f%%)\n", 
                          progressKB, contentLength / 1024.0f, progress);
          } else {
            Serial.printf("Downloaded: %.1f KB\n", progressKB);
          }
          lastProgressTime = millis();
        }
      }
    } else {
      // No data available, small delay to prevent tight loop
      delay(10);
    }
    
    yield(); // Allow other tasks
  }
  
  file.close();
  http.end();
  
  Serial.printf("Download completed: %d bytes\n", totalBytes);
  
  if (totalBytes < MIN_FILE_SIZE) {
    Serial.printf("Downloaded file too small: %d bytes (minimum %d)\n", totalBytes, MIN_FILE_SIZE);
    FFat.remove(WAKE_FILE_PATH);
    return false;
  }
  
  Serial.printf("Wake file downloaded successfully: %d bytes (%.1f KB)\n", 
                totalBytes, totalBytes / 1024.0f);
  setLEDColor(0, 255, 0); // Green LED on success
  delay(1000);
  return true;
}

bool playWakeFile() {
  if (!fatfsReady || !i2sReady || WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot play audio: system not ready or WiFi disconnected");
    return false;
  }
  
  File file = FFat.open(WAKE_FILE_PATH, "r");
  if (!file) {
    Serial.println("Cannot open wake file for playback");
    return false;
  }
  
  Serial.println("Starting audio playback...");
  audioPlaying = true;
  
  // Skip WAV header (44 bytes) - simple approach
  file.seek(44);
  
  uint8_t buffer[512];
  size_t bytesWritten;
  
  while (file.available() && audioPlaying) {
    // Check WiFi connection during playback
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected during playback - aborting");
      audioPlaying = false;
      break;
    }
    
    size_t bytesRead = file.readBytes((char*)buffer, sizeof(buffer));
    if (bytesRead > 0) {
      if (i2s_write(SPK_I2S_PORT, buffer, bytesRead, &bytesWritten, portMAX_DELAY) != ESP_OK) {
        Serial.println("I2S write failed");
        break;
      }
    }
    yield(); // Allow other tasks
  }
  
  file.close();
  audioPlaying = false;
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Audio playback completed");
    return true;
  } else {
    Serial.println("Audio playback aborted due to WiFi disconnection");
    return false;
  }
}

// ============================================
// STREAMING AUDIO PLAYBACK WITH RING BUFFER
// ============================================

// Ring buffer configuration - balanced for ESP32-S3 memory limits
#define RING_BUFFER_SIZE      (64 * 1024)  // 64KB ring buffer (reasonable for ESP32-S3)
#define CHUNK_SIZE            4096          // 4KB chunks for good performance
#define MIN_BUFFER_FILL       (32 * 1024)  // Start playing when 32KB buffered (1 second)
#define REFILL_THRESHOLD      (16 * 1024)  // Refill when below 16KB (0.5 seconds)

// Ring buffer structure
typedef struct {
  uint8_t* buffer;
  size_t head;
  size_t tail;
  size_t size;
  size_t capacity;
  bool overflow;
} RingBuffer;

// Streaming playback variables
RingBuffer ringBuffer;
TaskHandle_t downloadTaskHandle = nullptr;
TaskHandle_t playbackTaskHandle = nullptr;
HTTPClient* streamingHttp = nullptr;
WiFiClient* streamingClient = nullptr;
bool streamingActive = false;
bool downloadComplete = false;
size_t totalStreamBytes = 0;
size_t streamBytesPlayed = 0;
String streamingUrl = "";

// Function prototypes
bool initRingBuffer(RingBuffer* rb, size_t capacity);
void destroyRingBuffer(RingBuffer* rb);
size_t ringBufferWrite(RingBuffer* rb, const uint8_t* data, size_t len);
size_t ringBufferRead(RingBuffer* rb, uint8_t* data, size_t len);
size_t ringBufferAvailable(RingBuffer* rb);
size_t ringBufferFree(RingBuffer* rb);
void downloadTask(void* parameter);
void playbackTask(void* parameter);
bool playStreamingAudio(const String& url);
void stopStreamingAudio();
bool isStreamingActive();
bool streamResponseAudio(HTTPClient* http, int contentLength);

// Ring buffer functions
bool initRingBuffer(RingBuffer* rb, size_t capacity) {
  // Check available PSRAM before allocation
  size_t freePsram = esp_psram_get_size() - (esp_psram_get_size() - heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("Available PSRAM: %d bytes, requesting: %d bytes\n", freePsram, capacity);
  
  if (freePsram < capacity + 32768) { // Need extra headroom
    Serial.printf("Insufficient PSRAM for ring buffer. Available: %d, needed: %d\n", freePsram, capacity);
    return false;
  }
  
  rb->buffer = (uint8_t*)ps_malloc(capacity);
  if (!rb->buffer) {
    Serial.println("Failed to allocate ring buffer in PSRAM");
    return false;
  }
  rb->head = 0;
  rb->tail = 0;
  rb->size = 0;
  rb->capacity = capacity;
  rb->overflow = false;
  Serial.printf("Ring buffer initialized: %d bytes in PSRAM\n", capacity);
  return true;
}

void destroyRingBuffer(RingBuffer* rb) {
  if (rb->buffer) {
    free(rb->buffer);
    rb->buffer = nullptr;
  }
  rb->head = rb->tail = rb->size = 0;
}

size_t ringBufferWrite(RingBuffer* rb, const uint8_t* data, size_t len) {
  if (!rb->buffer || !data || len == 0) return 0;
  
  size_t written = 0;
  for (size_t i = 0; i < len; i++) {
    if (rb->size < rb->capacity) {
      // Bounds check to prevent corruption
      if (rb->head < rb->capacity) {
        rb->buffer[rb->head] = data[i];
        rb->head = (rb->head + 1) % rb->capacity;
        rb->size++;
        written++;
      } else {
        Serial.println("Ring buffer head out of bounds!");
        break;
      }
    } else {
      rb->overflow = true;
      break;
    }
  }
  return written;
}

size_t ringBufferRead(RingBuffer* rb, uint8_t* data, size_t len) {
  if (!rb->buffer || !data || len == 0) return 0;
  
  size_t read = 0;
  for (size_t i = 0; i < len && rb->size > 0; i++) {
    // Bounds check to prevent corruption
    if (rb->tail < rb->capacity) {
      data[i] = rb->buffer[rb->tail];
      rb->tail = (rb->tail + 1) % rb->capacity;
      rb->size--;
      read++;
    } else {
      Serial.println("Ring buffer tail out of bounds!");
      break;
    }
  }
  return read;
}

size_t ringBufferAvailable(RingBuffer* rb) {
  return rb ? rb->size : 0;
}

size_t ringBufferFree(RingBuffer* rb) {
  return rb ? (rb->capacity - rb->size) : 0;
}

// Download task - runs on separate core
void downloadTask(void* parameter) {
  Serial.println("Download task started");
  
  streamingHttp = new HTTPClient();
  streamingHttp->begin(streamingUrl);
  streamingHttp->setTimeout(30000);
  
  int httpCode = streamingHttp->GET();
  if (httpCode != 200) {
    Serial.printf("Streaming download failed: HTTP %d\n", httpCode);
    downloadComplete = true;
    vTaskDelete(NULL);
    return;
  }
  
  totalStreamBytes = streamingHttp->getSize();
  streamingClient = streamingHttp->getStreamPtr();
  
  Serial.printf("Streaming started: %d bytes total\n", totalStreamBytes);
  
  // Skip WAV header (44 bytes)
  if (totalStreamBytes > 44) {
    uint8_t header[44];
    size_t headerRead = 0;
    while (headerRead < 44 && streamingClient->available()) {
      headerRead += streamingClient->readBytes(header + headerRead, 44 - headerRead);
      yield();
    }
    Serial.println("WAV header skipped");
  }
  
  uint8_t chunk[CHUNK_SIZE];
  size_t totalDownloaded = 44; // Header already read
  
  while (streamingActive && streamingClient->connected() && totalDownloaded < totalStreamBytes) {
    // Wait if buffer is full
    while (ringBufferFree(&ringBuffer) < CHUNK_SIZE && streamingActive) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    
    if (!streamingActive) break;
    
    size_t available = streamingClient->available();
    if (available > 0) {
      size_t toRead = min(available, (size_t)CHUNK_SIZE);
      toRead = min(toRead, totalStreamBytes - totalDownloaded);
      
      size_t bytesRead = streamingClient->readBytes(chunk, toRead);
      if (bytesRead > 0) {
        size_t written = ringBufferWrite(&ringBuffer, chunk, bytesRead);
        totalDownloaded += written;
        
        if (totalDownloaded % 4096 == 0) {
          float progress = (float)totalDownloaded / totalStreamBytes * 100.0f;
          Serial.printf("Downloaded: %.1f%% (%d/%d bytes)\n", progress, totalDownloaded, totalStreamBytes);
        }
      }
    } else {
      vTaskDelay(5 / portTICK_PERIOD_MS);
    }
    
    yield();
  }
  
  downloadComplete = true;
  Serial.printf("Download complete: %d bytes\n", totalDownloaded);
  
  streamingHttp->end();
  delete streamingHttp;
  streamingHttp = nullptr;
  
  vTaskDelete(NULL);
}

// Playback task - runs on separate core
void playbackTask(void* parameter) {
  Serial.println("Playback task started");
  
  // Wait for minimum buffer fill
  while (ringBufferAvailable(&ringBuffer) < MIN_BUFFER_FILL && !downloadComplete && streamingActive) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  
  if (!streamingActive) {
    playbackTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }
  
  Serial.println("Starting stream playback...");
  uint8_t playBuffer[512];
  size_t bytesWritten;
  
  while (streamingActive && (ringBufferAvailable(&ringBuffer) > 0 || !downloadComplete)) {
    size_t available = ringBufferAvailable(&ringBuffer);
    if (available > 0) {
      size_t toRead = min(available, sizeof(playBuffer));
      size_t bytesRead = ringBufferRead(&ringBuffer, playBuffer, toRead);
      
      if (bytesRead > 0) {
        esp_err_t result = i2s_write(SPK_I2S_PORT, playBuffer, bytesRead, &bytesWritten, portMAX_DELAY);
        if (result == ESP_OK) {
          streamBytesPlayed += bytesWritten;
        } else {
          Serial.println("I2S write failed during streaming");
          break;
        }
      }
    } else if (!downloadComplete) {
      // Buffer underrun - implement adaptive buffering
      size_t currentBuffer = ringBufferAvailable(&ringBuffer);
      Serial.printf("Buffer underrun - only %d bytes available, waiting for refill to %d bytes...\n", 
                   currentBuffer, REFILL_THRESHOLD);
      
      // Adaptive buffering - wait longer for refill to prevent repeated underruns
      int waitCount = 0;
      while (ringBufferAvailable(&ringBuffer) < REFILL_THRESHOLD && !downloadComplete && streamingActive) {
        vTaskDelay(200 / portTICK_PERIOD_MS); // Check every 200ms
        waitCount++;
        
        // Show progress every 5 checks (1 second)
        if (waitCount % 5 == 0) {
          Serial.printf("Buffering... %d bytes available (target: %d bytes)\n", 
                       ringBufferAvailable(&ringBuffer), REFILL_THRESHOLD);
        }
      }
      
      if (streamingActive && !downloadComplete) {
        Serial.printf("Buffer refilled to %d bytes - resuming playback\n", ringBufferAvailable(&ringBuffer));
      }
    } else {
      // No more data and download complete
      break;
    }
    
    yield();
  }
  
  Serial.printf("Stream playback complete: %d bytes played\n", streamBytesPlayed);
  playbackTaskHandle = nullptr;
  vTaskDelete(NULL);
}

// Main streaming function
bool playStreamingAudio(const String& url) {
  if (!i2sReady || WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot stream audio: I2S or WiFi not ready");
    return false;
  }
  
  // Stop any existing streaming
  stopStreamingAudio();
  
  // Initialize ring buffer
  if (!initRingBuffer(&ringBuffer, RING_BUFFER_SIZE)) {
    return false;
  }
  
  streamingUrl = url;
  streamingActive = true;
  downloadComplete = false;
  totalStreamBytes = 0;
  streamBytesPlayed = 0;
  
  Serial.printf("Starting streaming playback from: %s\n", url.c_str());
  
  // Create download task on core 0 with larger stack
  xTaskCreatePinnedToCore(
    downloadTask,
    "DownloadTask",
    16384,  // 16KB stack size for large responses
    NULL,
    2,      // Priority
    &downloadTaskHandle,
    0       // Core 0
  );
  
  // Create playback task on core 1 with larger stack
  xTaskCreatePinnedToCore(
    playbackTask,
    "PlaybackTask", 
    16384,  // 16KB stack size for large responses
    NULL,
    3,      // Higher priority
    &playbackTaskHandle,
    1       // Core 1
  );
  
  return true;
}

void stopStreamingAudio() {
  streamingActive = false;
  
  // Give tasks time to detect the flag change and exit gracefully
  delay(100);
  
  // Wait for tasks to finish with timeout
  unsigned long taskStopStart = millis();
  while ((downloadTaskHandle != nullptr || playbackTaskHandle != nullptr) && 
         (millis() - taskStopStart) < 2000) {
    delay(50);
  }
  
  // Force cleanup if tasks didn't exit gracefully
  if (downloadTaskHandle) {
    vTaskDelete(downloadTaskHandle);
    downloadTaskHandle = nullptr;
  }
  
  if (playbackTaskHandle) {
    vTaskDelete(playbackTaskHandle);
    playbackTaskHandle = nullptr;
  }
  
  // Clean up resources
  destroyRingBuffer(&ringBuffer);
  
  Serial.println("Streaming audio stopped");
}

bool isStreamingActive() {
  return streamingActive && (downloadTaskHandle != nullptr || playbackTaskHandle != nullptr);
}

// Stream audio directly from HTTP response
bool streamResponseAudio(HTTPClient* http, int contentLength) {
  if (!i2sReady || !http) {
    Serial.println("Cannot stream response: I2S not ready or invalid HTTP client");
    return false;
  }
  
  // Stop any existing streaming
  stopStreamingAudio();
  
  // Initialize ring buffer
  if (!initRingBuffer(&ringBuffer, RING_BUFFER_SIZE)) {
    return false;
  }
  
  streamingActive = true;
  downloadComplete = false;
  totalStreamBytes = contentLength;
  streamBytesPlayed = 0;
  
  Serial.printf("Starting response audio streaming: %d bytes\n", contentLength);
  
  WiFiClient* client = http->getStreamPtr();
  if (!client) {
    Serial.println("Failed to get stream client");
    destroyRingBuffer(&ringBuffer);
    return false;
  }
  
  // Read and analyze WAV header if present (44 bytes)
  uint8_t header[44];
  if (contentLength > 44) {
    size_t headerRead = 0;
    while (headerRead < 44 && client->available()) {
      headerRead += client->readBytes(header + headerRead, 44 - headerRead);
    }
    
    // Parse WAV header for debugging
    if (headerRead >= 44 && memcmp(header, "RIFF", 4) == 0) {
      uint32_t fileSize = *((uint32_t*)(header + 4));
      uint16_t audioFormat = *((uint16_t*)(header + 20));
      uint16_t numChannels = *((uint16_t*)(header + 22));
      uint32_t sampleRate = *((uint32_t*)(header + 24));
      uint16_t bitsPerSample = *((uint16_t*)(header + 34));
      uint32_t dataSize = *((uint32_t*)(header + 40));
      
      Serial.printf("=== WAV HEADER ANALYSIS ===\n");
      Serial.printf("File size: %d bytes\n", fileSize);
      Serial.printf("Audio format: %d (1=PCM)\n", audioFormat);
      Serial.printf("Channels: %d\n", numChannels);
      Serial.printf("Sample rate: %d Hz\n", sampleRate);
      Serial.printf("Bits per sample: %d\n", bitsPerSample);
      Serial.printf("Data size: %d bytes\n", dataSize);
      Serial.printf("Expected duration: %.2f seconds\n", (float)dataSize / (sampleRate * numChannels * (bitsPerSample/8)));
      Serial.printf("==========================\n");
      
      // Show first 16 bytes of header for debugging
      Serial.print("Header bytes: ");
      for (int i = 0; i < 16; i++) {
        Serial.printf("%02X ", header[i]);
      }
      Serial.println();
    } else {
      Serial.println("Invalid or non-WAV header detected");
      Serial.print("First 16 bytes: ");
      for (int i = 0; i < 16; i++) {
        Serial.printf("%02X ", header[i]);
      }
      Serial.println();
    }
    
    Serial.println("WAV header skipped from response");
    // Use actual content length instead of corrupted header data size
    totalStreamBytes = contentLength - 44;
  }
  
  // Start playback task with larger stack
  xTaskCreatePinnedToCore(
    playbackTask,
    "PlaybackTask", 
    16384,  // 16KB stack size for large responses
    NULL,
    3,      // Higher priority
    &playbackTaskHandle,
    1       // Core 1
  );
  
  // Stream data directly in this function (no separate download task needed)
  uint8_t chunk[CHUNK_SIZE];
  size_t totalRead = (contentLength > 44) ? 44 : 0; // Header already read
  unsigned long lastDataTime = millis();
  const unsigned long DATA_TIMEOUT = 10000; // 10 second timeout
  
  Serial.printf("Starting to read %d bytes of audio data...\n", contentLength - 44);
  
  while (streamingActive && totalRead < contentLength) {
    // Wait if buffer is full
    while (ringBufferFree(&ringBuffer) < CHUNK_SIZE && streamingActive) {
      delay(10);
    }
    
    if (!streamingActive) break;
    
    // Check if data is available or connection is still active
    if (client->available() || client->connected()) {
      size_t available = client->available();
      if (available > 0) {
        size_t toRead = min(available, (size_t)CHUNK_SIZE);
        toRead = min(toRead, contentLength - totalRead);
        
        size_t bytesRead = client->readBytes(chunk, toRead);
        if (bytesRead > 0) {
          size_t written = ringBufferWrite(&ringBuffer, chunk, bytesRead);
          totalRead += written;
          lastDataTime = millis(); // Reset timeout
          
          if (totalRead % 16384 == 0) { // Report every 16KB
            float progress = (float)totalRead / contentLength * 100.0f;
            size_t bufferUsed = ringBufferAvailable(&ringBuffer);
            float bufferPercent = (float)bufferUsed / RING_BUFFER_SIZE * 100.0f;
            unsigned long downloadTime = millis() - lastDataTime;
            float downloadSpeed = (float)bytesRead / (downloadTime / 1000.0f) / 1024.0f; // KB/s
            
            Serial.printf("Download: %.1f%% (%d/%d bytes) | Buffer: %d bytes (%.1f%%) | Speed: %.1f KB/s\n", 
                         progress, totalRead, contentLength, bufferUsed, bufferPercent, downloadSpeed);
          }
        }
      } else if (client->connected()) {
        // No data available but still connected - wait a bit
        delay(10);
        
        // Check for timeout
        if (millis() - lastDataTime > DATA_TIMEOUT) {
          Serial.printf("Data timeout after %d bytes - stopping\n", totalRead);
          break;
        }
      } else {
        // Connection closed
        Serial.printf("Connection closed after %d bytes\n", totalRead);
        break;
      }
    } else {
      // Not connected and no data
      Serial.printf("No connection and no data after %d bytes\n", totalRead);
      break;
    }
    
    yield();
  }
  
  downloadComplete = true;
  Serial.printf("Response streaming complete: %d bytes\n", totalRead);
  
  // Wait for playback task to finish processing remaining buffer data
  unsigned long cleanupStart = millis();
  while (playbackTaskHandle != nullptr && (millis() - cleanupStart) < 5000) {
    delay(100);
  }
  
  // Clean up streaming resources
  streamingActive = false;
  if (playbackTaskHandle) {
    vTaskDelete(playbackTaskHandle);
    playbackTaskHandle = nullptr;
  }
  destroyRingBuffer(&ringBuffer);
  Serial.println("Response streaming cleanup complete");
  
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== PlushieAI ESP32-S3 Starting ===");
  
  // Initialize button pin
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // Initialize built-in LED
  pinMode(LED_BUILTIN, OUTPUT);
  ledStartup(); // White pulse startup indicator
  
  // Check PSRAM availability
  if (!psramInit()) {
    Serial.println("PSRAM initialization failed!");
    ledMemoryError(); // Purple 2 blinks for memory error
    while(1) delay(1000);
  }
  
  size_t psramSize = esp_psram_get_size();
  Serial.printf("PSRAM initialized: %d bytes available\n", psramSize);
  
  // Reserve initial buffer space in PSRAM
  try {
    audioBuffer.reserve(INITIAL_BUFFER_SIZE / sizeof(int16_t));
    Serial.printf("Audio buffer reserved: %d samples\n", INITIAL_BUFFER_SIZE / sizeof(int16_t));
  } catch (std::bad_alloc& e) {
    Serial.println("Failed to reserve PSRAM buffer!");
    setLEDColor(255, 0, 0);
    while(1) delay(1000);
  }
  
  // Initialize preferences
  preferences.begin("plushie", false);
  
  // Load stored networks
  loadStoredNetworks();
  
  // Initialize filesystem and I2S
  initializeFilesystem();
  initializeI2S();
  
  // Set WiFi power to 19.5dBm
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.println("WiFi power set to 19.5dBm");
  
  // Optimize TCP for large uploads
  optimizeTCPForUploads();
  
  // Start WiFi setup flow
  setupWifi();
}

void loop() {
  static unsigned long lastCheck = 0;
  
  // Handle button input for voice recording
  updateButtonState();
  
  switch (currentState) {
    case STATE_CONNECTING:
      handleWifiConnection();
      break;
      
    case STATE_PROVISIONING:
      dnsServer.processNextRequest();
      
      // Continue trying stored networks in background during auto-provisioning
      // But only if no one is actively using the captive portal
      if (autoProvisioningActive && millis() - lastCheck > 10000) { // Try every 10 seconds
        int connectedClients = WiFi.softAPgetStationNum();
        if (connectedClients == 0) {
          Serial.println("Auto-provisioning: trying stored networks...");
          if (tryStoredNetworks()) {
            Serial.println("Background reconnection successful!");
            
            // Don't stop provisioning services immediately - let status page show progress
            // They'll be stopped after BigMax download completes or after timeout
            
            // Reset state
            disconnectStartTime = 0;
            autoProvisioningActive = false;
            currentState = STATE_CONNECTED;
          }
        } else {
          Serial.printf("Skipping stored network retry - %d client(s) connected to captive portal\n", connectedClients);
        }
        lastCheck = millis();
      }
      break;
      
    case STATE_CONNECTED:
      // Check if streaming audio is complete
      static bool wasStreaming = false;
      if (wasStreaming && !isStreamingActive()) {
        Serial.println("Streaming audio completed - turning off LED");
        setLEDColor(0, 0, 0);
        wasStreaming = false;
      } else if (!wasStreaming && isStreamingActive()) {
        wasStreaming = true;
      }
      
      // Check WiFi status - only check server after we've had a chance to test it
      if (WiFi.status() != WL_CONNECTED) {
        // Connection lost
        if (disconnectStartTime == 0) {
          disconnectStartTime = millis();
          Serial.println("Connection lost - starting disconnect timer");
          if (serverConnected) {
            wasEverDisconnected = true;
          }
        }
        
        serverConnected = false;
        audioPlaying = false; // Stop any ongoing audio
        setLEDColor(255, 0, 0); // Red LED for disconnection
        
        // Try to reconnect to stored networks in background
        if (millis() - disconnectStartTime > 5000) { // Try every 5 seconds
          Serial.println("Attempting background reconnection...");
          if (tryStoredNetworks()) {
            Serial.println("Reconnected to stored network!");
            disconnectStartTime = 0;
            autoProvisioningActive = false;
            currentState = STATE_CONNECTED;
            break;
          }
          // Don't reset timer - let it accumulate to trigger provisioning mode
        }
        
        // After 15 seconds, start auto-provisioning while continuing to try stored networks
        if (millis() - disconnectStartTime > 15000 && !autoProvisioningActive) {
          Serial.println("15s timeout - starting auto-provisioning mode");
          autoProvisioningActive = true;
          startProvisioning();
        }
        
        break;
      } else {
        // Connection is good, reset disconnect timer
        if (disconnectStartTime != 0) {
          Serial.println("Connection restored");
          disconnectStartTime = 0;
          autoProvisioningActive = false;
        }
      }
      
      // Test server connection if not connected
      if (!serverConnected && millis() - lastCheck > 1000) {
        connectToBackend();
        lastCheck = millis();
      }
      break;
      
    case STATE_SERVER_CHECK:
      if (!serverConnected && millis() - lastRetryTime > SERVER_RETRY_DELAY) {
        Serial.println("Retrying server connection...");
        connectToBackend();
        lastRetryTime = millis();
      }
      break;
      
    case STATE_AUDIO_CHECK:
      if (WiFi.status() != WL_CONNECTED || !serverConnected) {
        Serial.println("WiFi/Server disconnected, aborting audio operations");
        currentState = STATE_CONNECTED;
        serverConnected = false;
        break;
      }
      
      if (checkWakeFile()) {
        Serial.println("Wake file exists, starting BigMax alive playback");
        currentState = STATE_AUDIO_PLAY;
      } else {
        Serial.println("Wake file missing or invalid, downloading BigMax alive audio");
        currentState = STATE_AUDIO_DOWNLOAD;
      }
      break;
      
    case STATE_AUDIO_DOWNLOAD:
      if (WiFi.status() != WL_CONNECTED || !serverConnected) {
        Serial.println("WiFi/Server disconnected during download preparation");
        currentState = STATE_CONNECTED;
        serverConnected = false;
        break;
      }
      
      if (downloadWakeFile()) {
        Serial.println("BigMax alive audio downloaded successfully, starting playback");
        currentState = STATE_AUDIO_PLAY;
      } else {
        Serial.println("BigMax alive audio download failed, retrying in 10 seconds");
        currentState = STATE_AUDIO_DOWNLOAD_RETRY;
        lastRetryTime = millis();
      }
      break;
      
    case STATE_AUDIO_DOWNLOAD_RETRY:
      if (WiFi.status() != WL_CONNECTED || !serverConnected) {
        Serial.println("WiFi/Server disconnected during download retry wait");
        currentState = STATE_CONNECTED;
        serverConnected = false;
        break;
      }
      
      if (millis() - lastRetryTime > 10000) { // Wait 10 seconds
        Serial.println("Retrying BigMax alive audio download...");
        currentState = STATE_AUDIO_DOWNLOAD;
      }
      break;
      
    case STATE_AUDIO_PLAY:
      if (WiFi.status() != WL_CONNECTED || !serverConnected) {
        Serial.println("WiFi/Server disconnected, aborting BigMax alive playback");
        audioPlaying = false;
        currentState = STATE_CONNECTED;
        serverConnected = false;
        break;
      }
      
      if (!audioPlaying) {
        if (playWakeFile()) {
          Serial.println("BigMax alive audio completed successfully");
        } else {
          Serial.println("BigMax alive audio playback failed or was aborted");
        }
        
        
        // Return to connected state after playback
        currentState = STATE_CONNECTED;
      }
      break;
      
    case STATE_RECORDING:
      if (micI2SInitialized) {
        captureAudioData();
      }
      break;
      
    case STATE_ENCODING:
      setLEDColor(255, 255, 0); // Yellow for processing
      encodeAndUpload();
      break;
      
    case STATE_UPLOADING:
      // Upload in progress - LED handled in upload function
      break;
      
    default:
      break;
  }
  
  // Update LED status
  updateLEDStatus();
  
  yield(); // Allow background tasks
}

void optimizeTCPForUploads() {
  Serial.println("TCP optimization configured - will apply after WiFi connection");
}

void setupWifi() {
  Serial.println("Attempting to connect to stored networks...");
  
  if (tryStoredNetworks()) {
    Serial.println("Connected to stored network");
    currentState = STATE_CONNECTED;
  } else {
    Serial.println("No stored networks available, starting provisioning...");
    startProvisioning();
  }
}

void handleWifiConnection() {
  static unsigned long connectStart = millis();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected successfully!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    // Apply upload optimizations after connection
    WiFi.setSleep(false);
    Serial.println("WiFi power saving disabled for upload performance");
    
    // Turn LED green and wait 2 seconds
    setLEDColor(0, 255, 0);
    delay(2000);
    
    currentState = STATE_CONNECTED;
    return;
  }
  
  // Check for timeout
  if (millis() - connectStart > WIFI_TIMEOUT) {
    Serial.println("WiFi connection timeout, starting provisioning...");
    setLEDColor(255, 0, 0); // Red LED for WiFi failure
    delay(1000);
    WiFi.disconnect();
    startProvisioning();
    return;
  }
  
  // Still connecting...
  if (millis() % 1000 < 50) { // Print every second
    Serial.print(".");
  }
}

void startProvisioning() {
  Serial.println("Starting provisioning mode...");
  currentState = STATE_PROVISIONING;
  
  // Ensure clean WiFi state - force stop any ongoing connections
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(1000); // Give more time for WiFi to fully stop
  
  // Start Access Point
  WiFi.mode(WIFI_AP);
  delay(500); // Give more time for mode switch
  
  bool apStarted = WiFi.softAP(AP_NAME);
  if (!apStarted) {
    Serial.println("Failed to start AP, retrying...");
    delay(1000);
    WiFi.softAP(AP_NAME);
  }
  
  // Wait for AP to be ready
  delay(1000);
  
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(apIP);
  
  // Start DNS server for captive portal
  dnsServer.start(53, "*", apIP);
  
  // Setup web server routes
  setupWebServer();
  
  // Small delay before starting server
  delay(100);
  server.begin();
  Serial.println("Provisioning server started");
}

void setupWebServer() {
  // Serve main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });
  
  // Handle network scan
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"networks\":[";
    
    int n = WiFi.scanNetworks();
    bool firstNetwork = true;
    for (int i = 0; i < n; i++) {
      // Only include 2.4GHz networks (channels 1-14)
      int channel = WiFi.channel(i);
      if (channel <= 14) {  // 2.4GHz channels are 1-14
        if (!firstNetwork) json += ",";
        json += "{";
        json += "\"ssid\":\"" + WiFi.SSID(i) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"channel\":" + String(channel);
        json += "}";
        firstNetwork = false;
      }
    }
    
    json += "]}";
    request->send(200, "application/json", json);
  });
  
  // Handle credential save
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    String ssid = "";
    String password = "";
    
    if (request->hasParam("ssid", true)) {
      ssid = request->getParam("ssid", true)->value();
    }
    if (request->hasParam("password", true)) {
      password = request->getParam("password", true)->value();
    }
    
    if (ssid.length() > 0) {
      Serial.println("Testing new network credentials...");
      
      // Store temporarily for adding to network list on success
      tempSSID = ssid;
      tempPassword = password;
      
      // Try connection in AP+STA mode to test credentials
      WiFi.mode(WIFI_AP_STA);
      delay(500); // Wait for mode change to complete
      WiFi.begin(ssid.c_str(), password.c_str());
      
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing SSID");
    }
  });
  
  // Status endpoint for connection monitoring
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    String status = "connecting";
    wl_status_t wifiStatus = WiFi.status();
    
    if (wifiStatus == WL_CONNECTED) {
      status = "connected";
      
      // Add this network to stored networks (only once)
      String connectedSSID = WiFi.SSID();
      if (tempSSID == connectedSSID && tempPassword.length() > 0) {
        addNetwork(connectedSSID, tempPassword);
        Serial.printf("Auto-saved new network: %s\n", connectedSSID.c_str());
        tempSSID = "";  // Clear to prevent re-adding
        tempPassword = "";
      }
      
      // Switch to STA mode and update state (but don't close portal here)
      if (currentState != STATE_CONNECTED) {
        WiFi.mode(WIFI_STA);
        currentState = STATE_CONNECTED;
        disconnectStartTime = 0;
        autoProvisioningActive = false;
      }
    } else if (wifiStatus == WL_CONNECT_FAILED) {
      status = "wrong_password";  // Most likely cause of connection failure
      // Reset to AP mode only
      WiFi.disconnect(true);
      delay(500); // Wait for disconnect to complete
      WiFi.mode(WIFI_AP);
    } else if (wifiStatus == WL_NO_SSID_AVAIL) {
      status = "failed";  // Network not found
      // Reset to AP mode only
      WiFi.disconnect(true);
      delay(500); // Wait for disconnect to complete
      WiFi.mode(WIFI_AP);
    } else if (wifiStatus == WL_DISCONNECTED || wifiStatus == WL_CONNECTION_LOST) {
      // Check if it's been too long (timeout)
      static unsigned long connectStartTime = 0;
      if (connectStartTime == 0) {
        connectStartTime = millis();
      }
      if (millis() - connectStartTime > 15000) { // 15 second timeout
        status = "timeout";
        WiFi.disconnect(true);
        delay(500); // Wait for disconnect to complete
        WiFi.mode(WIFI_AP);
        connectStartTime = 0;
      }
    }
    
    request->send(200, "text/plain", status);
  });
  
  // Get stored networks info
  server.on("/networks", HTTP_GET, [](AsyncWebServerRequest *request) {
    int count = 0;
    String networks = "";
    for (int i = 0; i < MAX_NETWORKS; i++) {
      if (storedNetworks[i].isValid) {
        count++;
        if (networks.length() > 0) networks += ", ";
        networks += storedNetworks[i].ssid;
      }
    }
    
    String json = "{\"count\":" + String(count) + ",\"networks\":\"" + networks + "\"}";
    request->send(200, "application/json", json);
  });
  
  // Catch-all for captive portal
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->redirect("/");
  });
}

void connectToBackend() {
  Serial.println("Attempting backend connection...");
  
  // Check WiFi connection first
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping backend test");
    return;
  }
  
  http.begin(String(BACKEND_HOST) + "/");
  http.setTimeout(5000); // Increased to 5 seconds
  http.setConnectTimeout(3000); // 3 second connection timeout
  http.setReuse(false); // Don't reuse connections
  
  Serial.printf("Connecting to: %s/\n", BACKEND_HOST);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    Serial.println("Backend root endpoint accessible");
    http.end();
    
    // Skip the /wakeup endpoint test for faster setup
    // We'll test it during audio download if needed
    Serial.println("Backend connection successful!");
    setLEDColor(0, 255, 0); // Solid green LED for full connectivity
    
    // Close captive portal now that backend is connected
    if (currentState == STATE_PROVISIONING) {
      Serial.println("Closing captive portal - backend connection established");
      server.end();
      dnsServer.stop();
    }
    
    // Check if we should play audio (first time or after disconnection)
    bool shouldPlayAudio = !serverConnected && (wasEverDisconnected || isFirstConnection);
    serverConnected = true;
    
    if (shouldPlayAudio) {
      if (isFirstConnection) {
        Serial.println("First connection - triggering BigMax alive audio");
        isFirstConnection = false;
      } else {
        Serial.println("Connection recovered - triggering BigMax alive audio");
        wasEverDisconnected = false;
      }
      currentState = STATE_AUDIO_CHECK;
      return;
    }
  } else {
    Serial.printf("Backend root endpoint failed: HTTP %d", httpCode);
    if (httpCode < 0) {
      Serial.println(" (Connection/timeout error)");
    } else {
      Serial.println();
    }
    serverConnectionFailed();
  }
  
  http.end();
}

void serverConnectionFailed() {
  Serial.println("Server connection failed, will retry...");
  
  // Set solid red LED for backend failure
  setLEDColor(255, 0, 0);
  
  currentState = STATE_SERVER_CHECK;
  lastRetryTime = millis();
}

// ============================================
// LED DEBUGGING SYSTEM
// ============================================

void setLEDColor(uint8_t red, uint8_t green, uint8_t blue) {
  neopixelWrite(LED_BUILTIN, red, green, blue);
}

// LED Debug Functions
void ledOff() {
  setLEDColor(0, 0, 0);
}

void ledRecording() {
  setLEDColor(0, 0, 255); // Blue solid
}

void ledProcessing() {
  setLEDColor(255, 255, 0); // Yellow solid  
}

void ledAudioPlaying() {
  setLEDColor(0, 255, 0); // Green solid
}

void ledError() {
  setLEDColor(255, 0, 0); // Red solid
}

void ledStartup() {
  // White pulse effect
  for (int i = 0; i < 3; i++) {
    setLEDColor(255, 255, 255);
    delay(200);
    setLEDColor(0, 0, 0);
    delay(200);
  }
}

void ledFlash(uint8_t red, uint8_t green, uint8_t blue, int duration = 150) {
  setLEDColor(red, green, blue);
  delay(duration);
  setLEDColor(0, 0, 0);
}

void ledBlink(uint8_t red, uint8_t green, uint8_t blue, int count, int onTime = 200, int offTime = 200) {
  for (int i = 0; i < count; i++) {
    setLEDColor(red, green, blue);
    delay(onTime);
    setLEDColor(0, 0, 0);
    if (i < count - 1) delay(offTime); // No delay after last blink
  }
}

// Specific Error Patterns
void ledWiFiError() {
  ledBlink(255, 0, 0, 1); // Red 1 blink
}

void ledServerError() {
  ledBlink(255, 0, 0, 2); // Red 2 blinks  
}

void ledUploadError() {
  ledBlink(255, 0, 0, 3); // Red 3 blinks
}

void ledAudioError() {
  ledBlink(128, 0, 128, 1); // Purple 1 blink
}

void ledMemoryError() {
  ledBlink(128, 0, 128, 2); // Purple 2 blinks
}

void ledSystemError() {
  ledBlink(128, 0, 128, 3); // Purple 3 blinks
}

// Status Indicators
void ledSuccessFlash() {
  ledFlash(0, 255, 0); // Green flash
}

void ledProcessingFlash() {
  ledFlash(255, 255, 0); // Yellow flash
}

void ledWarningFlash() {
  ledFlash(255, 128, 0); // Orange flash
}

void updateLEDStatus() {
  // Handle blinking LED for backend checking
  if ((currentState == STATE_CONNECTED || currentState == STATE_SERVER_CHECK) && 
      WiFi.status() == WL_CONNECTED && !serverConnected) {
    // Blink green while checking backend
    if (millis() - lastBlinkTime > 500) { // 500ms blink interval
      blinkState = !blinkState;
      lastBlinkTime = millis();
      if (blinkState) {
        setLEDColor(0, 255, 0); // Green on
      } else {
        setLEDColor(0, 0, 0); // Off
      }
    }
  }
  
  // Handle overflow LED during recording
  if (psramOverflow && currentState == STATE_RECORDING) {
    // Keep orange LED solid during overflow
    static unsigned long lastOverflowBlink = 0;
    if (millis() - lastOverflowBlink > 1000) {
      setLEDColor(255, 128, 0); // Ensure orange stays on
      lastOverflowBlink = millis();
    }
  }
}

// ===========================================
// VOICE RECORDING FUNCTIONS
// ===========================================

void updateButtonState() {
  unsigned long currentTime = millis();
  bool currentButtonValue = digitalRead(BUTTON_PIN) == LOW; // Active low
  
  // Read button every 10ms for responsiveness
  if (currentTime - lastButtonRead < 10) return;
  lastButtonRead = currentTime;
  
  switch (buttonState) {
    case BUTTON_IDLE:
      if (currentButtonValue && !lastButtonValue) {
        // Button pressed - start debounce
        buttonState = BUTTON_PRESSED_DEBOUNCE;
        buttonStateChangeTime = currentTime;
      }
      break;
      
    case BUTTON_PRESSED_DEBOUNCE:
      if (currentTime - buttonStateChangeTime >= DEBOUNCE_TIME_MS) {
        if (currentButtonValue) {
          // Still pressed after debounce - confirmed press
          buttonState = BUTTON_PRESSED;
          onButtonPressed();
        } else {
          // Released during debounce - false trigger
          buttonState = BUTTON_IDLE;
        }
      }
      break;
      
    case BUTTON_PRESSED:
      if (!currentButtonValue && lastButtonValue) {
        // Button released - start debounce
        buttonState = BUTTON_RELEASED_DEBOUNCE;
        buttonStateChangeTime = currentTime;
      }
      break;
      
    case BUTTON_RELEASED_DEBOUNCE:
      if (currentTime - buttonStateChangeTime >= DEBOUNCE_TIME_MS) {
        if (!currentButtonValue) {
          // Still released after debounce - confirmed release
          buttonState = BUTTON_IDLE;
          onButtonReleased();
        } else {
          // Pressed again during debounce - back to pressed
          buttonState = BUTTON_PRESSED;
        }
      }
      break;
  }
  
  lastButtonValue = currentButtonValue;
}

void onButtonPressed() {
  Serial.println("Button pressed - starting recording");
  
  // Only start recording if we're connected and not doing other audio operations
  if (currentState == STATE_CONNECTED && serverConnected && !audioPlaying) {
    startRecording();
  } else {
    Serial.println("Cannot start recording - system busy or disconnected");
  }
}

void onButtonReleased() {
  Serial.println("Button released - stopping recording");
  
  if (currentState == STATE_RECORDING) {
    stopRecording();
  }
}

void startRecording() {
  Serial.println("Initializing recording...");
  
  // Clear any previous buffer data
  audioBuffer.clear();
  totalSamples = 0;
  bufferOverflow = false;
  psramOverflow = false;
  lowPassPrev = 0.0f;  // Reset low-pass filter state
  
  // Enable microphone power
  digitalWrite(MIC_POWER_PIN, HIGH);
  delay(100); // Give mic time to stabilize
  
  // Initialize I2S for recording (separate from speaker I2S)
  if (!initializeMicrophoneI2S()) {
    Serial.println("Failed to initialize microphone I2S");
    digitalWrite(MIC_POWER_PIN, LOW);
    return;
  }
  
  currentState = STATE_RECORDING;
  recordingStartTime = millis();
  setLEDColor(0, 0, 255); // Blue for recording
  
  Serial.println("Recording started...");
}

void stopRecording() {
  Serial.println("Stopping recording...");
  
  // Stop microphone I2S
  if (micI2SInitialized) {
    i2s_driver_uninstall(MIC_I2S_PORT);
    micI2SInitialized = false;
  }
  
  // Turn off microphone power
  digitalWrite(MIC_POWER_PIN, LOW);
  
  unsigned long recordingDuration = millis() - recordingStartTime;
  size_t audioDataSize = totalSamples * sizeof(int16_t);
  float durationSeconds = recordingDuration / 1000.0f;
  float audioSizeKB = audioDataSize / 1024.0f;
  
  Serial.printf("=== RECORDING COMPLETED ===\n");
  Serial.printf("Duration: %.2f seconds\n", durationSeconds);
  Serial.printf("Samples: %d\n", totalSamples);
  Serial.printf("Audio data size: %.2f KB (%d bytes)\n", audioSizeKB, audioDataSize);
  Serial.printf("Sample rate: %d Hz\n", SAMPLE_RATE);
  Serial.printf("Bit depth: %d bits\n", BITS_PER_SAMPLE);
  if (bufferOverflow) {
    Serial.printf("WARNING: PSRAM overflow occurred - some audio may be lost\n");
  }
  Serial.printf("========================\n");
  
  if (totalSamples > 0) {
    currentState = STATE_ENCODING;
  } else {
    Serial.println("No audio data recorded");
    // Return to connected state
    currentState = STATE_CONNECTED;
  }
}

bool initializeMicrophoneI2S() {
  Serial.println("Initializing I2S for microphone...");
  
  i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Mono from INMP441
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SD
  };
  
  esp_err_t result = i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL);
  if (result != ESP_OK) {
    Serial.printf("Microphone I2S driver install failed: %d\n", result);
    return false;
  }
  
  result = i2s_set_pin(MIC_I2S_PORT, &pin_config);
  if (result != ESP_OK) {
    Serial.printf("Microphone I2S pin config failed: %d\n", result);
    i2s_driver_uninstall(MIC_I2S_PORT);
    return false;
  }
  
  // Clear DMA buffers
  i2s_zero_dma_buffer(MIC_I2S_PORT);
  
  micI2SInitialized = true;
  Serial.println("Microphone I2S initialized successfully");
  return true;
}

void captureAudioData() {
  const size_t bufferSize = 512; // Read 512 samples at a time
  int16_t i2sBuffer[bufferSize];
  size_t bytesRead = 0;
  
  esp_err_t result = i2s_read(MIC_I2S_PORT, i2sBuffer, 
                              bufferSize * sizeof(int16_t), 
                              &bytesRead, 10); // 10ms timeout
  
  if (result == ESP_OK && bytesRead > 0) {
    size_t samplesRead = bytesRead / sizeof(int16_t);
    
    // Check PSRAM usage before adding samples
    if (!bufferOverflow && !checkPSRAMSpace(samplesRead)) {
      Serial.println("PSRAM overflow detected - stopping sample capture");
      bufferOverflow = true;
      psramOverflow = true;
      setLEDColor(255, 128, 0); // Orange for overflow
      return;
    }
    
    // Add samples to buffer if not overflowing
    if (!bufferOverflow) {
      try {
        for (size_t i = 0; i < samplesRead; i++) {
          // Apply gain amplification with soft limiting
          float sample = (float)i2sBuffer[i] * RECORDING_GAIN;
          
          // Apply low-pass filter to reduce high frequencies
          lowPassPrev = lowPassPrev + LOW_PASS_ALPHA * (sample - lowPassPrev);
          sample = lowPassPrev;
          
          // Soft limiting using tanh function to prevent harsh clipping
          float threshold = 32767.0f * SOFT_LIMIT_THRESH;
          if (sample > threshold || sample < -threshold) {
            sample = threshold * tanhf(sample / threshold);
          }
          
          // Convert back to int16_t
          audioBuffer.push_back((int16_t)sample);
        }
        totalSamples += samplesRead;
      } catch (std::bad_alloc& e) {
        Serial.println("Memory allocation failed during recording");
        bufferOverflow = true;
        psramOverflow = true;
        setLEDColor(255, 128, 0); // Orange for overflow
      }
    }
    
    // Debug info every second
    if (totalSamples % 16000 == 0) { // Every second at 16kHz
      float seconds = totalSamples / (float)SAMPLE_RATE;
      float currentSizeKB = (totalSamples * sizeof(int16_t)) / 1024.0f;
      size_t freePSRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      float freePSRAMKB = freePSRAM / 1024.0f;
      Serial.printf("Recording: %.1fs | %d samples | %.1f KB | Free PSRAM: %.1f KB\n", 
                    seconds, totalSamples, currentSizeKB, freePSRAMKB);
    }
  }
}

bool checkPSRAMSpace(size_t additionalSamples) {
  size_t totalPSRAM = esp_psram_get_size();
  size_t freePSRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t additionalBytes = additionalSamples * sizeof(int16_t);
  
  if (freePSRAM < additionalBytes) return false;
  
  float usageAfterAddition = 1.0f - ((float)(freePSRAM - additionalBytes) / totalPSRAM);
  
  return usageAfterAddition < MAX_PSRAM_USAGE;
}

void encodeAndUpload() {
  Serial.println("Starting audio encoding and upload...");
  
  if (totalSamples == 0) {
    Serial.println("No audio data to encode");
    currentState = STATE_CONNECTED;
    return;
  }
  
#ifdef USE_ADPCM_COMPRESSION
  // Use ADPCM compression
  if (!initializeADPCMEncoder()) {
    Serial.println("Failed to initialize ADPCM encoder - falling back to WAV");
    uploadWAVFallback();
    return;
  }
  
  if (!encodeToADPCM()) {
    Serial.println("ADPCM encoding failed - falling back to WAV");
    destroyADPCMEncoder();
    uploadWAVFallback();
    return;
  }
  
  // Upload ADPCM encoded data
  currentState = STATE_UPLOADING;
  
  if (uploadADPCMRecording()) {
    // Success - green LED during response playback
    setLEDColor(0, 255, 0);
    Serial.println("ADPCM upload successful - response received and playing");
    // Note: Audio streaming is handled directly in uploadADPCMRecording()
    
    currentState = STATE_CONNECTED;
  } else {
    // Error - blink purple 3 times
    for (int i = 0; i < 3; i++) {
      setLEDColor(128, 0, 128);
      delay(300);
      setLEDColor(0, 0, 0);
      delay(300);
    }
    currentState = STATE_CONNECTED;
  }
  
  
  // Clean up encoder
  destroyADPCMEncoder();
  
#else
  uploadWAVFallback();
#endif
}

bool uploadVoiceRecording() {
  Serial.println("Uploading voice recording...");
  setLEDColor(255, 255, 0); // Yellow during upload
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return false;
  }
  
  unsigned long uploadStartTime = millis();
  
  HTTPClient http;
  http.begin(String(BACKEND_HOST) + "/upload");
  http.addHeader("Content-Type", "audio/wav");
  http.setTimeout(60000); // 60 second timeout for large files
  http.setConnectTimeout(15000); // 15 second connection timeout
  http.setReuse(true); // Enable connection reuse
  http.addHeader("Connection", "keep-alive");
  http.addHeader("User-Agent", "ESP32-PlushieAI/1.0");
  
  // Create simple WAV header
  size_t audioDataSize = totalSamples * sizeof(int16_t);
  size_t wavFileSize = 44 + audioDataSize; // WAV header + data
  
  // Prepare WAV file in memory
  uint8_t* wavBuffer = (uint8_t*)ps_malloc(wavFileSize);
  if (!wavBuffer) {
    Serial.println("Failed to allocate WAV buffer");
    http.end();
    return false;
  }
  
  // Create WAV header
  createWAVHeader(wavBuffer, audioDataSize);
  
  // Copy audio data after header
  memcpy(wavBuffer + 44, audioBuffer.data(), audioDataSize);
  
  float wavSizeKB = wavFileSize / 1024.0f;
  float wavSizeMB = wavFileSize / (1024.0f * 1024.0f);
  
  Serial.printf("=== UPLOAD STARTING ===\n");
  Serial.printf("WAV file size: %.2f KB (%.2f MB)\n", wavSizeKB, wavSizeMB);
  Serial.printf("Audio data: %.2f KB + 44 bytes header\n", audioDataSize / 1024.0f);
  Serial.printf("Uploading to: %s/upload\n", BACKEND_HOST);
  Serial.printf("WiFi RSSI: %d dBm\n", WiFi.RSSI());
  Serial.printf("======================\n");
  
  unsigned long httpStartTime = millis();
  int httpResponseCode = http.POST(wavBuffer, wavFileSize);
  unsigned long httpEndTime = millis();
  
  // Free the buffer
  free(wavBuffer);
  
  unsigned long totalUploadTime = millis() - uploadStartTime;
  unsigned long httpTime = httpEndTime - httpStartTime;
  float uploadTimeSeconds = totalUploadTime / 1000.0f;
  float httpTimeSeconds = httpTime / 1000.0f;
  float uploadSpeedKBps = wavSizeKB / uploadTimeSeconds;
  
  if (httpResponseCode == 200) {
    Serial.printf("=== UPLOAD SUCCESS ===\n");
    Serial.printf("HTTP Response: %d OK\n", httpResponseCode);
    Serial.printf("Uploaded: %.2f KB\n", wavSizeKB);
    Serial.printf("Upload time: %.2f seconds\n", uploadTimeSeconds);
    Serial.printf("HTTP time: %.2f seconds\n", httpTimeSeconds);
    Serial.printf("Upload speed: %.2f KB/s\n", uploadSpeedKBps);
    Serial.printf("======================\n");
    http.end();
    return true;
  } else {
    Serial.printf("=== UPLOAD FAILED ===\n");
    Serial.printf("HTTP Response: %d\n", httpResponseCode);
    Serial.printf("File size attempted: %.2f KB\n", wavSizeKB);
    Serial.printf("Upload time: %.2f seconds\n", uploadTimeSeconds);
    Serial.printf("HTTP time: %.2f seconds\n", httpTimeSeconds);
    if (httpResponseCode > 0) {
      String response = http.getString();
      Serial.printf("Server response: %s\n", response.c_str());
    }
    Serial.printf("==================\n");
    http.end();
    return false;
  }
}

void createWAVHeader(uint8_t* header, size_t audioDataSize) {
  size_t fileSize = 36 + audioDataSize;
  
  // RIFF header
  memcpy(header, "RIFF", 4);
  *((uint32_t*)(header + 4)) = fileSize;
  memcpy(header + 8, "WAVE", 4);
  
  // fmt chunk
  memcpy(header + 12, "fmt ", 4);
  *((uint32_t*)(header + 16)) = 16; // fmt chunk size
  *((uint16_t*)(header + 20)) = 1;  // PCM format
  *((uint16_t*)(header + 22)) = 1;  // mono
  *((uint32_t*)(header + 24)) = SAMPLE_RATE;
  *((uint32_t*)(header + 28)) = SAMPLE_RATE * 2; // byte rate
  *((uint16_t*)(header + 32)) = 2;  // block align
  *((uint16_t*)(header + 34)) = 16; // bits per sample
  
  // data chunk
  memcpy(header + 36, "data", 4);
  *((uint32_t*)(header + 40)) = audioDataSize;
}

// ===========================================
// AUDIO COMPRESSION FUNCTIONS
// ===========================================

#ifdef USE_ADPCM_COMPRESSION

// ADPCM step size table
const int16_t adpcmStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

// ADPCM index adjustment table
const int8_t adpcmIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

// Encode a single 16-bit sample to 4-bit ADPCM
uint8_t encodeADPCMSample(int16_t sample, ADPCMState* state) {
    int16_t step = adpcmStepTable[state->stepIndex];
    int16_t diff = sample - state->prevSample;
    
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    
    if (diff >= step) {
        code |= 4;
        diff -= step;
    }
    if (diff >= step >> 1) {
        code |= 2;
        diff -= step >> 1;
    }
    if (diff >= step >> 2) {
        code |= 1;
    }
    
    // Update predictor
    int16_t diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += step >> 1;
    if (code & 1) diffq += step >> 2;
    
    if (code & 8) {
        state->prevSample -= diffq;
    } else {
        state->prevSample += diffq;
    }
    
    // Clamp predictor
    if (state->prevSample > 32767) state->prevSample = 32767;
    else if (state->prevSample < -32768) state->prevSample = -32768;
    
    // Update step index
    state->stepIndex += adpcmIndexTable[code];
    if (state->stepIndex < 0) state->stepIndex = 0;
    else if (state->stepIndex > 88) state->stepIndex = 88;
    
    return code;
}

bool initializeADPCMEncoder() {
    Serial.println("Initializing ADPCM encoder...");
    
    // Initialize ADPCM state
    adpcmState.prevSample = 0;
    adpcmState.stepIndex = 0; 
    
    // Clear output buffer
    compressedData.clear();
    compressedData.reserve(totalSamples / 4); // ADPCM is 4:1 compression
    
    compressionInitialized = true;
    Serial.println("ADPCM encoder initialized successfully");
    return true;
}

void destroyADPCMEncoder() {
    compressionInitialized = false;
    Serial.println("ADPCM encoder destroyed");
}

bool encodeToADPCM() {
    if (!compressionInitialized || totalSamples == 0) {
        Serial.println("Cannot encode: ADPCM not initialized or no audio data");
        return false;
    }
    
    Serial.println("Starting ADPCM encoding...");
    compressedData.clear();
    
    float originalSizeKB = (totalSamples * sizeof(int16_t)) / 1024.0f;
    Serial.printf("Encoding %d samples (%.2f KB) to ADPCM...\n", totalSamples, originalSizeKB);
    
    // Process samples in pairs (2 samples = 1 byte ADPCM)
    for (size_t i = 0; i < totalSamples; i += 2) {
        uint8_t adpcmByte = 0;
        
        // First sample (lower 4 bits)
        uint8_t code1 = encodeADPCMSample(audioBuffer[i], &adpcmState);
        adpcmByte = code1 & 0x0F;
        
        // Second sample (upper 4 bits)
        if (i + 1 < totalSamples) {
            uint8_t code2 = encodeADPCMSample(audioBuffer[i + 1], &adpcmState);
            adpcmByte |= (code2 & 0x0F) << 4;
        }
        
        compressedData.push_back(adpcmByte);
        
        // Progress update every 1000 samples
        if (i % 1000 == 0) {
            float progress = (float)i / totalSamples * 100.0f;
            Serial.printf("ADPCM progress: %.1f%%\n", progress);
        }
        
        // Yield periodically
        if (i % 500 == 0) {
            yield();
        }
    }
    
    float compressedSizeKB = compressedData.size() / 1024.0f;
    float compressionRatio = originalSizeKB / compressedSizeKB;
    
    Serial.printf("=== ADPCM ENCODING COMPLETED ===\n");
    Serial.printf("Original size: %.2f KB\n", originalSizeKB);
    Serial.printf("Compressed size: %.2f KB\n", compressedSizeKB);
    Serial.printf("Compression ratio: %.1fx\n", compressionRatio);
    Serial.printf("Space saved: %.1f%%\n", (1.0f - 1.0f/compressionRatio) * 100.0f);
    Serial.printf("================================\n");
    
    return true;
}

bool uploadADPCMRecording() {
    Serial.println("Uploading ADPCM recording...");
    setLEDColor(255, 255, 0); // Yellow during upload
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected");
        return false;
    }
    
    // Check WiFi signal strength before upload
    int rssi = WiFi.RSSI();
    if (rssi < -80) {
        Serial.printf("WARNING: Weak WiFi signal (%d dBm) - upload may fail\n", rssi);
    }
    
    if (compressedData.empty()) {
        Serial.println("No ADPCM data to upload");
        return false;
    }
    
    unsigned long uploadStartTime = millis();
    
    HTTPClient http;
    http.begin(String(BACKEND_HOST) + "/upload");
    http.addHeader("Content-Type", "audio/adpcm");
    http.addHeader("X-Sample-Rate", String(SAMPLE_RATE));
    http.addHeader("X-Channels", "1");
    http.addHeader("X-Bits-Per-Sample", "4");
    http.setTimeout(120000); // 120 second timeout for large files
    http.setConnectTimeout(30000); // 30 second connection timeout
    http.setReuse(false); // Disable connection reuse to avoid stale connections
    http.addHeader("User-Agent", "ESP32-PlushieAI/1.0");
    
    size_t adpcmSize = compressedData.size();
    float adpcmSizeKB = adpcmSize / 1024.0f;
    
    Serial.printf("=== ADPCM UPLOAD STARTING ===\n");
    Serial.printf("ADPCM file size: %.2f KB\n", adpcmSizeKB);
    Serial.printf("Sample rate: %d Hz\n", SAMPLE_RATE);
    Serial.printf("WiFi RSSI: %d dBm\n", WiFi.RSSI());
    Serial.printf("Uploading to: %s/upload\n", BACKEND_HOST);
    Serial.printf("=============================\n");
    
    unsigned long httpStartTime = millis();
    int httpResponseCode = http.POST(compressedData.data(), adpcmSize);
    unsigned long httpEndTime = millis();
    
    unsigned long totalUploadTime = millis() - uploadStartTime;
    unsigned long httpTime = httpEndTime - httpStartTime;
    float uploadTimeSeconds = totalUploadTime / 1000.0f;
    float httpTimeSeconds = httpTime / 1000.0f;
    float uploadSpeedKBps = adpcmSizeKB / uploadTimeSeconds;
    
    if (httpResponseCode == 200) {
        Serial.printf("=== ADPCM UPLOAD SUCCESS ===\n");
        Serial.printf("HTTP Response: %d OK\n", httpResponseCode);
        Serial.printf("Uploaded: %.2f KB ADPCM data\n", adpcmSizeKB);
        Serial.printf("Upload time: %.2f seconds\n", uploadTimeSeconds);
        Serial.printf("HTTP time: %.2f seconds\n", httpTimeSeconds);
        Serial.printf("Upload speed: %.2f KB/s\n", uploadSpeedKBps);
        Serial.printf("============================\n");
        
        // Check if response contains audio data
        int contentLength = http.getSize();
        if (contentLength > 0) {
            Serial.printf("Server response contains %d bytes - streaming as audio\n", contentLength);
            
            // Stream the response directly
            if (streamResponseAudio(&http, contentLength)) {
                Serial.println("Response audio streaming started successfully");
            } else {
                Serial.println("Failed to start response audio streaming");
            }
        } else {
            Serial.println("No audio response from server");
        }
        
        http.end();
        
        // Clear compressed data after successful upload to free memory
        compressedData.clear();
        compressedData.shrink_to_fit();
        Serial.println("Compressed data cleared from memory");
        
        return true;
    } else {
        Serial.printf("=== ADPCM UPLOAD FAILED ===\n");
        Serial.printf("HTTP Response: %d\n", httpResponseCode);
        
        // Provide detailed error explanation
        if (httpResponseCode == -1) {
            Serial.println("Error: Connection failed - check WiFi and server");
        } else if (httpResponseCode == -2) {
            Serial.println("Error: Send header failed");
        } else if (httpResponseCode == -3) {
            Serial.println("Error: Send payload failed - connection lost during upload");
        } else if (httpResponseCode == -4) {
            Serial.println("Error: Not connected");
        } else if (httpResponseCode == -5) {
            Serial.println("Error: Connection lost");
        } else if (httpResponseCode == -6) {
            Serial.println("Error: No stream");
        } else if (httpResponseCode == -7) {
            Serial.println("Error: No HTTP server");
        } else if (httpResponseCode == -8) {
            Serial.println("Error: Too less RAM");
        } else if (httpResponseCode == -9) {
            Serial.println("Error: Encoding error");
        } else if (httpResponseCode == -10) {
            Serial.println("Error: Stream write error");
        } else if (httpResponseCode == -11) {
            Serial.println("Error: Read timeout");
        }
        
        Serial.printf("File size attempted: %.2f KB\n", adpcmSizeKB);
        Serial.printf("Upload time: %.2f seconds\n", uploadTimeSeconds);
        Serial.printf("WiFi Status: %d\n", WiFi.status());
        Serial.printf("WiFi RSSI: %d dBm\n", WiFi.RSSI());
        
        if (httpResponseCode > 0) {
            String response = http.getString();
            Serial.printf("Server response: %s\n", response.c_str());
        }
        Serial.printf("===========================\n");
        http.end();
        
        // Clear compressed data after failed upload to free memory
        compressedData.clear();
        compressedData.shrink_to_fit();
        Serial.println("Compressed data cleared from memory");
        
        return false;
    }
}




#endif // USE_ADPCM_COMPRESSION


// WAV upload fallback function
void uploadWAVFallback() {
  currentState = STATE_UPLOADING;
  
  if (uploadVoiceRecording()) {
    setLEDColor(0, 255, 0);
    delay(2000);
    currentState = STATE_CONNECTED;
    setLEDColor(0, 0, 0);
  } else {
    for (int i = 0; i < 3; i++) {
      setLEDColor(128, 0, 128);
      delay(300);
      setLEDColor(0, 0, 0);
      delay(300);
    }
    currentState = STATE_CONNECTED;
  }
}


