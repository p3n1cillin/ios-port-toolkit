//
// r5qfull (Test 45+): a minimal fastboot device on Qualcomm's EFI_USBFN_IO_PROTOCOL, inside the full firmware's boot
// manager. It lets the PC push large images (the iOS kernel cache, ramdisk and system volume) into the phone's RAM
// with the stock Android platform-tools fastboot.exe.
//
// Windows binds its inbox WinUSB driver by itself through MS OS 2.0 descriptors (BOS platform capability plus the
// vendor request), with the Android USB interface GUID that fastboot.exe enumerates, so no driver is installed.
//
// Included into FirstBootManager.c (uses its LogSec, FbPrintLine, R5qPersistLog, ReadCntPct and ReadCntFrq).
// Every setup packet that reaches the client is answered here; the controller driver does SET_ADDRESS and the status
// stages (as Mu-Silicium's open UsbfnDwc3Dxe for the same protocol does). Logs everything to the RAM ring.
//

#define R5Q_USB_VID          0x18D1U     // Google: the vendor ID fastboot implementations use
#define R5Q_USB_PID          0xD00DU     // fastboot
#define R5Q_MSOS_VENDOR      0x20U
#define R5Q_FB_MAX_DOWNLOAD  0x40000000U // Test 45: 1 GiB
#define R5Q_FB_CHUNK         0x00400000U // 4 MiB per bulk receive

// Qualcomm's EFI_USBFN_IO_PROTOCOL (revision 0x00010003) = the UEFI standard members + a SuperSpeed configure call
// (QcomPkg EFIUsbfnIo.h; that header clashes with MdePkg's newer Usb.h, so the extension is declared here)
typedef struct {
  EFI_USB_ENDPOINT_DESCRIPTOR    *EndpointDescriptor;
  VOID                           *EndpointCompanionDescriptor;
} R5Q_SS_EP_INFO;
typedef struct {
  EFI_USB_INTERFACE_DESCRIPTOR    *InterfaceDescriptor;
  R5Q_SS_EP_INFO                  **EndpointDescriptorTable;
} R5Q_SS_IF_INFO;
typedef struct {
  EFI_USB_CONFIG_DESCRIPTOR    *ConfigDescriptor;
  R5Q_SS_IF_INFO               **InterfaceInfoTable;
} R5Q_SS_CFG_INFO;
typedef struct {
  EFI_USB_DEVICE_DESCRIPTOR    *DeviceDescriptor;
  R5Q_SS_CFG_INFO              **ConfigInfoTable;
  VOID                         *BosDescriptor;
} R5Q_SS_DEV_INFO;
typedef EFI_STATUS (EFIAPI *R5Q_USBFN_CONFIGURE_EX)(EFI_USBFN_IO_PROTOCOL *This, EFI_USB_DEVICE_INFO *DeviceInfo, R5Q_SS_DEV_INFO *SsDeviceInfo);
typedef struct {
  EFI_USBFN_IO_PROTOCOL     Std;
  R5Q_USBFN_CONFIGURE_EX    ConfigureEnableEndpointsEx;
  VOID                      *UpdateSSDescriptor;
} R5Q_QCOM_USBFN_IO;

#pragma pack(1)
typedef struct {
  EFI_USB_CONFIG_DESCRIPTOR       Config;
  EFI_USB_INTERFACE_DESCRIPTOR    Interface;
  EFI_USB_ENDPOINT_DESCRIPTOR     EpIn;
  EFI_USB_ENDPOINT_DESCRIPTOR     EpOut;
} R5Q_HS_CONFIG;

typedef struct {
  UINT8     Length;
  UINT8     DescriptorType;
  UINT8     MaxBurst;
  UINT8     Attributes;
  UINT16    BytesPerInterval;
} R5Q_SS_COMPANION;

typedef struct {
  EFI_USB_CONFIG_DESCRIPTOR       Config;
  EFI_USB_INTERFACE_DESCRIPTOR    Interface;
  EFI_USB_ENDPOINT_DESCRIPTOR     EpIn;
  R5Q_SS_COMPANION                EpInComp;
  EFI_USB_ENDPOINT_DESCRIPTOR     EpOut;
  R5Q_SS_COMPANION                EpOutComp;
} R5Q_SS_CONFIG;
#pragma pack()

// BOS: USB 2.0 extension, SuperSpeed capability, MS OS 2.0 platform capability (50 bytes)
STATIC UINT8  mR5qBos[50] = {
  5, 0x0F, 50, 0, 3,
  7, 0x10, 0x02, 0x02, 0x00, 0x00, 0x00,
  10, 0x10, 0x03, 0x00, 0x0E, 0x00, 0x01, 0x0A, 0xFF, 0x07,
  28, 0x10, 0x05, 0x00,
  0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C, 0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
  0x00, 0x00, 0x03, 0x06,   // Windows 8.1+
  162, 0,                   // descriptor set length
  R5Q_MSOS_VENDOR, 0
};

// MS OS 2.0 descriptor set: WINUSB compatible ID + DeviceInterfaceGUIDs = the Android USB interface class (162 bytes)
STATIC UINT8  mR5qMsOs20[162];

STATIC VOID
R5qBuildMsOs20 (
  VOID
  )
{
  STATIC CONST CHAR8  Name[] = "DeviceInterfaceGUIDs";
  STATIC CONST CHAR8  Guid[] = "{F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}";
  UINT8               *P = mR5qMsOs20;
  UINTN               I;

  ZeroMem (mR5qMsOs20, sizeof (mR5qMsOs20));
  // set header
  P[0] = 10; P[2] = 0x00; P[4] = 0x00; P[5] = 0x00; P[6] = 0x03; P[7] = 0x06; P[8] = 162; P += 10;
  // compatible ID
  P[0] = 20; P[2] = 0x03; CopyMem (P + 4, "WINUSB", 6); P += 20;
  // registry property: REG_MULTI_SZ DeviceInterfaceGUIDs
  P[0] = 132; P[2] = 0x04; P[4] = 0x07; P[6] = 42;
  for (I = 0; I < 20; I++) {
    P[8 + 2 * I] = (UINT8)Name[I];
  }
  P[50] = 80;
  for (I = 0; I < 38; I++) {
    P[52 + 2 * I] = (UINT8)Guid[I];
  }
}

STATIC EFI_USBFN_IO_PROTOCOL  *mFn;
STATIC EFI_USB_BUS_SPEED      mFnSpeed;
STATIC BOOLEAN                mFnConfigured;
STATIC BOOLEAN                mFnRxQueued;
STATIC BOOLEAN                mFnTxBusy;
STATIC UINT8                  *mFnEp0;
STATIC UINT8                  *mFnCmd;
STATIC UINT8                  *mFnTx;
STATIC UINT8                  *mFnChunk;
STATIC UINTN                  mFnChunkSize;
STATIC UINT32                 mFnSetups;
STATIC UINT32                 mFnEvents;
STATIC EFI_USB_DEVICE_DESCRIPTOR  mFnDev;
STATIC EFI_USB_DEVICE_DESCRIPTOR  mFnSsDev;
STATIC R5Q_HS_CONFIG          mFnHs;
STATIC R5Q_SS_CONFIG          mFnSs;

// download state
STATIC UINT8                  *mFbData;
STATIC UINTN                  mFbDataPages;
STATIC UINT32                 mFbSize;
STATIC UINT32                 mFbGot;
STATIC UINT32                 mFbPending;   // bytes requested in the current bulk receive
STATIC BOOLEAN                mFbDownloading;
STATIC BOOLEAN                mFbDone;

STATIC VOID
FnLog (
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    L[160];
  VA_LIST  Args;

  VA_START (Args, Format);
  AsciiVSPrint (L, sizeof (L), Format, Args);
  VA_END (Args);
  LogSec ("[USB] ");
  LogSec (L);
  LogSec ("\n");
}

STATIC VOID
FnPoll (
  VOID
  );

STATIC EFI_STATUS
FnEp0Send (
  IN CONST VOID  *Data,
  IN UINTN       Size,
  IN UINT16      WLength
  )
{
  UINTN  N = MIN (Size, (UINTN)WLength);

  CopyMem (mFnEp0, Data, N);
  return mFn->Transfer (mFn, 0, EfiUsbEndpointDirectionDeviceTx, &N, mFnEp0);
}

STATIC VOID
FnStall (
  IN EFI_USB_DEVICE_REQUEST  *R
  )
{
  // Qualcomm's (and the UEFI spec's) SetEndpointStallState takes the state by value; MdePkg declares a pointer
  typedef EFI_STATUS (EFIAPI *R5Q_SET_STALL)(EFI_USBFN_IO_PROTOCOL *, UINT8, EFI_USBFN_ENDPOINT_DIRECTION, BOOLEAN);
  ((R5Q_SET_STALL)(VOID *)mFn->SetEndpointStallState)(
    mFn, 0, (R->RequestType & 0x80) ? EfiUsbEndpointDirectionDeviceTx : EfiUsbEndpointDirectionDeviceRx, TRUE);
}

STATIC UINTN
FnString (
  IN  UINT8  Index,
  OUT UINT8  *Out
  )
{
  STATIC CONST CHAR8  *Strings[] = { NULL, "R5Q", "R5Q UEFI FASTBOOT", "R5QUSB0001" };
  CONST CHAR8         *S;
  UINTN               I;

  if (Index == 0) {
    Out[0] = 4; Out[1] = 3; Out[2] = 0x09; Out[3] = 0x04;
    return 4;
  }
  if (Index >= 4) {
    return 0;
  }
  S = Strings[Index];
  for (I = 0; S[I] != '\0'; I++) {
    Out[2 + 2 * I] = (UINT8)S[I];
    Out[3 + 2 * I] = 0;
  }
  Out[0] = (UINT8)(2 + 2 * I);
  Out[1] = 3;
  return Out[0];
}

STATIC VOID
FnQueueCmd (
  VOID
  )
{
  UINTN       N = 512;
  EFI_STATUS  S;

  if (!mFnConfigured || mFnRxQueued || mFbDownloading) {
    return;
  }
  ZeroMem (mFnCmd, 512);
  S = mFn->Transfer (mFn, 1, EfiUsbEndpointDirectionDeviceRx, &N, mFnCmd);
  mFnRxQueued = !EFI_ERROR (S);
  if (EFI_ERROR (S)) {
    FnLog ("queue command rx failed 0x%lx", (UINT64)S);
  }
}

STATIC VOID
FnHandleSetup (
  IN EFI_USB_DEVICE_REQUEST  *R
  )
{
  UINT8       Buf[256];
  UINTN       N;
  UINT8       Type  = (UINT8)(R->Value >> 8);
  UINT8       Index = (UINT8)R->Value;
  BOOLEAN     Ss    = (mFnSpeed >= UsbBusSpeedSuper);
  EFI_STATUS  S     = EFI_SUCCESS;

  mFnSetups++;
  if (mFnSetups <= 80) {
    FnLog ("setup %02x %02x %04x %04x %u", R->RequestType, R->Request, R->Value, R->Index, R->Length);
  }

  if (R->RequestType == 0x80 && R->Request == USB_REQ_GET_DESCRIPTOR) {
    switch (Type) {
      case USB_DESC_TYPE_DEVICE:
        S = FnEp0Send (Ss ? &mFnSsDev : &mFnDev, sizeof (mFnDev), R->Length);
        break;
      case USB_DESC_TYPE_CONFIG:
        S = Ss ? FnEp0Send (&mFnSs, sizeof (mFnSs), R->Length) : FnEp0Send (&mFnHs, sizeof (mFnHs), R->Length);
        break;
      case USB_DESC_TYPE_STRING:
        N = FnString (Index, Buf);
        if (N == 0) {
          FnStall (R);
          return;
        }
        S = FnEp0Send (Buf, N, R->Length);
        break;
      case 0x06:  // device qualifier (high speed only)
        if (Ss) {
          FnStall (R);
          return;
        }
        ZeroMem (Buf, 10);
        Buf[0] = 10; Buf[1] = 6; Buf[2] = 0x00; Buf[3] = 0x02; Buf[7] = 64; Buf[8] = 1;
        S = FnEp0Send (Buf, 10, R->Length);
        break;
      case 0x0F:  // BOS
        S = FnEp0Send (mR5qBos, sizeof (mR5qBos), R->Length);
        break;
      default:
        FnStall (R);
        return;
    }
  } else if (R->RequestType == 0xC0 && R->Request == R5Q_MSOS_VENDOR && R->Index == 7) {
    S = FnEp0Send (mR5qMsOs20, sizeof (mR5qMsOs20), R->Length);
    FnLog ("MS OS 2.0 descriptor set sent (%u)", R->Length);
  } else if ((R->RequestType & 0x80) && R->Request == USB_REQ_GET_STATUS) {
    ZeroMem (Buf, 2);
    S = FnEp0Send (Buf, 2, R->Length);
  } else if (R->RequestType == 0x80 && R->Request == USB_REQ_GET_CONFIG) {
    Buf[0] = mFnConfigured ? 1 : 0;
    S = FnEp0Send (Buf, 1, R->Length);
  } else if (R->RequestType == 0x81 && R->Request == USB_REQ_GET_INTERFACE) {
    Buf[0] = 0;
    S = FnEp0Send (Buf, 1, R->Length);
  } else if (R->RequestType == 0x00 && R->Request == USB_REQ_SET_CONFIG) {
    mFnConfigured = (R->Value & 0xFF) != 0;
    mFnRxQueued   = FALSE;   // the driver resets the bulk endpoints on SET_CONFIGURATION
    mFnTxBusy     = FALSE;
    FnLog ("configured %u at speed %u", R->Value & 0xFF, mFnSpeed);
    FnQueueCmd ();
  } else if ((R->RequestType & 0x80) == 0 && R->Length != 0) {
    N = MIN ((UINTN)R->Length, (UINTN)512);   // e.g. SET_SEL: accept and ignore the data stage
    S = mFn->Transfer (mFn, 0, EfiUsbEndpointDirectionDeviceRx, &N, mFnEp0);
  } else if ((R->RequestType & 0x80) == 0) {
    // no data stage (SET_INTERFACE, SET/CLEAR_FEATURE, SET_ISOCH_DELAY): the driver runs the status stage
  } else {
    FnStall (R);
    return;
  }
  if (EFI_ERROR (S)) {
    FnLog ("ep0 reply failed 0x%lx", (UINT64)S);
  }
}

STATIC VOID
FnSendResponse (
  IN CONST CHAR8  *Text
  )
{
  UINTN       N = AsciiStrLen (Text);
  EFI_STATUS  S;
  UINT64      T0 = ReadCntPct ();
  UINT64      F  = ReadCntFrq ();

  while (mFnTxBusy && (ReadCntPct () - T0) < 2 * F) {
    FnPoll ();
  }
  CopyMem (mFnTx, Text, N);
  mFnTxBusy = TRUE;
  S         = mFn->Transfer (mFn, 1, EfiUsbEndpointDirectionDeviceTx, &N, mFnTx);
  if (EFI_ERROR (S)) {
    mFnTxBusy = FALSE;
    FnLog ("response tx failed 0x%lx", (UINT64)S);
  }
  if (Text[0] != 'D' || Text[1] != 'A') {
    FnLog ("-> %a", Text);
  }
}

STATIC VOID
FnQueueChunk (
  VOID
  )
{
  UINTN       N;
  EFI_STATUS  S;

  mFbPending = MIN (mFbSize - mFbGot, (UINT32)mFnChunkSize);
  N          = mFbPending;
  S          = mFn->Transfer (mFn, 1, EfiUsbEndpointDirectionDeviceRx, &N, mFnChunk);
  if (EFI_ERROR (S)) {
    FnLog ("queue data rx failed 0x%lx at %u", (UINT64)S, mFbGot);
    mFbDownloading = FALSE;
  }
}

STATIC VOID
FnCommand (
  IN CHAR8  *Cmd
  )
{
  CHAR8       R[64];
  UINT32      Size;
  UINT32      Crc = 0;
  EFI_STATUS  S;

  FnLog ("<- %a", Cmd);
  if (AsciiStrnCmp (Cmd, "getvar:", 7) == 0) {
    Cmd += 7;
    if (AsciiStrCmp (Cmd, "version") == 0) {
      FnSendResponse ("OKAY0.4");
    } else if (AsciiStrCmp (Cmd, "product") == 0) {
      FnSendResponse ("OKAYr5q");
    } else if (AsciiStrCmp (Cmd, "serialno") == 0) {
      FnSendResponse ("OKAYR5QUSB0001");
    } else if (AsciiStrCmp (Cmd, "max-download-size") == 0) {
      AsciiSPrint (R, sizeof (R), "OKAY0x%08x", R5Q_FB_MAX_DOWNLOAD);
      FnSendResponse (R);
    } else if (AsciiStrCmp (Cmd, "r5q-speed") == 0) {
      AsciiSPrint (R, sizeof (R), "OKAY%u", (UINT32)mFnSpeed);
      FnSendResponse (R);
    } else {
      FnSendResponse ("FAILunknown variable");
    }
  } else if (AsciiStrnCmp (Cmd, "download:", 9) == 0) {
    Size = (UINT32)AsciiStrHexToUintn (Cmd + 9);
    if ((Size == 0) || (Size > R5Q_FB_MAX_DOWNLOAD)) {
      FnSendResponse ("FAILbad size");
      return;
    }
    if (mFbData == NULL) {
      EFI_PHYSICAL_ADDRESS  A = 0x27F000000ULL;   // below the top of the largest free block (0xF4000000 + 6326 MB)
      mFbDataPages = EFI_SIZE_TO_PAGES (R5Q_FB_MAX_DOWNLOAD);
      S            = gBS->AllocatePages (AllocateMaxAddress, EfiLoaderData, mFbDataPages, &A);
      if (EFI_ERROR (S)) {
        FnLog ("download buffer allocation failed 0x%lx", (UINT64)S);
        FnSendResponse ("FAILno memory");
        return;
      }
      mFbData = (UINT8 *)(UINTN)A;
      FnLog ("download buffer at 0x%lx", (UINT64)A);
    }
    mFbSize        = Size;
    mFbGot         = 0;
    mFbDownloading = TRUE;
    AsciiSPrint (R, sizeof (R), "DATA%08x", Size);
    FnSendResponse (R);
    FnLog ("download %u bytes", Size);
    FnQueueChunk ();
  } else if (AsciiStrCmp (Cmd, "oem crc") == 0) {
    if ((mFbData == NULL) || (mFbGot == 0)) {
      FnSendResponse ("FAILnothing downloaded");
      return;
    }
    gBS->CalculateCrc32 (mFbData, mFbGot, &Crc);
    AsciiSPrint (R, sizeof (R), "INFOcrc32=%08x size=%u", Crc, mFbGot);
    FnSendResponse (R);
    FnLog ("crc32 %08x over %u bytes at 0x%lx", Crc, mFbGot, (UINT64)(UINTN)mFbData);
    FnSendResponse ("OKAY");
  } else if ((AsciiStrCmp (Cmd, "continue") == 0) || (AsciiStrnCmp (Cmd, "reboot", 6) == 0)) {
    FnSendResponse ("OKAY");
    mFbDone = TRUE;
  } else {
    FnSendResponse ("FAILunknown command");
  }
}

STATIC UINT64  mFbT0;

STATIC VOID
FnPoll (
  VOID
  )
{
  EFI_USBFN_MESSAGE          Msg = EfiUsbMsgNone;
  EFI_USBFN_MESSAGE_PAYLOAD  P;
  UINTN                      PSize = sizeof (P);
  EFI_STATUS                 S;
  CHAR8                      Cmd[513];
  UINTN                      N;

  ZeroMem (&P, sizeof (P));
  S = mFn->EventHandler (mFn, &Msg, &PSize, &P);
  if (EFI_ERROR (S) || (Msg == EfiUsbMsgNone)) {
    return;
  }
  mFnEvents++;
  switch (Msg) {
    case EfiUsbMsgSetupPacket:
      FnHandleSetup (&P.udr);
      break;
    case EfiUsbMsgBusEventSpeed:
      mFnSpeed = P.ubs;
      FnLog ("bus speed %u", (UINT32)P.ubs);
      break;
    case EfiUsbMsgBusEventReset:
    case EfiUsbMsgBusEventAttach:
    case EfiUsbMsgBusEventDetach:
    case EfiUsbMsgBusEventSuspend:
    case EfiUsbMsgBusEventResume:
      FnLog ("bus event %u", (UINT32)Msg);
      if ((Msg == EfiUsbMsgBusEventReset) || (Msg == EfiUsbMsgBusEventDetach)) {
        mFnConfigured = FALSE;
        mFnRxQueued   = FALSE;
        mFnTxBusy     = FALSE;
      }
      break;
    case EfiUsbMsgEndpointStatusChangedTx:
      if (P.utr.EndpointIndex == 1) {
        mFnTxBusy = FALSE;
      }
      break;
    case EfiUsbMsgEndpointStatusChangedRx:
      if (P.utr.EndpointIndex != 1) {
        break;
      }
      if (P.utr.TransferStatus != UsbTransferStatusComplete) {
        FnLog ("bulk rx status %u (%lu bytes)", (UINT32)P.utr.TransferStatus, (UINT64)P.utr.BytesTransferred);
        mFnRxQueued = FALSE;
        if (mFbDownloading) {
          mFbDownloading = FALSE;
        }
        FnQueueCmd ();
        break;
      }
      if (mFbDownloading) {
        N = MIN ((UINTN)P.utr.BytesTransferred, (UINTN)mFbPending);
        CopyMem (mFbData + mFbGot, mFnChunk, N);
        mFbGot += (UINT32)N;
        if (mFbGot >= mFbSize) {
          UINT64  Ms = ((ReadCntPct () - mFbT0) * 1000ULL) / ReadCntFrq ();
          mFbDownloading = FALSE;
          FnLog ("download complete: %u bytes in %lu ms", mFbGot, Ms);
          FnSendResponse ("OKAY");
          FnQueueCmd ();
        } else {
          FnQueueChunk ();
        }
        break;
      }
      mFnRxQueued = FALSE;
      N           = MIN ((UINTN)P.utr.BytesTransferred, (UINTN)512);
      CopyMem (Cmd, mFnCmd, N);
      Cmd[N] = '\0';
      if (AsciiStrnCmp (Cmd, "download:", 9) == 0) {
        mFbT0 = ReadCntPct ();
      }
      FnCommand (Cmd);
      FnQueueCmd ();
      break;
    default:
      break;
  }
}

//
// One USB session: wait up to WaitFirstS seconds for a host to configure the device, then serve fastboot commands
// until "continue"/"reboot", or until IdleS seconds pass without a command. Never touches storage.
//
STATIC VOID
R5qUsbSession (
  IN UINT32  WaitFirstS,
  IN UINT32  IdleS
  )
{
  STATIC EFI_GUID                       FnGuid = EFI_USBFN_IO_PROTOCOL_GUID;
  EFI_STATUS                            S;
  EFI_USB_ENDPOINT_DESCRIPTOR           *HsEps[2];
  EFI_USB_INTERFACE_INFO                HsIf;
  EFI_USB_INTERFACE_INFO                *HsIfs[1];
  EFI_USB_CONFIG_INFO                   HsCfg;
  EFI_USB_CONFIG_INFO                   *HsCfgs[1];
  EFI_USB_DEVICE_INFO                   HsInfo;
  R5Q_SS_EP_INFO                        SsEp[2];
  R5Q_SS_EP_INFO                        *SsEps[2];
  R5Q_SS_IF_INFO                        SsIf;
  R5Q_SS_IF_INFO                        *SsIfs[1];
  R5Q_SS_CFG_INFO                       SsCfg;
  R5Q_SS_CFG_INFO                       *SsCfgs[1];
  R5Q_SS_DEV_INFO                       SsInfo;
  EFI_USBFN_PORT_TYPE                   Port = EfiUsbUnknownPort;
  UINTN                                 MaxXfer = 0;
  UINT64                                F, T0, LastCmd;
  BOOLEAN                               Ex = FALSE;
  UINT32                                LastGot = 0;
  CHAR8                                 L[64];

  mFn = NULL;
  S   = gBS->LocateProtocol (&FnGuid, NULL, (VOID **)&mFn);
  FnLog ("UsbfnIo 0x%lx rev 0x%x", (UINT64)S, (mFn != NULL) ? mFn->Revision : 0);
  if (EFI_ERROR (S) || (mFn == NULL)) {
    FbPrintLine ("[U] USBFN IO MISSING", 0xFFFF5050U);
    return;
  }
  R5qBuildMsOs20 ();

  ZeroMem (&mFnDev, sizeof (mFnDev));
  mFnDev.Length            = 18;
  mFnDev.DescriptorType    = USB_DESC_TYPE_DEVICE;
  mFnDev.BcdUSB            = 0x0210;
  mFnDev.MaxPacketSize0    = 64;
  mFnDev.IdVendor          = R5Q_USB_VID;
  mFnDev.IdProduct         = R5Q_USB_PID;
  mFnDev.BcdDevice         = 0x0100;
  mFnDev.StrManufacturer   = 1;
  mFnDev.StrProduct        = 2;
  mFnDev.StrSerialNumber   = 3;
  mFnDev.NumConfigurations = 1;
  CopyMem (&mFnSsDev, &mFnDev, sizeof (mFnDev));
  mFnSsDev.BcdUSB         = 0x0320;
  mFnSsDev.MaxPacketSize0 = 9;

  ZeroMem (&mFnHs, sizeof (mFnHs));
  mFnHs.Config.Length             = 9;
  mFnHs.Config.DescriptorType     = USB_DESC_TYPE_CONFIG;
  mFnHs.Config.TotalLength        = sizeof (mFnHs);
  mFnHs.Config.NumInterfaces      = 1;
  mFnHs.Config.ConfigurationValue = 1;
  mFnHs.Config.Attributes         = 0x80;
  mFnHs.Config.MaxPower           = 0x32;
  mFnHs.Interface.Length            = 9;
  mFnHs.Interface.DescriptorType    = USB_DESC_TYPE_INTERFACE;
  mFnHs.Interface.NumEndpoints      = 2;
  mFnHs.Interface.InterfaceClass    = 0xFF;
  mFnHs.Interface.InterfaceSubClass = 0x42;
  mFnHs.Interface.InterfaceProtocol = 0x03;
  mFnHs.EpIn.Length          = 7;
  mFnHs.EpIn.DescriptorType  = USB_DESC_TYPE_ENDPOINT;
  mFnHs.EpIn.EndpointAddress = 0x81;
  mFnHs.EpIn.Attributes      = USB_ENDPOINT_BULK;
  mFnHs.EpIn.MaxPacketSize   = 512;
  CopyMem (&mFnHs.EpOut, &mFnHs.EpIn, sizeof (mFnHs.EpIn));
  mFnHs.EpOut.EndpointAddress = 0x01;

  ZeroMem (&mFnSs, sizeof (mFnSs));
  CopyMem (&mFnSs.Config, &mFnHs.Config, sizeof (mFnHs.Config));
  CopyMem (&mFnSs.Interface, &mFnHs.Interface, sizeof (mFnHs.Interface));
  mFnSs.Config.TotalLength = sizeof (mFnSs);
  mFnSs.Config.MaxPower    = 0x0C;
  CopyMem (&mFnSs.EpIn, &mFnHs.EpIn, sizeof (mFnHs.EpIn));
  CopyMem (&mFnSs.EpOut, &mFnHs.EpOut, sizeof (mFnHs.EpOut));
  mFnSs.EpIn.MaxPacketSize  = 1024;
  mFnSs.EpOut.MaxPacketSize = 1024;
  mFnSs.EpInComp.Length          = 6;
  mFnSs.EpInComp.DescriptorType  = 0x30;
  mFnSs.EpInComp.MaxBurst        = 15;
  CopyMem (&mFnSs.EpOutComp, &mFnSs.EpInComp, sizeof (mFnSs.EpInComp));

  HsEps[0] = &mFnHs.EpIn;
  HsEps[1] = &mFnHs.EpOut;
  HsIf.InterfaceDescriptor     = &mFnHs.Interface;
  HsIf.EndpointDescriptorTable = HsEps;
  HsIfs[0]                     = &HsIf;
  HsCfg.ConfigDescriptor       = &mFnHs.Config;
  HsCfg.InterfaceInfoTable     = HsIfs;
  HsCfgs[0]                    = &HsCfg;
  HsInfo.DeviceDescriptor      = &mFnDev;
  HsInfo.ConfigInfoTable       = HsCfgs;

  SsEp[0].EndpointDescriptor          = &mFnSs.EpIn;
  SsEp[0].EndpointCompanionDescriptor = &mFnSs.EpInComp;
  SsEp[1].EndpointDescriptor          = &mFnSs.EpOut;
  SsEp[1].EndpointCompanionDescriptor = &mFnSs.EpOutComp;
  SsEps[0]                     = &SsEp[0];
  SsEps[1]                     = &SsEp[1];
  SsIf.InterfaceDescriptor     = &mFnSs.Interface;
  SsIf.EndpointDescriptorTable = SsEps;
  SsIfs[0]                     = &SsIf;
  SsCfg.ConfigDescriptor       = &mFnSs.Config;
  SsCfg.InterfaceInfoTable     = SsIfs;
  SsCfgs[0]                    = &SsCfg;
  SsInfo.DeviceDescriptor      = &mFnSsDev;
  SsInfo.ConfigInfoTable       = SsCfgs;
  SsInfo.BosDescriptor         = mR5qBos;

  S = mFn->AllocateTransferBuffer (mFn, 4096, (VOID **)&mFnEp0);
  if (!EFI_ERROR (S)) {
    S = mFn->AllocateTransferBuffer (mFn, 4096, (VOID **)&mFnCmd);
  }
  if (!EFI_ERROR (S)) {
    S = mFn->AllocateTransferBuffer (mFn, 4096, (VOID **)&mFnTx);
  }
  if (!EFI_ERROR (mFn->GetMaxTransferSize (mFn, &MaxXfer)) && (MaxXfer != 0)) {
    mFnChunkSize = MIN ((UINTN)R5Q_FB_CHUNK, MaxXfer) & ~(UINTN)1023;
  } else {
    mFnChunkSize = 0x100000;
  }
  if (!EFI_ERROR (S)) {
    S = mFn->AllocateTransferBuffer (mFn, mFnChunkSize, (VOID **)&mFnChunk);
  }
  FnLog ("buffers 0x%lx; max transfer %lu, chunk %lu", (UINT64)S, (UINT64)MaxXfer, (UINT64)mFnChunkSize);
  if (EFI_ERROR (S)) {
    FbPrintLine ("[U] USB BUFFERS FAILED", 0xFFFF5050U);
    return;
  }

  S = mFn->StartController (mFn);
  FnLog ("StartController 0x%lx", (UINT64)S);
  if (mFn->Revision >= 0x00010003) {
    S  = ((R5Q_QCOM_USBFN_IO *)mFn)->ConfigureEnableEndpointsEx (mFn, &HsInfo, &SsInfo);
    Ex = !EFI_ERROR (S);
    FnLog ("ConfigureEnableEndpointsEx 0x%lx", (UINT64)S);
  }
  if (!Ex) {
    S = mFn->ConfigureEnableEndpoints (mFn, &HsInfo);
    FnLog ("ConfigureEnableEndpoints 0x%lx", (UINT64)S);
  }
  if (EFI_ERROR (S)) {
    FbPrintLine ("[U] USB CONFIGURE FAILED", 0xFFFF5050U);
    mFn->StopController (mFn);
    return;
  }
  mFn->DetectPort (mFn, &Port);
  FnLog ("port type %u; waiting up to %u s for the host", (UINT32)Port, WaitFirstS);
  FbPrintLine ("[U] FASTBOOT ON USB: WAITING FOR THE PC", 0xFF80C0FFU);
  R5qPersistLog ("usb waiting");

  F       = ReadCntFrq ();
  T0      = ReadCntPct ();
  LastCmd = 0;
  while (!mFbDone) {
    UINT32  Before        = mFnEvents;
    BOOLEAN WasConfigured = mFnConfigured;
    FnPoll ();
    if (mFnConfigured && !WasConfigured) {
      FbPrintLine ("[U] PC CONNECTED", 0xFF40FF60U);
      LastCmd = ReadCntPct ();
    }
    if ((mFbGot != LastGot) || (mFnEvents != Before && mFnConfigured)) {
      LastCmd = ReadCntPct ();
      LastGot = mFbGot;
    }
    if (mFnRxQueued == FALSE && mFnConfigured && !mFbDownloading) {
      FnQueueCmd ();
    }
    if (LastCmd == 0) {
      if ((ReadCntPct () - T0) > (UINT64)WaitFirstS * F) {
        FnLog ("no host within %u s", WaitFirstS);
        break;
      }
    } else if ((ReadCntPct () - LastCmd) > (UINT64)IdleS * F) {
      FnLog ("idle for %u s; leaving", IdleS);
      break;
    }
  }
  if (mFbDone) {
    // let the final OKAY go out
    T0 = ReadCntPct ();
    while (mFnTxBusy && (ReadCntPct () - T0) < F) {
      FnPoll ();
    }
  }
  AsciiSPrint (L, sizeof (L), "[U] USB SESSION END, %u BYTES IN", mFbGot);
  FbPrintLine (L, 0xFFF0F4F8U);
  FnLog ("session end: setups %u, downloaded %u, done %u", mFnSetups, mFbGot, (UINT32)mFbDone);
  S = mFn->StopController (mFn);
  FnLog ("StopController 0x%lx", (UINT64)S);
}
