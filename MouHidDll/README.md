# MouHid.dll

**User-mode component for [MapperGang](https://github.com/zytkas/MapperGang).**

A thin wrapper around the IOCTLs of the `MouHidInputHook.sys` driver — it turns them into a convenient C API that `MapperGang` calls via P/Invoke ([`InputCaptureManager.cs`](https://github.com/zytkas/MapperGang/blob/master/Services/InputCaptureService/InputCaptureManager.cs)).

## What it does

- Opens `\\.\MouHidInputHook` via `CreateFileW` (`FILE_FLAG_OVERLAPPED`).
- Starts a background read-thread with `THREAD_PRIORITY_TIME_CRITICAL` that reads `MOUSE_INPUT_DATA` through `IOCTL_READ_MOUSE_DATA` and stores it in its own ring buffer (1024 packets).
- Enables/disables physical mouse blocking via `IOCTL_SET_MOUSE_BLOCKING`.
- Returns accumulated packets in a batch through `MouHid_ReadMouseData`.

## Exported API (`__stdcall`)

```cpp
IntPtr  MouHid_Create();
bool    MouHid_Connect(IntPtr handle);
bool    MouHid_SetBlocking(IntPtr handle, bool block);
bool    MouHid_ReadMouseData(IntPtr handle,
                             MouseDataPacket* outData,
                             int* outPacketCount,
                             int maxPackets);
void    MouHid_Destroy(IntPtr handle);

struct MouseDataPacket {
    int            DeltaX;       // MOUSE_INPUT_DATA.LastX
    int            DeltaY;       // MOUSE_INPUT_DATA.LastY
    unsigned short ButtonFlags;  // MOUSE_INPUT_DATA.ButtonFlags
};
```

`ButtonFlags` are the standard WDK flags (`MOUSE_LEFT_DOWN = 0x0001`, `MOUSE_BUTTON_4_DOWN = 0x0040`, `MOUSE_BUTTON_5_DOWN = 0x0100`, …).

## Requirements

- The `MouHidInputHook.sys` driver is loaded and the symbolic link `\\.\MouHidInputHook` is available.
- A process with **admin** rights (to open the device handle).
- x64 platform.

## Where to put it

Next to `MapperGang.exe` (or in any folder in `PATH`). The C# side is set to:

```csharp
[DllImport("MouHid.dll", CallingConvention = CallingConvention.StdCall)]
```

— meaning the file name is fixed.

## Related projects

- Driver: see parent folder [`../`](../) (`MouHidInputHook.sys`).
- Consumer: [MapperGang](https://github.com/zytkas/MapperGang).
