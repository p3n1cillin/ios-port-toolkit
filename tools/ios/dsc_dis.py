"""dsc_dis.py CACHE ADDR N: N instructions at an unslid VM address of a split dyld shared cache, disassembled (llvm-mc)."""
import struct
import subprocess
import sys

cache, addr, n = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3])
out = subprocess.run([sys.executable, __file__.replace('dsc_dis.py', 'dsc_words.py'), cache, '%x' % addr, str(n)],
                     capture_output=True, text=True).stdout.strip().splitlines()[-1]
words = [int(w, 16) for w in out.split()]
text = ' '.join('0x%02x' % b for w in words for b in struct.pack('<I', w))
res = subprocess.run(['llvm-mc', '--disassemble', '-triple=arm64-apple-ios', '-mattr=+v8.3a'], input=text, capture_output=True,
                     text=True).stdout.splitlines()
i = 0
for line in res:
    line = line.strip()
    if not line or line.startswith('.'):
        continue
    print('%x: %s' % (addr + 4 * i, line))
    i += 1
