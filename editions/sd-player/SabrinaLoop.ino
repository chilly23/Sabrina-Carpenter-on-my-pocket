/*
  SabrinaLoop - full-circle looping video player
  Board : VIEWE ESP32-S3 Round Dev Board (ESP32-S3-N16R8) with 1.5" / 1.75" AMOLED (CO5300)
          or 1.85" TFT (ST77916). Pick yours with DISPLAY_TYPE below.
  Video : MJPEG file on the SD card, copied to the card's root folder.

  Arduino IDE settings (Tools menu):
    Board            : ESP32S3 Dev Module
    PSRAM            : OPI PSRAM        <-- required, frames are buffered in PSRAM
    Flash Size       : 16MB (128Mb)
    Partition Scheme : 16M Flash (3MB APP/9.9MB FATFS)  (or Default 4MB with spiffs - any is fine)
    USB CDC On Boot  : Enabled          (only needed to see Serial logs)
    Core             : esp32 by Espressif 3.x

  Libraries (Library Manager):
    GFX Library for Arduino  (Arduino_GFX, by moononournation)
    JPEGDEC                  (by Larry Bank / bitbank2)
*/

#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include <stdio.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"

// ---------------------------------------------------------------- CONFIG ----
// 175 = 1.75" AMOLED, 150 = 1.5" AMOLED (same driver, same pins, same code path)
// 185 = 1.85" TFT (ST77916)  - best effort, see notes
#define DISPLAY_TYPE 175

// Brightness 0..255 (AMOLED only)
#define BRIGHTNESS 255

// Playback speed. Must match the fps the video file was encoded with.
#if DISPLAY_TYPE == 185
  #define VIDEO_PATH "/video_360.mjpeg"
  #define VIDEO_FPS  24
#else
  #define VIDEO_PATH "/video.mjpeg"
  #define VIDEO_FPS  20
#endif
// ------------------------------------------------------------------------

// Pins from the board spec / vendor examples
#define LCD_CS   12
#define LCD_SCK  48
#define LCD_D0   13
#define LCD_D1   47
#define LCD_D2   21
#define LCD_D3   14
#define LCD_RST  11
#define LCD_BL   45

#define SD_CLK   39
#define SD_CMD   38
#define SD_D0    40
#define SD_CD    41   // card-detect / DAT3 line; the vendor code passes it as "cd"

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);

#if DISPLAY_TYPE == 185
  #define PANEL_W 360
  #define PANEL_H 360
  Arduino_GFX *gfx = new Arduino_ST77916(bus, LCD_RST, 0 /*rotation*/, true /*IPS*/, PANEL_W, PANEL_H);
#else
  #define PANEL_W 466
  #define PANEL_H 466
  // CO5300 panels start at column 6 (same as the vendor's 0x2A window 6..476)
  Arduino_CO5300 *amoled = new Arduino_CO5300(bus, LCD_RST, 0 /*rotation*/, PANEL_W, PANEL_H, 6 /*col offset*/, 0, 0, 0);
  Arduino_GFX *gfx = amoled;
#endif

// ----------------------------------------------------------- buffers ------
static const size_t RD_SZ   = 32 * 1024;     // SD read chunk
static const size_t JPG_MAX = 256 * 1024;    // largest single JPEG frame we accept

static uint8_t  *rdBuf    = nullptr;         // file chunk (internal RAM)
static uint8_t  *jpgBuf   = nullptr;         // one JPEG frame (PSRAM)
static uint16_t *frameBuf = nullptr;         // decoded RGB565 frame (PSRAM)
static int       imgW = 0, imgH = 0;         // size of the video frames
static int       offX = 0, offY = 0;         // centering offset on the panel

static size_t rdPos = 0, rdLen = 0;
static uint8_t prevByte = 0;

JPEGDEC jpeg;

// ------------------------------------------------------------ helpers -----
static void showMessage(const char *l1, const char *l2 = nullptr) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(l1, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((PANEL_W - w) / 2, PANEL_H / 2 - 20);
  gfx->print(l1);
  if (l2) {
    gfx->getTextBounds(l2, 0, 0, &x1, &y1, &w, &h);
    gfx->setCursor((PANEL_W - w) / 2, PANEL_H / 2 + 10);
    gfx->print(l2);
  }
}

static sdmmc_card_t *sdCard = nullptr;
static esp_err_t sdErr = ESP_OK;

// Same setup as the VIEWE vendor example: SDMMC, 1-bit, internal pull-ups ON.
// (Arduino's SD_MMC class does not enable the pull-ups; this matches the vendor code.)
static bool mountSD(int khz, bool useCd) {
  esp_vfs_fat_sdmmc_mount_config_t mc = {};
  mc.format_if_mount_failed = false;
  mc.max_files = 4;
  mc.allocation_unit_size = 16 * 1024;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = khz;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = (gpio_num_t)SD_CLK;
  slot.cmd = (gpio_num_t)SD_CMD;
  slot.d0  = (gpio_num_t)SD_D0;
  if (useCd) slot.cd = (gpio_num_t)SD_CD;           // exactly like the vendor examples
  else       pinMode(SD_CD, INPUT_PULLUP);          // keep the CD/DAT3 line high so the card stays in SD mode
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  sdErr = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &sdCard);
  Serial.printf("SD mount @%d kHz cd=%d: %s\n", khz, (int)useCd, esp_err_to_name(sdErr));
  return sdErr == ESP_OK;
}

// Pull the next complete JPEG (FFD8 ... FFD9) out of the file into jpgBuf.
// Returns its length, or 0 at end of file / on error.
static size_t readFrame(FILE *f) {
  size_t n = 0;
  bool started = false;
  for (;;) {
    if (rdPos >= rdLen) {
      rdLen = fread(rdBuf, 1, RD_SZ, f);
      rdPos = 0;
      if (rdLen == 0) return 0;                        // EOF
    }
    uint8_t b = rdBuf[rdPos++];
    if (!started) {
      if (prevByte == 0xFF && b == 0xD8) {
        started = true;
        jpgBuf[0] = 0xFF; jpgBuf[1] = 0xD8; n = 2;
      }
    } else {
      if (n >= JPG_MAX) { started = false; n = 0; }    // corrupt / oversized, resync
      else {
        jpgBuf[n++] = b;
        if (prevByte == 0xFF && b == 0xD9) { prevByte = 0; return n; }
      }
    }
    prevByte = b;
  }
}

// JPEGDEC hands us blocks of decoded pixels; copy them into the frame buffer.
static int jpegDraw(JPEGDRAW *p) {
  int w = p->iWidth;                                   // row stride of the source block
  int copyW = w;
  if (p->x + copyW > imgW) copyW = imgW - p->x;
  int copyH = p->iHeight;
  if (p->y + copyH > imgH) copyH = imgH - p->y;
  if (copyW <= 0 || copyH <= 0) return 1;
  for (int r = 0; r < copyH; r++) {
    memcpy(frameBuf + (size_t)(p->y + r) * imgW + p->x,
           p->pPixels + (size_t)r * w,
           copyW * sizeof(uint16_t));
  }
  return 1;
}

// Decode one frame into frameBuf. First call also sizes/allocates the buffer.
static bool decodeFrame(size_t len) {
  if (!jpeg.openRAM(jpgBuf, (int)len, jpegDraw)) return false;
  if (!frameBuf) {
    imgW = jpeg.getWidth();
    imgH = jpeg.getHeight();
    frameBuf = (uint16_t *)heap_caps_malloc((size_t)imgW * imgH * 2, MALLOC_CAP_SPIRAM);
    if (!frameBuf) { jpeg.close(); return false; }
    memset(frameBuf, 0xFF, (size_t)imgW * imgH * 2);
    offX = (PANEL_W - imgW) / 2;
    offY = (PANEL_H - imgH) / 2;
    if (offX < 0 || offY < 0) {                        // video bigger than panel: crop equally
      Serial.println("Video larger than panel - re-encode to panel size!");
    }
    if (offX > 0 || offY > 0) gfx->fillScreen(RGB565_BLACK);
  }
  jpeg.setPixelType(RGB565_BIG_ENDIAN);                // what Arduino_GFX Be-bitmap draw expects
  jpeg.setMaxOutputSize((imgW + 15) / 16 + 1);         // one callback per full MCU row
  int ok = jpeg.decode(0, 0, 0);
  jpeg.close();
  return ok == 1;
}

// ------------------------------------------------------------ main --------
void setup() {
  Serial.begin(115200);

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  if (!gfx->begin()) Serial.println("gfx->begin() failed!");
  gfx->fillScreen(RGB565_BLACK);
#if DISPLAY_TYPE != 185
  amoled->setBrightness(BRIGHTNESS);
#endif

  if (!psramFound()) {
    showMessage("Enable PSRAM", "Tools > OPI PSRAM");
    while (true) delay(1000);
  }

  rdBuf  = (uint8_t *)malloc(RD_SZ);
  jpgBuf = (uint8_t *)heap_caps_malloc(JPG_MAX, MALLOC_CAP_SPIRAM);
  if (!rdBuf || !jpgBuf) {
    showMessage("Out of memory");
    while (true) delay(1000);
  }
}

void loop() {
  static const int  freqs[]  = {40000, 20000, 20000, 10000};
  static const bool useCd[]  = {true,  true,  false, false};
  bool mounted = false;
  for (int i = 0; i < 4 && !mounted; i++) mounted = mountSD(freqs[i], useCd[i]);
  if (!mounted) {
    char code[32];
    snprintf(code, sizeof(code), "%s", esp_err_to_name(sdErr));
    showMessage("No SD card", code);
    delay(2000);
    return;
  }
  sdmmc_card_print_info(stdout, sdCard);

  FILE *f = fopen("/sdcard" VIDEO_PATH, "rb");
  if (!f) {
    Serial.printf("Cannot open /sdcard%s\n", VIDEO_PATH);
    showMessage("Missing file", VIDEO_PATH);
    esp_vfs_fat_sdcard_unmount("/sdcard", sdCard);
    delay(2000);
    return;
  }
  Serial.printf("Playing %s\n", VIDEO_PATH);

  const uint32_t framePeriodUs = 1000000UL / VIDEO_FPS;

  for (;;) {                                           // loop the video forever
    fseek(f, 0, SEEK_SET);
    rdPos = rdLen = 0;
    prevByte = 0;
    uint32_t due = micros();
    uint32_t shown = 0, skipped = 0, t0 = millis();

    for (;;) {
      size_t len = readFrame(f);
      if (len == 0) break;                             // end of file -> restart

      due += framePeriodUs;
      int32_t late = (int32_t)(micros() - due);        // >0 means we are behind schedule

      if (late > (int32_t)framePeriodUs) {             // more than a frame behind: drop this one
        skipped++;
        continue;
      }
      if (!decodeFrame(len)) continue;

      int32_t wait = -(int32_t)(micros() - due);
      if (wait > 2000) delay(wait / 1000);             // keep a steady frame rate

      gfx->draw16bitBeRGBBitmap(offX, offY, frameBuf, imgW, imgH);
      shown++;
    }
    Serial.printf("Loop: %u shown, %u skipped, %.1f fps\n",
                  (unsigned)shown, (unsigned)skipped,
                  shown * 1000.0f / (millis() - t0));
  }
}
