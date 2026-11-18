"""dsc_words.py CACHE ADDR N: N 32-bit words at an unslid VM address, and the cache's unslid base."""
import struct, sys
cache = sys.argv[1]
exec(open(__file__.replace('dsc_words.py', 'dsc_whose.py')).read().split("where = None")[0].replace("path, text = sys.argv[1], sys.argv[2].encode()", "path = cache"))
addr, n = int(sys.argv[2], 16), int(sys.argv[3])
print('first mapping (unslid base) %#x' % min(s[0] for s in subs))
d = mem(addr, 4 * n)
print(' '.join('%#010x' % w for w in struct.unpack('<%dI' % n, d)))
