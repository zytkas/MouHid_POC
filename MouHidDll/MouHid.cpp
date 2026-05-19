#include "pch.h"
#include <windows.h>
#include <winioctl.h>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

typedef void* IntPtr;
#define FILE_DEVICE_MOUHID_INPUT_HOOK  51382

#define IOCTL_ENABLE_MOUHID_INPUT_MONITOR  \
    CTL_CODE(FILE_DEVICE_MOUHID_INPUT_HOOK, 3500, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_SET_MOUSE_BLOCKING           \
    CTL_CODE(FILE_DEVICE_MOUHID_INPUT_HOOK, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_READ_MOUSE_DATA              \
    CTL_CODE(FILE_DEVICE_MOUHID_INPUT_HOOK, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _MOUSE_INPUT_DATA {
    USHORT UnitId;
    USHORT Flags;
    union {
        ULONG Buttons;
        struct {
            USHORT ButtonFlags;
            USHORT ButtonData;
        };
    };
    ULONG RawButtons;
    LONG LastX;
    LONG LastY;
    ULONG ExtraInformation;
} MOUSE_INPUT_DATA, * PMOUSE_INPUT_DATA;

typedef struct _SET_MOUSE_BLOCKING_REQUEST {
    BOOLEAN BlockMouse;
} SET_MOUSE_BLOCKING_REQUEST;

struct MouseDataPacket {
    int DeltaX;
    int DeltaY;
    unsigned short ButtonFlags;
};


#define RING_BUFFER_SIZE 1024 

class MouHidContext {
public:
    HANDLE hDevice;
    HANDLE hStopEvent;
    std::thread* readerThread;
    std::atomic<bool> isRunning;

    MouseDataPacket buffer[RING_BUFFER_SIZE];
    int head;
    int tail; 
    std::mutex bufferMutex; 

    MouHidContext() : hDevice(INVALID_HANDLE_VALUE), hStopEvent(NULL), readerThread(nullptr), isRunning(false), head(0), tail(0) {}
};

void ReaderWorker(MouHidContext* ctx) {
    HANDLE hOverlappedEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    HANDLE handles[2] = { hOverlappedEvent, ctx->hStopEvent };

    MOUSE_INPUT_DATA driverBuffer[64];
    DWORD bytesRead;
    OVERLAPPED overlapped = { 0 }; 

    while (ctx->isRunning) {
        RtlZeroMemory(&overlapped, sizeof(OVERLAPPED));
        overlapped.hEvent = hOverlappedEvent;

        BOOL result = DeviceIoControl(
            ctx->hDevice,
            IOCTL_READ_MOUSE_DATA,
            NULL, 0,
            driverBuffer, sizeof(driverBuffer),
            NULL,
            &overlapped
        );

        DWORD transferedBytes = 0;
        bool dataReady = false;

        if (!result) {
            DWORD err = GetLastError();

            if (err == ERROR_IO_PENDING) {
                DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

                if (waitResult == WAIT_OBJECT_0) {
                    if (GetOverlappedResult(ctx->hDevice, &overlapped, &transferedBytes, FALSE)) {
                        dataReady = true;
                    }
                }
                else if (waitResult == WAIT_OBJECT_0 + 1) {
                    CancelIo(ctx->hDevice);
                    break;
                }
            }
            else {
                Sleep(10);
                continue;
            }
        }
        else {
            if (GetOverlappedResult(ctx->hDevice, &overlapped, &transferedBytes, FALSE)) {
                dataReady = true;
            }
        }

        if (dataReady && transferedBytes > 0) {
            int count = transferedBytes / sizeof(MOUSE_INPUT_DATA);
            std::lock_guard<std::mutex> lock(ctx->bufferMutex);

            for (int i = 0; i < count; i++) {
                int nextHead = (ctx->head + 1) % RING_BUFFER_SIZE;
                if (nextHead != ctx->tail) {
                    ctx->buffer[ctx->head].DeltaX = driverBuffer[i].LastX;
                    ctx->buffer[ctx->head].DeltaY = driverBuffer[i].LastY;
                    ctx->buffer[ctx->head].ButtonFlags = driverBuffer[i].ButtonFlags;
                    ctx->head = nextHead;
                }
            }
        }
    }

    CloseHandle(hOverlappedEvent);
}

extern "C" {

    __declspec(dllexport) IntPtr __stdcall MouHid_Create() {
        return (IntPtr)(new MouHidContext());
    }

    __declspec(dllexport) bool __stdcall MouHid_Connect(IntPtr handle) {
        MouHidContext* ctx = (MouHidContext*)handle;
        if (!ctx) return false;

        ctx->hDevice = CreateFileW(
            L"\\\\.\\MouHidInputHook",
            GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, NULL
        );

        if (ctx->hDevice == INVALID_HANDLE_VALUE) return false;
        DWORD bytes;
        DeviceIoControl(ctx->hDevice, IOCTL_ENABLE_MOUHID_INPUT_MONITOR, NULL, 0, NULL, 0, &bytes, NULL);
        ctx->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        ctx->isRunning = true;
        ctx->readerThread = new std::thread(ReaderWorker, ctx);
        SetThreadPriority(ctx->readerThread->native_handle(), THREAD_PRIORITY_TIME_CRITICAL);

        return true;
    }

    __declspec(dllexport) bool __stdcall MouHid_SetBlocking(IntPtr handle, bool block) {
        MouHidContext* ctx = (MouHidContext*)handle;
        if (!ctx || ctx->hDevice == INVALID_HANDLE_VALUE) return false;

        SET_MOUSE_BLOCKING_REQUEST req;
        req.BlockMouse = block;
        DWORD bytes;

        return DeviceIoControl(
            ctx->hDevice,
            IOCTL_SET_MOUSE_BLOCKING,
            &req, sizeof(req),
            NULL, 0, &bytes, NULL
        );
    }

    __declspec(dllexport) bool __stdcall MouHid_ReadMouseData(
        IntPtr handle,
        MouseDataPacket* outData,
        int* outPacketCount,
        int maxPackets
    ) {
        MouHidContext* ctx = (MouHidContext*)handle;
        if (!ctx) return false;

        *outPacketCount = 0;
        std::lock_guard<std::mutex> lock(ctx->bufferMutex);

        if (ctx->head == ctx->tail) return true; 

        int count = 0;
        while (ctx->tail != ctx->head && count < maxPackets) {
            outData[count] = ctx->buffer[ctx->tail];
            ctx->tail = (ctx->tail + 1) % RING_BUFFER_SIZE;
            count++;
        }

        *outPacketCount = count;
        return true;
    }

    __declspec(dllexport) void __stdcall MouHid_Destroy(IntPtr handle) {
        MouHidContext* ctx = (MouHidContext*)handle;
        if (!ctx) return;

        ctx->isRunning = false;
        if (ctx->hStopEvent) SetEvent(ctx->hStopEvent);

        if (ctx->hDevice != INVALID_HANDLE_VALUE) {
            CancelIoEx(ctx->hDevice, NULL); 
        }
        if (ctx->readerThread) {
            if (ctx->readerThread->joinable()) ctx->readerThread->join();
            delete ctx->readerThread;
        }
        if (ctx->hDevice != INVALID_HANDLE_VALUE) {
            SET_MOUSE_BLOCKING_REQUEST req = { FALSE };
            DWORD bytes;
            DeviceIoControl(ctx->hDevice, IOCTL_SET_MOUSE_BLOCKING, &req, sizeof(req), NULL, 0, &bytes, NULL);
            CloseHandle(ctx->hDevice);
        }
        if (ctx->hStopEvent) CloseHandle(ctx->hStopEvent);
        delete ctx;
    }
}
