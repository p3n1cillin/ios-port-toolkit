// Host check of R5qUsbRamDisk.c's FAT32 formatter and FAT walker (stub UEFI types; no USB).
// usage: test_ramdisk_host format IMG | find IMG NAME.EXT
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32; typedef uint64_t UINT64; typedef uint64_t UINTN;
typedef char CHAR8; typedef uint8_t BOOLEAN; typedef uint64_t EFI_STATUS; typedef uint64_t EFI_LBA; typedef void VOID;
typedef uint64_t EFI_PHYSICAL_ADDRESS; typedef uint16_t CHAR16;
typedef struct { UINT32 a; UINT16 b, c; UINT8 d[8]; } EFI_GUID;
#define EFIAPI
#define STATIC static
#define IN
#define OUT
#define CONST const
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_ERROR(s) ((s) != 0)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define EFI_BLOCK_IO_PROTOCOL_REVISION 0x00010000
#define EFI_SIZE_TO_PAGES(s) (((s) + 4095) / 4096)
typedef struct { UINT32 MediaId; BOOLEAN RemovableMedia, MediaPresent, LogicalPartition, ReadOnly, WriteCaching;
                 UINT32 BlockSize, IoAlign; EFI_LBA LastBlock; } EFI_BLOCK_IO_MEDIA;
typedef struct _EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO_PROTOCOL;
struct _EFI_BLOCK_IO_PROTOCOL { UINT64 Revision; EFI_BLOCK_IO_MEDIA *Media;
  EFI_STATUS (*Reset)(EFI_BLOCK_IO_PROTOCOL *, BOOLEAN);
  EFI_STATUS (*ReadBlocks)(EFI_BLOCK_IO_PROTOCOL *, UINT32, EFI_LBA, UINTN, VOID *);
  EFI_STATUS (*WriteBlocks)(EFI_BLOCK_IO_PROTOCOL *, UINT32, EFI_LBA, UINTN, VOID *);
  EFI_STATUS (*FlushBlocks)(EFI_BLOCK_IO_PROTOCOL *); };
#define CopyMem(d, s, n) memcpy((d), (s), (n))
#define ZeroMem(d, n) memset((d), 0, (n))
#define SetMem(d, n, v) memset((d), (v), (n))
#define CompareMem(a, b, n) memcmp((a), (b), (n))
static void LogSec(const char *s) { fputs(s, stdout); }
static void FbPrintLine(const char *s, UINT32 c) { printf("[screen] %s\n", s); }
static void R5qPersistLog(const char *s) {}
static UINT64 ReadCntPct(void) { return 0; }
static UINT64 ReadCntFrq(void) { return 1; }
static UINTN AsciiStrDecimalToUintn(const char *s) { return strtoull(s, 0, 10); }
static UINTN AsciiStrHexToUintn(const char *s) { return strtoull(s, 0, 16); }
static void AsciiSPrint(char *b, UINTN n, const char *f, ...) {
  char g[256]; size_t j = 0;   // UEFI %a/%lx -> C %s/%llx
  for (size_t i = 0; f[i] && j < sizeof g - 3; i++) {
    if (f[i] == '%' && f[i + 1] == 'a') { g[j++] = '%'; g[j++] = 's'; i++; }
    else if (f[i] == '%' && f[i + 1] == 'l') { g[j++] = '%'; g[j++] = 'l'; g[j++] = 'l'; i++; }
    else g[j++] = f[i];
  }
  g[j] = 0; va_list a; va_start(a, f); vsnprintf(b, n, g, a); va_end(a);
}
static struct {
  EFI_STATUS (*AllocatePages)(int, int, UINTN, EFI_PHYSICAL_ADDRESS *);
  EFI_STATUS (*CalculateCrc32)(VOID *, UINTN, UINT32 *);
  EFI_STATUS (*LocateProtocol)(EFI_GUID *, VOID *, VOID **);
  EFI_STATUS (*SetWatchdogTimer)(UINTN, UINT64, UINTN, CHAR16 *);
} gBSs, *gBS = &gBSs;
#define AllocateMaxAddress 0
#define EfiLoaderData 0
static EFI_STATUS hAlloc(int t, int m, UINTN p, EFI_PHYSICAL_ADDRESS *a) { *a = (UINT64)(uintptr_t)malloc(p * 4096); return *a ? 0 : 9; }
static EFI_STATUS hCrc(VOID *d, UINTN n, UINT32 *c) { *c = (UINT32)crc32(0, d, (uInt)n); return 0; }

#include "R5qUsbRamDisk.c"

int main(int argc, char **argv) {
  int fd = open(argv[2], O_RDWR | O_CREAT, 0644);
  if (ftruncate(fd, R5Q_RD_BYTES)) return 1;
  mRd = mmap(0, R5Q_RD_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  gBSs.AllocatePages = hAlloc; gBSs.CalculateCrc32 = hCrc;
  if (!strcmp(argv[1], "format")) { RdFormat(); printf("formatted, FAT size %u sectors\n", mRdFatSz); }
  else {   // recompute the FAT size from the BPB, then check like the firmware does
    mRdFatSz = Get32(RdPart() + 36);
    UINT32 first, size;
    if (!RdFind("DONE    TXT", &first, &size)) { printf("DONE.TXT not found\n"); return 2; }
    printf("DONE.TXT cluster %u size %u\n", first, size);
    printf("verified %u\n", RdCheckFiles(first, size));
  }
  munmap(mRd, R5Q_RD_BYTES); close(fd);
  return 0;
}
