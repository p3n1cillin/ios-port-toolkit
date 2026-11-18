# tools/ios: reading and patching iOS's own code (host side)

For the iOS side of a port: find where Apple's code decides something about the hardware, and change it with its
code signature intact ([docs/SYSTEM-PATCHING.md](../../docs/SYSTEM-PATCHING.md)). Python 3; `dsc_dis.py`/`dsc_sel.py`
also need `llvm-mc`, `magfit.py` needs numpy. The cache is your own copy from the IPSW for the build you run (the
S10 Lite uses iOS 16.5 20F66, iPhone10,3: `.../20F66__iPhone10,3_6/dyld_shared_cache_arm64`, split into `.01`–`.NN`).
All addresses are **unslid** VM addresses.

| Tool | Does |
| --- | --- |
| `dsc_whose.py CACHE 'text'` | a C string's address, the image whose `__TEXT` holds it, and every adrp+add that reaches it |
| `dsc_strings.py CACHE IMAGE REGEX` | an image's C strings matching REGEX |
| `dsc_sel.py CACHE IMAGE OFFSET COUNT` | disassemble an image's code, with each adrp+ldr/add annotated with the string or selector it reaches |
| `dsc_dis.py CACHE ADDR N` / `dsc_words.py CACHE ADDR N` | N instructions / words at an address |
| `dsc_callers.py CACHE IMAGE ADDR…` | every bl/b in an image to an address |
| `dsc_where.py CACHE SLIDE ADDR…` | which image (and offset) a slid address from a crash log is in |
| `dsc_fileoff.py CACHE ADDR` | which subcache file holds an address, at what offset, and the word there |
| `dsc_cdpatch.py SUBCACHE OFFSET OLD NEW` | the CodeDirectory hash change that must go with changing that word (slot offset, old and new SHA-256) |
| `macho_xref.py FILE 'text'` / `macho_callers.py FILE ADDR…` | the same two searches in a thin arm64 Mach-O (a daemon such as locationd) |
| `macho_patch.py IN OUT OFFSET OLD NEW` | a patched copy of a Mach-O with every CodeDirectory's page hash updated |
| `magfit.py RECORDING [--matrix …]` | the magnetometer's hard-iron offset, units and axis check from a `magrec` recording |

Patches made with these on the S10 Lite (iOS 16.5 20F66):

| What | Where | Old → new |
| --- | --- | --- |
| CoreMotion motion caps: no SPU fusion/compass | cache `.05` 0x3dd8164 (0x1919a8164) + page 3958's hash at 0x6d12fa1 | `mov x9,#0xfeff` → `mov x9,#0xfe47` |
| CoreMotion compass factory: CLCompass6 always | cache `.05` 0x40750e8 (0x191c450e8) + page 4125's hash at 0x6d14481 | `bl 0x1919a0808` → `mov w0,#1` |
| locationd motion caps | file 0x3b6e38 (+ 4 KB page 950's hash) | `mov x9,#0xfeff` → `mov x9,#0xfe47` |
| locationd compass factory | file 0x6e4360 (+ page 1764's hash) | `bl …` → `mov w0,#1` |
