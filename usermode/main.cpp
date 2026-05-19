#include <windows.h>
#include <stdio.h>
#include <conio.h>
#include <thread>
#include <atomic>
#include <vector>

// ==========================================
// 1. DEFINITIONS (Must match Driver)
// ==========================================
#define FILE_DEVICE_MOUHID_INPUT_HOOK  51382
#define IOCTL_ENABLE_MOUHID_INPUT_MONITOR  CTL_CODE(FILE_DEVICE_MOUHID_INPUT_HOOK, 3500, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SET_MOUSE_BLOCKING           CTL_CODE(FILE_DEVICE_MOUHID_INPUT_HOOK, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_READ_MOUSE_DATA              CTL_CODE(FILE_DEVICE_MOUHID_INPUT_HOOK, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _SET_MOUSE_BLOCKING_REQUEST {
    BOOLEAN BlockMouse;
} SET_MOUSE_BLOCKING_REQUEST;

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

// Driver Constants
#define MOUSE_BUTTON_5_DOWN 0x0100

// WinAPI Constants
#define VK_XBUTTON2         0x06 // Mouse 5

// ==========================================
// 2. LOGIC CLASS
// ==========================================
class MouHidTester {
    HANDLE hDevice;
    HANDLE hStopEvent;
    std::atomic<bool> isRunning;
    std::atomic<bool> isBlocked;
    std::thread* workerThread;

    // Stats
    long totalPackets = 0;
    long lastX = 0;
    long lastY = 0;

    // Send IOCTL to driver
    bool SetDriverBlockState(bool block) {
        if (hDevice == INVALID_HANDLE_VALUE) return false;

        SET_MOUSE_BLOCKING_REQUEST req;
        req.BlockMouse = block ? TRUE : FALSE;
        DWORD bytes;

        // В твоем новом драйвере этот вызов сделает FlushQueue!
        // Это значит, что висящий Read запрос завершится мгновенно.
        return DeviceIoControl(hDevice, IOCTL_SET_MOUSE_BLOCKING, &req, sizeof(req), NULL, 0, &bytes, NULL);
    }

    void ThreadLoop() {
        OVERLAPPED overlapped = { 0 };
        overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL); // MANUAL reset!
        MOUSE_INPUT_DATA buffer[64];
        HANDLE events[2] = { overlapped.hEvent, hStopEvent };

        BOOL requestPending = FALSE; // Флаг активного запроса

        printf("[THREAD] Started. Mode: %s\n", isBlocked ? "BLOCKED" : "SYSTEM");

        while (isRunning) {

            // ==============================================
            // MODE 1: BLOCKED (Reading Driver)
            // ==============================================
            if (isBlocked) {

                // Если нет активного запроса - отправляем новый
                if (!requestPending) {
                    // Очищаем структуру
                    ZeroMemory(&overlapped, sizeof(overlapped));
                    overlapped.hEvent = events[0];
                    ResetEvent(overlapped.hEvent);

                    // Отправляем запрос
                    BOOL res = DeviceIoControl(
                        hDevice,
                        IOCTL_READ_MOUSE_DATA,
                        NULL, 0,
                        buffer, sizeof(buffer),
                        NULL,
                        &overlapped
                    );

                    if (!res) {
                        DWORD err = GetLastError();
                        if (err == ERROR_IO_PENDING) {
                            requestPending = TRUE;
                        }
                        else {
                            printf("[ERROR] DeviceIoControl failed: %d\n", err);
                            Sleep(100);
                            continue;
                        }
                    }
                    else {
                        // Запрос завершился сразу
                        requestPending = TRUE;
                    }
                }

                // Ждем завершения запроса
                DWORD waitRes = WaitForMultipleObjects(2, events, FALSE, INFINITE);

                if (waitRes == WAIT_OBJECT_0) {
                    // IRP завершился
                    DWORD bytesRead = 0;
                    BOOL getResult = GetOverlappedResult(hDevice, &overlapped, &bytesRead, FALSE);
                    requestPending = FALSE; // Запрос завершен

                    if (!getResult) {
                        DWORD err = GetLastError();
                        printf("[ERROR] GetOverlappedResult failed: %d (0x%08X)\n", err, err);
                        printf("        ERROR_OPERATION_ABORTED = %d\n", ERROR_OPERATION_ABORTED);
                        printf("        BytesRead = %d\n", bytesRead);
                        printf("        Will resubmit = %d\n", !isBlocked ? 0 : 1);
                        Sleep(50);
                        continue;
                    }

                    if (bytesRead == 0) {
                        // Flush произошел - драйвер переключился в SYSTEM режим
                        // Просто продолжаем цикл, isBlocked уже должен быть false
                        printf("[DEBUG] Flush detected (0 bytes)\n");
                        continue;
                    }

                    // Обрабатываем данные
                    int count = bytesRead / sizeof(MOUSE_INPUT_DATA);
                    totalPackets += count;

                    for (int i = 0; i < count; i++) {
                        lastX = buffer[i].LastX;
                        lastY = buffer[i].LastY;

                        // DETECT UNBLOCK TRIGGER
                        if (buffer[i].ButtonFlags & MOUSE_BUTTON_5_DOWN) {
                            printf("\n[TRIGGER] Mouse 5 (Driver) -> UNBLOCKING...\n");

                            // ВАЖНО: Сначала меняем флаг
                            isBlocked = false;

                            // Потом отправляем команду драйверу
                            SetDriverBlockState(false);

                            // Отменяем текущий запрос (если он еще висит)
                            if (requestPending) {
                                CancelIoEx(hDevice, &overlapped);
                                // Ждем завершения отмены
                                DWORD temp;
                                GetOverlappedResult(hDevice, &overlapped, &temp, TRUE);
                                requestPending = FALSE;
                            }

                            // Debounce
                            while ((GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0) {
                                Sleep(10);
                            }

                            break; // Выходим из цикла обработки буфера
                        }
                    }
                }
                else if (waitRes == WAIT_OBJECT_0 + 1) {
                    // Stop Signal
                    break;
                }
                else {
                    // Timeout или ошибка
                    printf("[ERROR] Wait failed: %d\n", GetLastError());
                    Sleep(50);
                }
            }
            // ==============================================
            // MODE 2: ALLOWED (Polling System)
            // ==============================================
            else {
                // Отменяем любые pending запросы
                if (requestPending) {
                    CancelIoEx(hDevice, &overlapped);
                    DWORD temp;
                    GetOverlappedResult(hDevice, &overlapped, &temp, TRUE);
                    requestPending = FALSE;
                }

                // Опрашиваем систему
                if ((GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0) {
                    printf("\n[TRIGGER] Mouse 5 (System) -> BLOCKING...\n");

                    // ВАЖНО: Сначала меняем флаг
                    isBlocked = true;

                    // Потом отправляем команду драйверу
                    SetDriverBlockState(true);

                    // Debounce
                    while ((GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0) {
                        Sleep(10);
                    }

                    // НЕ отправляем IRP здесь! Это сделает следующая итерация цикла
                }
                else {
                    Sleep(10);
                }
            }
        }

        // Cleanup
        if (requestPending) {
            CancelIoEx(hDevice, &overlapped);
            DWORD temp;
            GetOverlappedResult(hDevice, &overlapped, &temp, TRUE);
        }

        CloseHandle(overlapped.hEvent);
        printf("[THREAD] Stopped.\n");
    }

public:
    MouHidTester() : hDevice(INVALID_HANDLE_VALUE), hStopEvent(NULL), workerThread(nullptr) {
        hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        isRunning = false;
        isBlocked = true; // Start blocked
    }

    ~MouHidTester() {
        Stop();
    }

    bool Start() {
        hDevice = CreateFileW(L"\\\\.\\MouHidInputHook", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (hDevice == INVALID_HANDLE_VALUE) return false;

        DWORD bytes;
        DeviceIoControl(hDevice, IOCTL_ENABLE_MOUHID_INPUT_MONITOR, NULL, 0, NULL, 0, &bytes, NULL);

        // Initial Block
        SetDriverBlockState(true);

        isRunning = true;
        ResetEvent(hStopEvent);
        workerThread = new std::thread(&MouHidTester::ThreadLoop, this);
        return true;
    }

    void Stop() {
        if (!isRunning) return;
        isRunning = false;
        SetEvent(hStopEvent);

        // Cancel IO just in case we are stuck
        if (hDevice != INVALID_HANDLE_VALUE) CancelIoEx(hDevice, NULL);

        if (workerThread) {
            if (workerThread->joinable()) workerThread->join();
            delete workerThread;
            workerThread = nullptr;
        }

        if (hDevice != INVALID_HANDLE_VALUE) {
            SetDriverBlockState(false);
            CloseHandle(hDevice);
            hDevice = INVALID_HANDLE_VALUE;
        }
    }

    void PrintStats() {
        // Simple console redraw to avoid flicker
        static HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        COORD c = { 0, 0 };
        SetConsoleCursorPosition(hOut, c);

        printf("============================================\n");
        printf(" MODE: %s                \n", isBlocked ? "[ BLOCKED ]" : "[ SYSTEM  ]");
        printf("============================================\n");
        printf(" Packets Read: %ld        \n", totalPackets);
        printf(" Last Delta:   X:%ld Y:%ld      \n", lastX, lastY);
        printf("--------------------------------------------\n");
        printf(" Press [SPACE] to toggle manually\n");
        printf(" Hold  [MOUSE 5] to toggle seamlessly\n");
        printf(" Press [ESC] to exit\n");
    }

    // Manual toggle for testing
    void ManualToggle() {
        bool newState = !isBlocked;
        SetDriverBlockState(newState);
        isBlocked = newState;
    }
};

int main() {
    MouHidTester app;

    if (app.Start()) {
        system("cls");
        while (true) {
            app.PrintStats();

            if (_kbhit()) {
                int k = _getch();
                if (k == 27) break; // ESC
                if (k == 32) app.ManualToggle(); // Space
            }
            Sleep(50);
        }
        app.Stop();
    }
    else {
        printf("Failed to connect! Run as Admin?\n");
        _getch();
    }
    return 0;
}