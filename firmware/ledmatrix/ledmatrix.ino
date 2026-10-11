#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

// ---------- CHANGE THESE ----------
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* HOSTNAME      = "ledmatrix";

// Fixed LAN address, so the router's port forward always points at the ESP32.
// Pick a free address on your network, outside your router's DHCP range.
#define USE_STATIC_IP 1               // 0 = get an address from the router (DHCP)
const IPAddress STATIC_IP (192, 168, 1, 50);
const IPAddress GATEWAY   (192, 168, 1, 1);     // your router
const IPAddress SUBNET    (255, 255, 255, 0);
const IPAddress DNS_SERVER(8, 8, 8, 8);
const int   TCP_PORT      = 8443;            // forward this port on your router to the ESP32
// 64 hex characters (32 bytes). Make one with:  openssl rand -hex 32
// Must be identical to DEVICE_KEY on Vercel. It both authenticates and encrypts.
const char* DEVICE_KEY    = "PASTE-64-HEX-CHARACTERS-HERE";
// ----------------------------------

#define LED_PIN       16
#define BUZZER_PIN    17
#define USE_BUZZER    1      // 1 = play sounds when new text arrives, 0 = silent
#define MATRIX_W      32     // two 16x16 panels side by side
#define MAX_TEXT_LEN  100

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  16, 16, 2, 1, LED_PIN,
  NEO_MATRIX_BOTTOM     + NEO_MATRIX_RIGHT +
  NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG +
  NEO_TILE_BOTTOM       + NEO_TILE_RIGHT +
  NEO_TILE_ROWS      + NEO_TILE_PROGRESSIVE,
  NEO_GRB + NEO_KHZ800
);

String currentText = "Starting...";
int textWidth = 0;
int xPos = 0;
bool scrolling = false;
unsigned long lastScroll = 0;
const int scrollInterval = 60;   // ms per pixel step (lower = faster)
bool wasConnected = false;
bool serverStarted = false;
String currentColorHex = "00c8ff";
String statusText = "Starting...";   // what the website shows as "now showing" (UTF-8)
uint16_t* bmp = nullptr;             // picture sent by the website (RGB565, row-major, bmpW x 16)
int bmpW = 0;
bool bitmapMode = false;
int authFailures = 0;
unsigned long lockedUntil = 0;

WiFiServer tcpServer(TCP_PORT);
WiFiClient client;
bool clientActive = false;
unsigned long clientSince = 0;
uint8_t key[32];
uint8_t challenge[16];
const int MAX_BITMAP_W = 800;                         // pixels; keep equal to MAX_BITMAP_W on the website
const size_t MAX_FRAME = 12 + 9 + 400 + (size_t)MAX_BITMAP_W * 16 * 2 + 16;
uint8_t rx[2 + MAX_FRAME];
size_t rxLen = 0;

// ---------- Songs (non-blocking; order must match the website's dropdown) ----------

struct Note { uint16_t freq; uint16_t ms; };   // freq 0 = rest

const Note SONG_BEEP[]     = { {1200, 80} };
const Note SONG_DINGDONG[] = { {659, 450}, {523, 700} };
const Note SONG_TWINKLE[]  = {
  {523,300},{523,300},{784,300},{784,300},{880,300},{880,300},{784,600},
  {698,300},{698,300},{659,300},{659,300},{587,300},{587,300},{523,600} };
const Note SONG_ODE[]      = {
  {659,300},{659,300},{698,300},{784,300},{784,300},{698,300},{659,300},{587,300},
  {523,300},{523,300},{587,300},{659,300},{659,450},{587,150},{587,600} };
const Note SONG_BIRTHDAY[] = {
  {784,300},{784,150},{880,450},{784,450},{1047,450},{988,900},
  {784,300},{784,150},{880,450},{784,450},{1175,450},{1047,900} };

struct Song { const Note* notes; int len; };
#define SONG(a) { a, (int)(sizeof(a) / sizeof(a[0])) }
const Song SONGS[] = {          // index 0 = none
  { nullptr, 0 },
  SONG(SONG_BEEP), SONG(SONG_DINGDONG), SONG(SONG_TWINKLE), SONG(SONG_ODE), SONG(SONG_BIRTHDAY)
};
const int SONG_COUNT = sizeof(SONGS) / sizeof(SONGS[0]);

const Song* playing = nullptr;
int noteIdx = 0;
bool inGap = false;
unsigned long nextNoteAt = 0;

void startSong(int id) {
#if USE_BUZZER
  if (id <= 0 || id >= SONG_COUNT) return;
  playing = &SONGS[id];
  noteIdx = 0;
  inGap = false;
  nextNoteAt = 0;
#endif
}

void updateSong() {
#if USE_BUZZER
  if (!playing || millis() < nextNoteAt) return;
  if (inGap) {                   // short silence between notes so repeats are audible
    inGap = false;
    if (noteIdx >= playing->len) { playing = nullptr; return; }
  } else if (noteIdx > 0) {
    noTone(BUZZER_PIN);
    inGap = true;
    nextNoteAt = millis() + 25;
    return;
  }
  if (noteIdx >= playing->len) { noTone(BUZZER_PIN); playing = nullptr; return; }
  Note n = playing->notes[noteIdx++];
  if (n.freq) tone(BUZZER_PIN, n.freq); else noTone(BUZZER_PIN);
  nextNoteAt = millis() + n.ms;
#endif
}

// ---------- Display ----------

void drawText() {
  matrix.fillScreen(0);
  if (bitmapMode) {
    for (int x = 0; x < MATRIX_W; x++) {              // only the visible columns
      int sx = x - xPos;
      if (sx < 0 || sx >= bmpW) continue;
      for (int y = 0; y < 16; y++) matrix.drawPixel(x, y, bmp[y * bmpW + sx]);
    }
  } else {
    matrix.setCursor(xPos, 4);   // y=4 centers 8px text on 16 rows
    matrix.print(currentText);
  }
  matrix.show();
}

// Sets new text: static + centered if it fits, otherwise scrolls and loops.
bool setText(String t, uint16_t color) {
  String clean = "";
  for (unsigned int i = 0; i < t.length() && clean.length() < MAX_TEXT_LEN; i++) {
    char c = t[i];
    clean += (c >= 32 && c <= 126) ? c : '?';
  }
  clean.trim();
  if (clean.length() == 0) return false;

  currentText = clean;
  statusText = clean;
  bitmapMode = false;
  free(bmp);
  bmp = nullptr;
  matrix.setTextColor(color);
  textWidth = currentText.length() * 6 - 1;   // 5px glyph + 1px spacing
  scrolling = textWidth > MATRIX_W;
  xPos = scrolling ? MATRIX_W : (MATRIX_W - textWidth) / 2;
  drawText();
  return true;
}

// Shows a picture (big-endian RGB565, row-major, w x 16): static + centered if it fits, else scrolls.
bool setBitmap(const uint8_t* px, int w) {
  uint16_t* nb = (uint16_t*)malloc((size_t)w * 16 * sizeof(uint16_t));
  if (!nb) return false;
  for (int i = 0; i < w * 16; i++) nb[i] = ((uint16_t)px[2 * i] << 8) | px[2 * i + 1];
  free(bmp);
  bmp = nb;
  bmpW = w;
  bitmapMode = true;
  textWidth = w;
  scrolling = w > MATRIX_W;
  xPos = scrolling ? MATRIX_W : (MATRIX_W - w) / 2;
  drawText();
  return true;
}

uint16_t DEFAULT_COLOR() { return matrix.Color(0, 200, 255); }

void updateScrolling() {
  if (!scrolling) return;
  if (millis() - lastScroll < scrollInterval) return;
  lastScroll = millis();

  drawText();
  xPos--;
  if (xPos < -textWidth) {
    xPos = MATRIX_W;             // loop: start again from the right edge
  }
}

// ---------- Incoming messages (encrypted TCP) ----------
//
// Protocol (the Vercel site is the client):
//   ESP32 -> site : 16 random bytes (challenge, new for every connection)
//   site -> ESP32 : [2-byte length][12-byte IV | AES-256-GCM ciphertext | 16-byte tag]
//                   key = DEVICE_KEY, AAD = challenge  (so recorded requests can't be replayed)
//   ESP32 -> site : same framing, AAD = challenge + "R"
// Requests: "S" = status, or a binary "B" message:
//   'B' | song(1) | width(2, BE) | textLen(2, BE) | R G B (3) | text (UTF-8) | pixels (width*16*2)
// The website draws the text itself (any language, emoji) and sends it as a picture.

bool parseKey() {
  if (strlen(DEVICE_KEY) != 64) return false;
  for (int i = 0; i < 32; i++) {
    char h[3] = { DEVICE_KEY[2 * i], DEVICE_KEY[2 * i + 1], 0 };
    char* end;
    key[i] = (uint8_t)strtol(h, &end, 16);
    if (*end) return false;
  }
  return true;
}

void closeClient() {
  client.stop();
  clientActive = false;
  rxLen = 0;
}

void authFailed() {
  if (++authFailures >= 5) { lockedUntil = millis() + 60000; authFailures = 0; }   // 5 bad tries: ignore everyone for 1 min
  closeClient();
}

// Returns the reply text for a decrypted request.
String handleRequest(const uint8_t* p, size_t len) {
  if (len == 1 && p[0] == 'S') return currentColorHex + "\n" + statusText;

  if (len >= 9 && p[0] == 'B') {
    int song = p[1];
    int w = (p[2] << 8) | p[3];
    size_t t = ((size_t)p[4] << 8) | p[5];
    if (w < 1 || w > MAX_BITMAP_W || t > 400 || 9 + t + (size_t)w * 32 != len) return "ERR";
    const uint8_t* txt = p + 9;
    if (!setBitmap(txt + t, w)) return "ERR";

    char hex[7];
    snprintf(hex, sizeof(hex), "%02x%02x%02x", p[6], p[7], p[8]);
    currentColorHex = hex;
    statusText = "";
    statusText.concat((const char*)txt, t);
    startSong(song);
    return "OK";
  }
  return "ERR";
}

void sendReply(const String& reply) {
  size_t n = reply.length();
  size_t frameLen = 12 + n + 16;
  uint8_t* out = (uint8_t*)malloc(2 + frameLen);
  if (!out) return;
  uint8_t aad[17];
  memcpy(aad, challenge, 16);
  aad[16] = 'R';

  uint8_t* frame = out + 2;
  esp_fill_random(frame, 12);                                   // fresh IV
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
  mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, frame, 12, aad, sizeof(aad),
                            (const uint8_t*)reply.c_str(), frame + 12, 16, frame + 12 + n);
  mbedtls_gcm_free(&g);

  out[0] = frameLen >> 8;
  out[1] = frameLen & 0xFF;
  client.write(out, 2 + frameLen);
  client.flush();
  free(out);
}

void processFrame(const uint8_t* f, size_t n) {
  size_t ptLen = n - 12 - 16;
  uint8_t* pt = (uint8_t*)malloc(ptLen + 1);
  if (!pt) { closeClient(); return; }
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
  int ret = mbedtls_gcm_auth_decrypt(&g, ptLen, f, 12, challenge, sizeof(challenge),
                                     f + n - 16, 16, f + 12, pt);
  mbedtls_gcm_free(&g);
  if (ret != 0) { free(pt); authFailed(); return; }             // wrong key or tampered/replayed

  authFailures = 0;
  String reply = handleRequest(pt, ptLen);
  free(pt);
  sendReply(reply);
  closeClient();
}

// Called every loop(); never blocks.
void serviceNetwork() {
  WiFiClient incoming = tcpServer.available();
  if (incoming) {
    if (millis() < lockedUntil) {
      incoming.stop();
    } else {
      if (clientActive) closeClient();                          // newest connection wins
      client = incoming;
      client.setNoDelay(true);
      esp_fill_random(challenge, sizeof(challenge));
      client.write(challenge, sizeof(challenge));
      clientActive = true;
      clientSince = millis();
      rxLen = 0;
    }
  }
  if (!clientActive) return;

  if (millis() - clientSince > 10000 || !client.connected()) { closeClient(); return; }

  int avail = client.available();
  if (avail > 0 && rxLen < sizeof(rx)) {
    int r = client.read(rx + rxLen, min((size_t)avail, sizeof(rx) - rxLen));
    if (r > 0) rxLen += r;
  }
  if (rxLen < 2) return;

  size_t n = ((size_t)rx[0] << 8) | rx[1];
  if (n < 12 + 16 + 1 || n > MAX_FRAME) { authFailed(); return; }   // malformed
  if (rxLen >= 2 + n) processFrame(rx + 2, n);
}

// ---------- Setup / loop ----------

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);

  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(5);      // keep low; use an external 5V supply for more
  setText("WiFi...", DEFAULT_COLOR());

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
#if USE_STATIC_IP
  if (!WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS_SERVER)) Serial.println("Static IP config failed");
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  if (!parseKey()) {
    Serial.println("DEVICE_KEY must be exactly 64 hex characters");
    setText("KEY?", matrix.Color(255, 0, 0));   // short so it fits without scrolling
    while (true) delay(1000);
  }
}

void loop() {
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected && !wasConnected) {
    Serial.println("Connected, LAN IP " + WiFi.localIP().toString() + ", port " + String(TCP_PORT));
    setText("Online", DEFAULT_COLOR());
    currentColorHex = "00c8ff";
    if (!serverStarted) {        // start only once WiFi is up
      tcpServer.begin();
      serverStarted = true;
    }
  }
  wasConnected = connected;

  if (serverStarted) serviceNetwork();
  updateScrolling();
  updateSong();
}
