/*******************************************************
 * ESP8266 "Open Billboard" – Text + Bitmap via Web UI
 * 
 * New Features:
 *  - Dual-color display mode (yellow/blue sections)
 *  - Scroll and blink animations
 *  - Title/body text separation for dual-color displays
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
#define OLED_RESET    -1
#define OLED_ADDR     0x3C
#define YELLOW_HEIGHT 16     // Top 16 pixels are yellow on dual-color displays
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

ESP8266WebServer server(80);

// ====== Billboard state ======
enum DisplayMode { MODE_TEXT = 0, MODE_BITMAP = 1 };
volatile DisplayMode gMode = MODE_TEXT;
String gCurrentText = "Hello from billboard!";
String gTitleText = "";
bool gDualColorMode = false;
bool gLargeTitleMode = false;
uint8_t gBitmap[1024] = {0};
String gLastMessage = "";

// ====== Upload state ======
bool gUploadActive = false;
size_t gUploadBytes = 0;

// ====== Animation state ======
unsigned long gLastAnimUpdate = 0;
int16_t gScrollOffset = 0;
bool gBlinkState = false;

// ---- Forward declarations ----
void renderTextToOLED(const String& msg);
void renderDualColorToOLED(const String& title, const String& body, bool largeTitle);
void renderTextAtPosition(const String& msg, int16_t startX, int16_t startY, int16_t maxW, int16_t maxH);
void renderBitmapToOLED(const uint8_t* data, size_t len);
void handleRoot();
void handleSubmit();
void handleUpload();
void handleState();
void handleBitmapBin();
void sendRedirectHome();
String htmlPage();
size_t parseHexToBitmap(const String& hexText, uint8_t* outBuf, size_t outCap);
String hexPreview(const uint8_t* data, size_t len, size_t maxBytes);
void safeConnectWiFi();
void updateAnimations();

void setup() {
  Serial.begin(115200);
  delay(100);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 allocation failed!");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Booting...");
    display.display();
  }

  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  safeConnectWiFi();

  if (MDNS.begin(HOSTNAME)) {
    Serial.printf("mDNS: http://%s.local\n", HOSTNAME);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/submit", HTTP_POST, handleSubmit, handleUpload);
  server.on("/state", HTTP_GET, handleState);
  server.on("/bitmap.bin", HTTP_GET, handleBitmapBin);

  server.begin();
  Serial.println("HTTP server started.");

  if (gDualColorMode) {
    renderDualColorToOLED(gTitleText, gCurrentText, gLargeTitleMode);
  } else {
    renderTextToOLED(gCurrentText);
  }
}

void loop() {
  server.handleClient();
  MDNS.update();
  updateAnimations();
}

void safeConnectWiFi() {
  Serial.printf("Connecting to %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nTimeout, retrying...");
      start = millis();
    }
  }
  Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
}

void updateAnimations() {
  unsigned long now = millis();
  if (now - gLastAnimUpdate > 200) {
    gLastAnimUpdate = now;
    gScrollOffset = (gScrollOffset + 2) % (SCREEN_WIDTH + 100);
    gBlinkState = !gBlinkState;
    
    if (gMode == MODE_TEXT) {
      bool needsRedraw = false;
      if (gDualColorMode) {
        if (gTitleText.indexOf("<scroll>") >= 0 || gTitleText.indexOf("<blink>") >= 0 ||
            gCurrentText.indexOf("<scroll>") >= 0 || gCurrentText.indexOf("<blink>") >= 0) {
          needsRedraw = true;
        }
      } else {
        if (gCurrentText.indexOf("<scroll>") >= 0 || gCurrentText.indexOf("<blink>") >= 0) {
          needsRedraw = true;
        }
      }
      
      if (needsRedraw) {
        if (gDualColorMode) {
          renderDualColorToOLED(gTitleText, gCurrentText, gLargeTitleMode);
        } else {
          renderTextToOLED(gCurrentText);
        }
      }
    }
  }
}

void renderDualColorToOLED(const String& title, const String& body, bool largeTitle) {
  if (display.width() == 0) return;
  
  display.clearDisplay();
  
  // Render title in yellow section
  if (title.length() > 0) {
    uint8_t titleSize = largeTitle ? 2 : 1;
    int16_t titleHeight = 8 * titleSize;
    
    if (titleHeight > YELLOW_HEIGHT) {
      titleSize = 1;
      titleHeight = 8;
    }
    
    display.setTextSize(titleSize);
    display.setTextColor(SSD1306_WHITE);
    
    String cleanTitle = title;
    cleanTitle.replace("<scroll>", "");
    cleanTitle.replace("</scroll>", "");
    cleanTitle.replace("<blink>", "");
    cleanTitle.replace("</blink>", "");
    
    bool scrollTitle = title.indexOf("<scroll>") >= 0;
    int16_t xPos = scrollTitle ? (SCREEN_WIDTH - gScrollOffset) : 0;
    
    bool blinkTitle = title.indexOf("<blink>") >= 0;
    if (!blinkTitle || gBlinkState) {
      display.setCursor(xPos, (YELLOW_HEIGHT - titleHeight) / 2);
      display.print(cleanTitle);
    }
  }
  
  // Render body in blue section
  if (body.length() > 0) {
    renderTextAtPosition(body, 0, YELLOW_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT - YELLOW_HEIGHT);
  }
  
  display.display();
}

void renderTextAtPosition(const String& msg, int16_t startX, int16_t startY, int16_t maxW, int16_t maxH) {
  if (display.width() == 0) return;
  
  uint8_t textSize = 1;
  bool boldMode = false;
  bool italicMode = false;
  bool invertedMode = false;
  bool scrollMode = false;
  bool blinkMode = false;
  
  int16_t cursorX = startX;
  int16_t cursorY = startY;
  
  String remaining = msg;
  size_t i = 0;
  
  while (i < remaining.length()) {
    if (remaining.charAt(i) == '<') {
      int closePos = remaining.indexOf('>', i);
      if (closePos > i) {
        String tag = remaining.substring(i + 1, closePos);
        tag.toLowerCase();
        tag.trim();
        
        if (tag == "s1") textSize = 1;
        else if (tag == "s2") textSize = 2;
        else if (tag == "s3") textSize = 3;
        else if (tag == "b") boldMode = true;
        else if (tag == "/b") boldMode = false;
        else if (tag == "i") italicMode = true;
        else if (tag == "/i") italicMode = false;
        else if (tag == "inv") invertedMode = true;
        else if (tag == "n" || tag == "/inv") invertedMode = false;
        else if (tag == "scroll") scrollMode = true;
        else if (tag == "/scroll") scrollMode = false;
        else if (tag == "blink") blinkMode = true;
        else if (tag == "/blink") blinkMode = false;
        else if (tag == "br") {
          cursorX = startX;
          cursorY += 8 * textSize;
        }
        
        i = closePos + 1;
        continue;
      }
    }
    
    if (remaining.charAt(i) == '\n') {
      cursorX = startX;
      cursorY += 8 * textSize;
      i++;
      continue;
    }
    
    if (remaining.charAt(i) == '\r') {
      i++;
      continue;
    }
    
    if (blinkMode && !gBlinkState) {
      i++;
      continue;
    }
    
    char ch = remaining.charAt(i);
    
    display.setTextSize(textSize);
    if (invertedMode) {
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    }
    
    int16_t charWidth = 6 * textSize;
    int16_t lineHeight = 8 * textSize;
    
    int16_t drawX = cursorX;
    if (scrollMode) {
      drawX = cursorX - gScrollOffset;
      if (drawX < -charWidth) {
        drawX += maxW + 100;
      }
    }
    
    if (cursorX + charWidth > startX + maxW) {
      cursorX = startX;
      cursorY += lineHeight;
    }
    
    if (cursorY + lineHeight > startY + maxH) {
      break;
    }
    
    if (drawX >= -charWidth && drawX < maxW) {
      if (italicMode) {
        int16_t italicOffset = textSize;
        for (int16_t offset = 0; offset <= italicOffset; offset++) {
          display.setCursor(drawX + offset, cursorY + (italicOffset - offset) * 2);
          display.write(ch);
        }
      } else {
        display.setCursor(drawX, cursorY);
        display.write(ch);
      }
      
      if (boldMode && !italicMode) {
        display.setCursor(drawX + 1, cursorY);
        display.write(ch);
      }
    }
    
    cursorX += charWidth;
    i++;
  }
}

void renderTextToOLED(const String& msg) {
  if (display.width() == 0) return;
  
  display.clearDisplay();
  renderTextAtPosition(msg, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  display.display();
}

void renderBitmapToOLED(const uint8_t* data, size_t len) {
  if (display.width() == 0) return;

  display.clearDisplay();
  display.drawBitmap(0, 0, data, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  display.display();
}

String htmlPage() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Billboard</title><style>";
  html += "body{font-family:system-ui,sans-serif;max-width:860px;margin:2rem auto;padding:0 1rem}";
  html += ".banner{background:#eef;border:1px solid #99c;padding:.75rem;border-radius:6px;margin-bottom:1rem}";
  html += "fieldset{border:1px solid #ddd;padding:1rem;border-radius:6px;margin-bottom:1rem}";
  html += "label{display:block;margin:.5rem 0 .25rem}";
  html += "input[type=text],textarea{width:100%;padding:.5rem;border:1px solid #bbb;border-radius:4px;font-family:monospace}";
  html += "textarea{min-height:100px}";
  html += ".row{display:flex;gap:1rem;flex-wrap:wrap}";
  html += ".card{flex:1;border:1px solid #ddd;border-radius:6px;padding:1rem}";
  html += "button{padding:.6rem 1rem;border:1px solid #888;border-radius:4px;background:#fafafa;cursor:pointer}";
  html += "button:hover{background:#f0f0f0}";
  html += ".muted{color:#666;font-size:.9rem}";
  html += ".help{background:#ffe;border:1px solid #cc9;padding:.5rem;border-radius:4px;margin-top:.5rem;font-size:.85rem}";
  html += ".help code{background:#fff;padding:2px 4px;border-radius:2px}";
  html += ".preview{background:#000;color:#fff;font-family:monospace;padding:0;border-radius:4px;min-height:80px}";
  html += ".preview-yellow{background:#fc0;color:#000;padding:.5rem;font-weight:bold;min-height:20px}";
  html += ".preview-blue{background:#000;color:#0cf;padding:.5rem}";
  html += ".dual-options{display:none;margin-top:1rem;padding:1rem;background:#f9f9f9;border-radius:4px}";
  html += "</style>";
  html += "<script>function toggleDual(){var c=document.getElementById('dualCheck');";
  html += "document.getElementById('dualOpts').style.display=c.checked?'block':'none'}</script>";
  html += "</head><body>";
  html += "<h1>Billboard</h1>";
  html += "<p class='muted'>" + WiFi.localIP().toString() + " • " + String(HOSTNAME) + ".local</p>";

  if (gLastMessage.length()) {
    html += "<div class='banner'><strong>Status:</strong> " + gLastMessage + "</div>";
  }

  html += "<div class='row'><div class='card'><h2>Current Display</h2>";
  if (gMode == MODE_TEXT) {
    if (gDualColorMode) {
      html += "<div class='preview'>";
      html += "<div class='preview-yellow'>" + (gTitleText.length() > 0 ? gTitleText : "(no title)") + "</div>";
      html += "<div class='preview-blue'>" + gCurrentText + "</div></div>";
    } else {
      html += "<pre style='white-space:pre-wrap'>" + gCurrentText + "</pre>";
    }
  } else {
    html += "<p>Bitmap: 1024 bytes</p>";
    html += "<p class='muted'>" + hexPreview(gBitmap, sizeof(gBitmap), 64) + "</p>";
  }
  html += "</div>";

  html += "<div class='card'><h2>Update</h2>";
  html += "<form method='POST' action='/submit' enctype='multipart/form-data'>";
  html += "<fieldset><legend>Mode</legend>";
  html += "<label><input type='radio' name='mode' value='text' " + String(gMode == MODE_TEXT ? "checked" : "") + "> Text</label>";
  html += "<label><input type='radio' name='mode' value='bitmap' " + String(gMode == MODE_BITMAP ? "checked" : "") + "> Bitmap</label>";
  html += "</fieldset>";

  html += "<label><input type='checkbox' id='dualCheck' name='dualColorMode' value='1' onchange='toggleDual()' ";
  html += String(gDualColorMode ? "checked" : "") + "> Dual-color mode (yellow/blue)</label>";
  
  html += "<div id='dualOpts' class='dual-options' style='display:" + String(gDualColorMode ? "block" : "none") + "'>";
  html += "<label>Title (yellow, top 16px)</label>";
  html += "<input name='titleInput' type='text' value='" + gTitleText + "'>";
  html += "<label><input type='checkbox' name='largeTitleMode' value='1' " + String(gLargeTitleMode ? "checked" : "") + "> Large title</label>";
  html += "</div>";

  html += "<label>" + String(gDualColorMode ? "Body (blue section)" : "Text") + "</label>";
  html += "<textarea name='textInput'>" + gCurrentText + "</textarea>";
  html += "<div class='help'><strong>Markup:</strong> ";
  html += "<code>&lt;s1&gt;</code> <code>&lt;s2&gt;</code> <code>&lt;s3&gt;</code> = Size &nbsp;|&nbsp; ";
  html += "<code>&lt;b&gt;</code> = Bold &nbsp;|&nbsp; ";
  html += "<code>&lt;i&gt;</code> = Italic &nbsp;|&nbsp; ";
  html += "<code>&lt;inv&gt;</code> = Inverted &nbsp;|&nbsp; ";
  html += "<code>&lt;scroll&gt;</code> = Scroll &nbsp;|&nbsp; ";
  html += "<code>&lt;blink&gt;</code> = Blink";
  html += "</div>";

  html += "<label>Hex bytes (1024)</label>";
  html += "<textarea name='bitmapHex' placeholder='FF 00 AA...'></textarea>";

  html += "<label>Upload .bin</label>";
  html += "<input name='bitmapFile' type='file' accept='.bin,.txt'>";

  html += "<p><button type='submit'>Submit</button> ";
  html += "<a href='/'><button type='button'>Refresh</button></a></p>";
  html += "</form></div></div>";

  html += "</body></html>";
  return html;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", htmlPage());
}

void handleState() {
  String body = "{\"mode\":" + String((int)gMode);
  body += ",\"dualColorMode\":" + String(gDualColorMode ? "true" : "false");
  String safeText = gCurrentText;
  safeText.replace("\\", "\\\\");
  safeText.replace("\"", "\\\"");
  body += ",\"text\":\"" + safeText + "\"";
  String safeTitle = gTitleText;
  safeTitle.replace("\\", "\\\\");
  safeTitle.replace("\"", "\\\"");
  body += ",\"title\":\"" + safeTitle + "\"}";
  server.send(200, "application/json", body);
}

void handleBitmapBin() {
  server.sendHeader("Content-Type", "application/octet-stream");
  server.sendHeader("Content-Disposition", "attachment; filename=\"bitmap.bin\"");
  server.setContentLength(sizeof(gBitmap));
  server.send(200, "application/octet-stream", "");
  WiFiClient client = server.client();
  client.write(gBitmap, sizeof(gBitmap));
}

void handleSubmit() {
  String mode = server.hasArg("mode") ? server.arg("mode") : "";
  mode.toLowerCase();

  gDualColorMode = server.hasArg("dualColorMode");
  gLargeTitleMode = server.hasArg("largeTitleMode");
  
  String title = server.hasArg("titleInput") ? server.arg("titleInput") : "";
  String text = server.hasArg("textInput") ? server.arg("textInput") : "";
  String hexText = server.hasArg("bitmapHex") ? server.arg("bitmapHex") : "";

  bool updated = false;
  gLastMessage = "";

  if (mode == "bitmap") {
    if (gUploadBytes > 0) {
      if (gUploadBytes == sizeof(gBitmap)) {
        gMode = MODE_BITMAP;
        renderBitmapToOLED(gBitmap, sizeof(gBitmap));
        gLastMessage = "Bitmap from file (" + String(gUploadBytes) + " bytes)";
        updated = true;
      } else {
        gLastMessage = "Wrong size: " + String(gUploadBytes) + " bytes";
      }
    }

    if (!updated && hexText.length() > 0) {
      size_t n = parseHexToBitmap(hexText, gBitmap, sizeof(gBitmap));
      if (n > 0) {
        gMode = MODE_BITMAP;
        renderBitmapToOLED(gBitmap, sizeof(gBitmap));
        gLastMessage = "Bitmap from hex (" + String(n) + " bytes)";
        updated = true;
      }
    }
  } else {
    if (text.length() == 0) text = "(empty)";
    gCurrentText = text;
    gTitleText = title;
    gMode = MODE_TEXT;
    
    if (gDualColorMode) {
      renderDualColorToOLED(gTitleText, gCurrentText, gLargeTitleMode);
      gLastMessage = "Dual-color updated";
    } else {
      renderTextToOLED(gCurrentText);
      gLastMessage = "Text updated";
    }
    updated = true;
  }

  gUploadActive = false;
  gUploadBytes = 0;

  sendRedirectHome();
}

void handleUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    gUploadActive = true;
    gUploadBytes = 0;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    size_t canWrite = 0;
    if (gUploadBytes < sizeof(gBitmap)) {
      canWrite = min((size_t)upload.currentSize, sizeof(gBitmap) - gUploadBytes);
      memcpy(gBitmap + gUploadBytes, upload.buf, canWrite);
      gUploadBytes += canWrite;
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    gUploadActive = false;
    gUploadBytes = 0;
  }
}

void sendRedirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "Redirecting...");
}

size_t parseHexToBitmap(const String& hexText, uint8_t* outBuf, size_t outCap) {
  String cleaned;
  cleaned.reserve(hexText.length());
  for (size_t i = 0; i < hexText.length(); i++) {
    char c = hexText.charAt(i);
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
      cleaned += c;
    }
  }

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
