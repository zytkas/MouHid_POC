# MouHidInputHook (MapperGang fork)

> **This is a fork** of [`changeofpace/MouHidInputHook`](https://github.com/changeofpace/MouHidInputHook), adapted for the needs of [**MapperGang**](https://github.com/zytkas/MapperGang) — an application that maps mouse and keyboard to a virtual gamepad via ViGEm.
>
> The original project is a read-only hook for monitoring the HID mouse. This fork adds **blocking** of the physical mouse and a **user-mode API** for reading raw deltas.

## Why this fork

In the original, the `MouHid` hook can only **observe** mouse packets — it cannot modify or swallow them. To emulate "mouse as right stick" in MapperGang, the following was required:

1. **Fully block** physical mouse input from the OS (Windows must not move the cursor or click).
2. **Read raw `LastX/LastY/ButtonFlags`** from the kernel in user-mode with minimal latency.
3. Do this **without modifying win32k** and without installing a full filter driver (PatchGuard-safe).

The fork solves both tasks: a kernel-level "gate" inside the hook callback + a pair of IOCTLs + a user-mode DLL on top.

## Changes relative to upstream

### Kernel (driver) changes

| File | What was added |
|------|----------------|
| [`Common/ioctl.h`](Common/ioctl.h) | Two new IOCTLs — `IOCTL_SET_MOUSE_BLOCKING` (0x803) and `IOCTL_READ_MOUSE_DATA` (0x804); request structure `SET_MOUSE_BLOCKING_REQUEST`. |
| [`MouHidInputHook/driver.cpp`](MouHidInputHook/driver.cpp) | Support for **manual-map** loading (via `IoCreateDriver` when `pDriverObject == NULL`). `DIRECT_IO` instead of `BUFFERED`. Handlers for the two new IOCTLs: blocking + reading pending-IRPs. Cancel-routine for the read-IRP. |
| [`MouHidInputHook/mouhid_hook_manager.h/.cpp`](MouHidInputHook/mouhid_hook_manager.cpp) | Global flag `g_BlockMouseInput`, ring buffer `g_InputBuffer` (1024 `MOUSE_INPUT_DATA` packets), pending-IRP queue with spinlock. When blocking is enabled, the original class service callback is **not called**; packets are stored in the buffer and handed to a waiting IRP. |

In sum: the hook callback can now "eat" a packet (the OS doesn't see it) and simultaneously deliver its contents to a user-mode reader.

### User-mode additions

| Folder | What it is |
|--------|------------|
| [`MouHidDll/`](MouHidDll/) | **`MouHid.dll`** — a wrapper around the two new IOCTLs. Exports `MouHid_Create/Connect/SetBlocking/ReadMouseData/Destroy`. Internally holds its own ring buffer and a reader thread with `THREAD_PRIORITY_TIME_CRITICAL`. This is **the main component imported by MapperGang** via P/Invoke (`InputCaptureManager.cs`). |
| [`usermode/`](usermode/) | Standalone console driver tester with no dependency on the DLL — convenient for debugging IOCTLs in isolation. |

## Architecture (after fork)

The physical mouse (USB HID) feeds into `mouhid.sys` (Microsoft), which delivers `MOUSE_INPUT_DATA` to the class service callback. `MouHidInputHook.sys` (this fork) intercepts that callback:

- If `g_BlockMouseInput` is **off**, the original callback runs normally (`mouclass.sys` → `win32k` → OS).
- If `g_BlockMouseInput` is **on**, packets go into the ring buffer and complete the pending IRP.

The pending IRP is issued by `MouHid.dll` (user-mode) via `IOCTL_READ_MOUSE_DATA`, which exposes the data through P/Invoke to `MapperGang.exe` (C# / WPF).

In blocked mode, the system **does not see** any mouse movement or clicks — they are routed entirely into our user-mode pipeline. Escape hatch: press `Mouse4` or `Mouse5`; the DLL's read-loop catches that flag and disables blocking.

## Build

Dependencies and order are the same as in upstream:

1. Visual Studio 2019/2022 with WDK for the driver (`MouHidInputHook.vcxproj`, `MouHidMonitor.vcxproj`).
2. MSVC for the DLL and usermode tester (`MouHidDll.vcxproj` — added to [`MouHidInputHook.sln`](MouHidInputHook.sln)).

```
Build → Configuration Manager → x64
Solution → Rebuild Solution
```

Artifacts:
- `bin\x64\Release\MouHidInputHook.sys` — the driver itself.
- `bin\x64\Release\MouHidMonitor.exe` — the original monitoring client.
- `bin\x64\Release\MouHid.dll` — this file must be placed next to `MapperGang.exe`.

## Running

1. Enable **test signing** or sign `MouHidInputHook.sys` (standard requirement for a kernel driver without an EV cert):
   ```
   bcdedit /set testsigning on
   shutdown /r /t 0
   ```
2. Register and start the driver (any standard method — `sc create` / `OSR Driver Loader` / manual-map via kdmapper):
   ```
   sc create MouHidInputHook type= kernel binPath= "C:\path\to\MouHidInputHook.sys"
   sc start MouHidInputHook
   ```
3. Launch MapperGang — it will find `\\.\MouHidInputHook` on its own through `MouHid.dll`.

## License

Inherited from upstream — **MIT** (see [LICENSE](LICENSE)).
Copyright (c) 2019 changeofpace for the original portion; modifications made for MapperGang.

## Links

- Upstream: https://github.com/changeofpace/MouHidInputHook
- Related project: [MapperGang](https://github.com/zytkas/MapperGang) (mouse + keyboard → virtual gamepad)
