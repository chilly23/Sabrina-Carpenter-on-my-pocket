"""Check that the physical board completes at least two nonempty video loops."""
import re
import sys
import time
from pathlib import Path
import serial

port = serial.Serial()
port.port = sys.argv[1] if len(sys.argv) > 1 else 'COM10'
port.baudrate = 115200
port.timeout = 0.5
port.dtr = False
port.rts = False
port.open()
data = bytearray()
deadline = time.monotonic() + 55
try:
    while time.monotonic() < deadline:
        chunk = port.read(4096)
        data.extend(chunk)
        if chunk:
            print(chunk.decode(errors='replace'), end='', flush=True)
finally:
    port.close()
text = data.decode(errors='replace')
Path(__file__).with_name('playback.log').write_text(text, encoding='utf-8')
loops = re.findall(r'Loop: (\d+) shown, (\d+) skipped, ([\d.]+) fps', text)
passed = len(loops) >= 2 and all(int(shown) > 0 for shown, _, _ in loops)
passed = passed and not any(s in text for s in ['Guru Meditation', 'read failed', 'invalid onboard'])
print('\n' + ('PASS: repeated nonempty playback loops.' if passed else 'FAIL: repeated playback not confirmed.'))
sys.exit(0 if passed else 1)
