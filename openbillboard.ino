/*******************************************************
 * ESP8266 "Open Billboard" – Text + Bitmap via Web UI
 * Board: ESP8266 (e.g., NodeMCU / Wemos D1 mini)
 * OLED: 0.96" SSD1306 I2C on D1 (SCL), D2 (SDA), addr 0x3C
 *
 * Features:
 *  - Connects to Wi-Fi "WIFI_SSID" / "WIFI_PASS"
 *  - Hostname "billboard" + mDNS: http://billboard.local
 *  - Web page shows current display state (Text / Bitmap)
 *  - Text input with markup support for formatting
 *  - Textarea for hex byte data (128x64 / 1024 bytes)
 *  - File upload (.bin raw 1024 bytes or .txt of hex)
 *  - Submit updates display and redirects to home
 *
 * Text Markup:
 *  - <s1>, <s2>, <s3> = text size 1, 2, 3
 *  - <b> = bold (simulated with offset drawing)
 *  - <i> = italic (simulated with skewed drawing)
 *  - <inv> = inverted colors (black on white)
 *  - <n> = normal colors (white on black)
 *  - Newlines with \n or actual line breaks
 *
 * Libraries required:
 *  - Adafruit SSD1306
 *  - Adafruit GFX
 *******************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ====== Wi-Fi credentials & hostname ======
const char* WIFI_SSID     = "Linksys";
const char* WIFI_PASS     = "password";
const char* HOSTNAME      = "billboard";

// ====== OLED settings ======
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1     // -1 for no reset pin on most I2C OLEDs
#define OLED_ADDR     0x3C   // Change to 0x3D if needed
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ====== Web server ======
ESP8266WebServer server(80);

// ====== Billboard state ======
enum DisplayMode { MODE_TEXT = 0, MODE_BITMAP = 1 };
volatile DisplayMode gMode = MODE_TEXT;
String gCurrentText = "Hello from billboard!";
uint8_t gBitmap[SCREEN_WIDTH * SCREEN_HEIGHT / 8] = {0}; // 1024 bytes (128*64/8)

String gLastMessage = ""; // feedback banner shown on the webpage

// ====== Upload buffer state for multipart/form-data ======
bool gUploadActive = false;
size_t gUploadBytes = 0;

// ---- Forward declarations ----
void renderTextToOLED(const String& msg);
void renderBitmapToOLED(const uint8_t* data, size_t len);
void handleRoot();
void handleSubmit();
void handleUpload(); // multipart upload handler
void handleState();
void handleBitmapBin();
void sendRedirectHome();
String htmlPage();
size_t parseHexToBitmap(const String& hexText, uint8_t* outBuf, size_t outCap);
String hexPreview(const uint8_t* data, size_t len, size_t maxBytes);
void safeConnectWiFi();

// =========================================================
// Setup
// =========================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 allocation failed! Check wiring/address.");
    // Don't halt; continue so web UI still works
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Booting billboard...");
    display.display();
  }

  // Wi-Fi STA mode & hostname
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  safeConnectWiFi();

  // mDNS
  if (MDNS.begin(HOSTNAME)) {
    Serial.printf("mDNS started: http://%s.local\n", HOSTNAME);
  } else {
    Serial.println("mDNS failed to start.");
  }

  // Web routes
  server.on("/", HTTP_GET, handleRoot);

  // For form submit (with or without file)
  server.on(
    "/submit",
    HTTP_POST,
    handleSubmit,    // called after upload completes
    handleUpload     // handles file data chunks
  );

  // Simple endpoints to fetch state (optional utilities)
  server.on("/state", HTTP_GET, handleState);
  server.on("/bitmap.bin", HTTP_GET, handleBitmapBin);

  server.begin();
  Serial.println("HTTP server started.");

  // Draw initial text
  renderTextToOLED(gCurrentText);
}

// =========================================================
// Loop
// =========================================================
void loop() {
  server.handleClient();
  MDNS.update();
}

// =========================================================
// Helpers
// =========================================================

void safeConnectWiFi() {
  Serial.printf("Connecting to WiFi SSID: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Show connection progress on OLED, if available
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (display.width() > 0) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.setTextSize(1);
      display.println("Connecting WiFi...");
      display.println(WIFI_SSID);
      display.print("IP: ");
      display.println(WiFi.localIP());
      display.display();
    }
    if (millis() - start > 20000) { // 20s fallback
      Serial.println("\nConnection timeout; retrying...");
      start = millis();
    }
  }
  Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
}

// Text rendering with markup support
void renderTextToOLED(const String& msg) {
  if (display.width() == 0) return; // if init failed

  display.clearDisplay();
  
  // Initial formatting state
  uint8_t textSize = 1;
  bool boldMode = false;
  bool italicMode = false;
  bool invertedMode = false;
  
  int16_t cursorX = 0;
  int16_t cursorY = 0;
  
  String remaining = msg;
  size_t i = 0;
  
  while (i < remaining.length()) {
    // Check for markup tags
    if (remaining.charAt(i) == '<') {
      // Find closing >
      int closePos = remaining.indexOf('>', i);
      if (closePos > i) {
        String tag = remaining.substring(i + 1, closePos);
        tag.toLowerCase();
        tag.trim();
        
        // Process tag
        if (tag == "s1") {
          textSize = 1;
        } else if (tag == "s2") {
          textSize = 2;
        } else if (tag == "s3") {
          textSize = 3;
        } else if (tag == "b") {
          boldMode = true;
        } else if (tag == "/b") {
          boldMode = false;
        } else if (tag == "i") {
          italicMode = true;
        } else if (tag == "/i") {
          italicMode = false;
        } else if (tag == "inv") {
          invertedMode = true;
        } else if (tag == "n" || tag == "/inv") {
          invertedMode = false;
        } else if (tag == "br") {
          // Line break
          cursorX = 0;
          cursorY += 8 * textSize;
        }
        
        // Skip past the tag
        i = closePos + 1;
        continue;
      }
    }
    
    // Check for newline character
    if (remaining.charAt(i) == '\n') {
      cursorX = 0;
      cursorY += 8 * textSize;
      i++;
      continue;
    }
    
    // Check for carriage return (ignore, handle \n for line breaks)
    if (remaining.charAt(i) == '\r') {
      i++;
      continue;
    }
    
    // Get the character to draw
    char ch = remaining.charAt(i);
    
    // Set text properties
    display.setTextSize(textSize);
    if (invertedMode) {
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    }
    
    // Calculate character width (6 pixels per char at size 1)
    int16_t charWidth = 6 * textSize;
    int16_t lineHeight = 8 * textSize;
    
    // Check if we need to wrap to next line
    if (cursorX + charWidth > SCREEN_WIDTH) {
      cursorX = 0;
      cursorY += lineHeight;
    }
    
    // Check if we're off the bottom of the screen
    if (cursorY + lineHeight > SCREEN_HEIGHT) {
      break; // Stop drawing, screen is full
    }
    
    // Draw the character
    if (italicMode) {
      // Simulate italic by drawing character with skew
      // Draw character in vertical strips with horizontal offset
      int16_t charHeight = 8 * textSize;
      for (int16_t row = 0; row < charHeight; row++) {
        // Calculate skew: shift right more at the top
        int16_t skew = (charHeight - row) / 4; // Adjust divisor for more/less slant
        display.setCursor(cursorX + skew, cursorY + row);
        // We need to draw pixel-by-pixel, but Adafruit GFX doesn't expose that easily
        // Alternative: draw the character multiple times with slight offsets
      }
      // Simplified italic: just draw at multiple x-offsets based on text size
      int16_t italicOffset = textSize; // Lean amount
      for (int16_t offset = 0; offset <= italicOffset; offset++) {
        display.setCursor(cursorX + offset, cursorY + (italicOffset - offset) * 2);
        display.write(ch);
      }
    } else {
      display.setCursor(cursorX, cursorY);
      display.write(ch);
    }
    
    // If bold mode, draw character again with 1-pixel offset
    if (boldMode && !italicMode) {
      display.setCursor(cursorX + 1, cursorY);
      display.write(ch);
    }
    
    // Advance cursor
    cursorX += charWidth;
    i++;
  }
  
  display.display();
}

void renderBitmapToOLED(const uint8_t* data, size_t len) {
  if (display.width() == 0) return; // if init failed

  display.clearDisplay();
  // len should be 1024 bytes; if less, draw anyway (drawBitmap reads width/height)
  display.drawBitmap(0, 0, data, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  display.display();
}

String htmlPage() {
  // Build a simple HTML page with inputs
  String html;
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Billboard</title>";
  html += "<style>";
  html += "body{font-family:system-ui,Segoe UI,Arial,sans-serif;max-width:860px;margin:2rem auto;padding:0 1rem;}";
  html += "header{display:flex;align-items:center;justify-content:space-between;}";
  html += ".banner{background:#eef;border:1px solid #99c;padding:.75rem;border-radius:6px;margin-bottom:1rem;}";
  html += "fieldset{border:1px solid #ddd;padding:1rem;border-radius:6px;margin-bottom:1rem;}";
  html += "label{display:block;margin:.5rem 0 .25rem;}";
  html += "input[type=text],textarea{width:100%;padding:.5rem;border:1px solid #bbb;border-radius:4px;font-family:monospace;}";
  html += "textarea{min-height:160px}";
  html += "#textInput{font-family:monospace;min-height:80px;}";
  html += ".row{display:flex;gap:1rem;flex-wrap:wrap}";
  html += ".card{flex:1;border:1px solid #ddd;border-radius:6px;padding:1rem}";
  html += "button{padding:.6rem 1rem;border:1px solid #888;border-radius:4px;background:#fafafa;cursor:pointer}";
  html += "button:hover{background:#f0f0f0}";
  html += ".muted{color:#666;font-size:.9rem}";
  html += ".help{background:#ffe;border:1px solid #cc9;padding:.5rem;border-radius:4px;margin-top:.5rem;font-size:.85rem;}";
  html += ".help code{background:#fff;padding:2px 4px;border-radius:2px;}";
  html += "</style></head><body>";
  html += "<header><h1>Billboard</h1><div class='muted'>http://" + WiFi.localIP().toString() + " • ";
  html += "Hostname: " + String(HOSTNAME) + " • mDNS: http://" + String(HOSTNAME) + ".local</div></header>";

  if (gLastMessage.length()) {
    html += "<div class='banner'><strong>Status:</strong> " + gLastMessage + "</div>";
  }

  // Current state card
  html += "<div class='row'>";
  html += "<div class='card'><h2>Current Display</h2>";
  html += "<p><strong>Mode:</strong> " + String(gMode == MODE_TEXT ? "Text" : "Bitmap") + "</p>";
  if (gMode == MODE_TEXT) {
    html += "<p><strong>Text:</strong> <pre style='white-space:pre-wrap'>" + gCurrentText + "</pre></p>";
  } else {
    html += "<p><strong>Bitmap:</strong> 1024 bytes</p>";
    html += "<p class='muted'>Preview (first 64 bytes): <code>" + hexPreview(gBitmap, sizeof(gBitmap), 64) + "</code></p>";
  }
  html += "<p class='muted'>Note: OLED is 128×64, 1-bit. Raw bitmap = 1024 bytes.</p>";
  html += "</div>";

  // Form card
  html += "<div class='card'><h2>Update Billboard</h2>";
  html += "<form method='POST' action='/submit' enctype='multipart/form-data'>";
  html += "<fieldset><legend>Choose Mode</legend>";
  html += "<label><input type='radio' name='mode' value='text' " + String(gMode == MODE_TEXT ? "checked" : "") + "> Text</label>";
  html += "<label><input type='radio' name='mode' value='bitmap' " + String(gMode == MODE_BITMAP ? "checked" : "") + "> Bitmap (raw)</label>";
  html += "</fieldset>";

  // Text input with markup help
  html += "<label for='textInput'>Text to display (supports markup)</label>";
  html += "<textarea id='textInput' name='textInput' placeholder='Type text for OLED&#10;Use tags like <s2> for size 2&#10;Use <b>bold text</b>&#10;Press Enter for new lines'>" + gCurrentText + "</textarea>";
  html += "<div class='help'><strong>Markup Guide:</strong><br>";
  html += "<code>&lt;s1&gt;</code> <code>&lt;s2&gt;</code> <code>&lt;s3&gt;</code> = Text size 1, 2, or 3 &nbsp;|&nbsp; ";
  html += "<code>&lt;b&gt;text&lt;/b&gt;</code> = Bold &nbsp;|&nbsp; ";
  html += "<code>&lt;i&gt;text&lt;/i&gt;</code> = Italic (skewed) &nbsp;|&nbsp; ";
  html += "<code>&lt;inv&gt;text&lt;/inv&gt;</code> = Inverted (black on white) &nbsp;|&nbsp; ";
  html += "<code>&lt;n&gt;</code> = Normal colors &nbsp;|&nbsp; ";
  html += "<code>&lt;br&gt;</code> or \\n or Enter = New line";
  html += "</div>";

  // Paste area for hex
  html += "<label for='bitmapHex'>Paste hex bytes for 128×64 bitmap (1024 bytes expected)</label>";
  html += "<textarea id='bitmapHex' name='bitmapHex' placeholder='Example: FF 00 AA 55 ... or 0xFF,0x00,0xAA,0x55'></textarea>";
  html += "<p class='muted'>Any non-hex separators are ignored. We parse pairs of hex digits into bytes until 1024 bytes.</p>";

  // File upload
  html += "<label for='bitmapFile'>Upload bitmap file (.bin raw 1024 bytes or .txt of hex)</label>";
  html += "<input id='bitmapFile' name='bitmapFile' type='file' accept='.bin,.txt'>";

  // Submit
  html += "<p style='margin-top:1rem'><button type='submit'>Submit & Refresh</button> ";
  html += "<a href='/'><button type='button'>Refresh</button></a></p>";
  html += "</form>";
  html += "</div></div>";

  html += "<footer class='muted' style='margin-top:2rem'>ESP8266 Billboard • SSD1306 I2C • " + String(WiFi.SSID()) + "</footer>";
  html += "</body></html>";
  return html;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", htmlPage());
}

void handleState() {
  String body = "{";
  body += "\"mode\":" + String((int)gMode) + ",";
  // Basic JSON escaping for quotes and backslashes in text
  String safeText = gCurrentText;
  safeText.replace("\\", "\\\\");
  safeText.replace("\"", "\\\"");
  body += "\"text\":\"" + safeText + "\"";
  body += "}";
  server.send(200, "application/json", body);
}

void handleBitmapBin() {
  // Safest way to send RAM buffer as binary with ESP8266WebServer
  server.sendHeader("Content-Type", "application/octet-stream");
  server.sendHeader("Content-Disposition", "attachment; filename=\"bitmap.bin\"");
  server.setContentLength(sizeof(gBitmap));
  server.send(200, "application/octet-stream", "");
  WiFiClient client = server.client();
  client.write(gBitmap, sizeof(gBitmap));
}

// Called after upload handler completes
void handleSubmit() {
  // Read mode selection
  String mode = server.hasArg("mode") ? server.arg("mode") : "";
  mode.toLowerCase();

  // Read text input
  String text = server.hasArg("textInput") ? server.arg("textInput") : "";

  // Read pasted hex (may be empty)
  String hexText = server.hasArg("bitmapHex") ? server.arg("bitmapHex") : "";

  bool updated = false;
  gLastMessage = "";

  if (mode == "bitmap") {
    // Prefer file upload if present; otherwise parse textarea
    if (gUploadBytes > 0) {
      // gBitmap already filled by handleUpload
      if (gUploadBytes == sizeof(gBitmap)) {
        gMode = MODE_BITMAP;
        renderBitmapToOLED(gBitmap, sizeof(gBitmap));
        gLastMessage = "Bitmap updated from uploaded file (" + String(gUploadBytes) + " bytes).";
        updated = true;
      } else {
        gLastMessage = "Upload size was " + String(gUploadBytes) + " bytes; expected 1024 for 128×64. Using parsed hex (if provided).";
      }
    }

    if (!updated && hexText.length() > 0) {
      size_t n = parseHexToBitmap(hexText, gBitmap, sizeof(gBitmap));
      if (n > 0) {
        gMode = MODE_BITMAP;
        renderBitmapToOLED(gBitmap, sizeof(gBitmap));
        gLastMessage = "Bitmap updated from pasted hex (" + String(n) + " bytes parsed; padded/truncated to 1024).";
        updated = true;
      } else {
        gLastMessage = "No valid hex bytes found in pasted data.";
      }
    }

    if (!updated) {
      gLastMessage = "Bitmap mode selected, but no valid data was provided.";
    }
  } else { // text mode (default)
    if (text.length() == 0) {
      text = "(empty)";
    }
    gCurrentText = text;
    gMode = MODE_TEXT;
    renderTextToOLED(gCurrentText);
    gLastMessage = "Text updated with markup support.";
    updated = true;
  }

  // Reset upload state for next request
  gUploadActive = false;
  gUploadBytes = 0;

  sendRedirectHome();
}

// Handles multipart file upload in chunks
void handleUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    gUploadActive = true;
    gUploadBytes = 0;
    Serial.printf("Upload start: %s, type=%s\n", upload.filename.c_str(), upload.type.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // Append to bitmap buffer (cap at 1024)
    size_t canWrite = 0;
    if (gUploadBytes < sizeof(gBitmap)) {
      canWrite = min((size_t)upload.currentSize, sizeof(gBitmap) - gUploadBytes);
      memcpy(gBitmap + gUploadBytes, upload.buf, canWrite);
      gUploadBytes += canWrite;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    Serial.printf("Upload end: %s, total=%u bytes\n", upload.filename.c_str(), (unsigned)gUploadBytes);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Serial.println("Upload aborted.");
    gUploadActive = false;
    gUploadBytes = 0;
  }
}

void sendRedirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "Redirecting...");
}

// Parse hex pairs from a text blob into bytes.
// Accepts formats like: "FF 00 AA 55" or "0xFF, 0x00" or "ff00aa55"
// Non-hex characters are ignored except we only read consecutive hex pairs.
size_t parseHexToBitmap(const String& hexText, uint8_t* outBuf, size_t outCap) {
  // Extract only [0-9a-fA-F] chars
  String cleaned;
  cleaned.reserve(hexText.length());
  for (size_t i = 0; i < hexText.length(); i++) {
    char c = hexText.charAt(i);
    if ((c >= '0' && c <= '9') ||
        (c >= 'a' && c <= 'f') ||
        (c >= 'A' && c <= 'F')) {
      cleaned += c;
    }
  }

  // Convert hex pairs to bytes
  size_t outIdx = 0;
  auto hexVal = [](char ch) -> uint8_t {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
    if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
    return 0;
  };

  for (size_t i = 0; i + 1 < cleaned.length() && outIdx < outCap; i += 2) {
    uint8_t byteVal = (hexVal(cleaned.charAt(i)) << 4) | hexVal(cleaned.charAt(i + 1));
    outBuf[outIdx++] = byteVal;
  }

  // Pad with zeros if fewer than outCap parsed
  for (size_t j = outIdx; j < outCap; j++) {
    outBuf[j] = 0;
  }
  return outIdx;
}

String hexPreview(const uint8_t* data, size_t len, size_t maxBytes) {
  size_t n = min(len, maxBytes);
  String s;
  for (size_t i = 0; i < n; i++) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%02X", data[i]);
    s += buf;
    if (i + 1 < n) s += " ";
  }
  return s;
}
