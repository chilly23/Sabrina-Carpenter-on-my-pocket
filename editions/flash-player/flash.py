"""Reinstall flash-only player and video using the board's existing partition map."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
manifest = json.loads((root / 'manifest.json').read_text())
for name, digest in manifest['sha256'].items():
    if hashlib.sha256((root / name).read_bytes()).hexdigest() != digest:
        raise RuntimeError(f'Checksum mismatch: {name}')
port = sys.argv[1] if len(sys.argv) > 1 else 'COM10'
sys.exit(subprocess.call([
    sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', port,
    '--baud', '460800', 'write-flash',
    '0x10000', str(root / 'build/SabrinaFlash.ino.bin'),
    '0x610000', str(root / 'video.bin')]))
