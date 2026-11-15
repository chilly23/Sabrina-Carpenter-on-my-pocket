"""Observe the real SD mount and playback path on COM10. Does not write the card."""
import sys
import time
from pathlib import Path
import serial

seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 25
port = serial.Serial()
port.port = 'COM10'
port.baudrate = 115200
port.timeout = 0.5
port.dtr = False
port.rts = False
port.open()
data = bytearray()
deadline = time.monotonic() + seconds
try:
    while time.monotonic() < deadline:
        chunk = port.read(4096)
        data.extend(chunk)
        if chunk:
            print(chunk.decode(errors='replace'), end='', flush=True)
finally:
    port.close()
text = data.decode(errors='replace')
Path(__file__).with_name('sd-live-check.log').write_text(text, encoding='utf-8')
if 'Playing /video.mjpeg' in text or 'Loop:' in text:
    print('\nPASS: card read and playback reached.')
    sys.exit(0)
if 'ESP_ERR_TIMEOUT' in text or 'ESP_ERR_NOT_FOUND' in text:
    print('\nFAIL: SD initialization still fails.')
    sys.exit(1)
print('\nINCONCLUSIVE: no mount/playback evidence captured.')
sys.exit(2)
