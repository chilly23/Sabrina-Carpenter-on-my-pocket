"""Build the flash-only sketch and raw video image from the original SD release."""
from pathlib import Path
import struct

root = Path(__file__).resolve().parents[1]
dest = root / 'editions/flash-player'
sketch = dest / 'SabrinaFlash'
sketch.mkdir(parents=True, exist_ok=True)
src = (root / 'editions/sd-player/SabrinaLoop.ino').read_text()
src = src.replace('Video : MJPEG file on the SD card, copied to the card\'s root folder.',
                  'Video : MJPEG stored in the onboard ffat partition; no SD card needed.')
src = src.replace('(or Default 4MB with spiffs - any is fine)', '(required for onboard video)')
src = src.replace('#include "esp_vfs_fat.h"\n#include "sdmmc_cmd.h"\n#include "driver/sdmmc_host.h"', '#include "esp_partition.h"')
start = src.index('#define SD_CLK')
end = src.index('Arduino_DataBus', start)
src = src[:start] + src[end:]
start = src.index('static sdmmc_card_t')
end = src.index('// Pull the next complete JPEG', start)
src = src[:start] + '''// Raw video occupies the existing ffat data partition. Do not mount/format it.
static const esp_partition_t *videoPartition = nullptr;
static uint32_t videoSize = 0, videoPos = 0;
static const uint32_t HEADER_SIZE = 16;

static bool openVideo() {
  videoPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                             ESP_PARTITION_SUBTYPE_ANY, "ffat");
  uint8_t header[HEADER_SIZE];
  if (!videoPartition || esp_partition_read(videoPartition, 0, header, sizeof(header)) != ESP_OK)
    return false;
  if (memcmp(header, "SABMJPG1", 8) != 0) return false;
  memcpy(&videoSize, header + 8, sizeof(videoSize));
  if (!videoSize || videoSize > videoPartition->size - HEADER_SIZE) return false;
  Serial.printf("Onboard video: %lu bytes at 0x%lx\\n",
                (unsigned long)videoSize, (unsigned long)videoPartition->address);
  return true;
}

''' + src[end:]
src = src.replace('static size_t readFrame(FILE *f)', 'static size_t readFrame()')
src = src.replace('rdLen = fread(rdBuf, 1, RD_SZ, f);', '''rdLen = min((size_t)(videoSize - videoPos), RD_SZ);
      if (rdLen && esp_partition_read(videoPartition, HEADER_SIZE + videoPos, rdBuf, rdLen) != ESP_OK) {
        Serial.println("Onboard video read failed");
        rdLen = 0;
      }
      videoPos += rdLen;''')
start = src.index('  static const int  freqs[]')
end = src.index('  const uint32_t framePeriodUs', start)
src = src[:start] + '''  if (!openVideo()) {
    showMessage("Missing flash video", "Upload video.bin");
    Serial.println("Missing or invalid onboard video image");
    delay(2000);
    return;
  }
  Serial.println("Playing video from onboard flash (SD not used)");

''' + src[end:]
src = src.replace('fseek(f, 0, SEEK_SET);', 'videoPos = 0;')
src = src.replace('readFrame(f)', 'readFrame()').replace('// SD read chunk', '// flash read chunk')
(sketch / 'SabrinaFlash.ino').write_text(src)
video = (root / 'media/video.mjpeg').read_bytes()
assert len(video) + 16 < 10112 * 1024
(dest / 'video.bin').write_bytes(b'SABMJPG1' + struct.pack('<II', len(video), 0) + video)
print(f'Created flash sketch and {len(video)}-byte video image in {dest}')
