#include "camera_avoidance_fix.h"
#include <shlobj.h>
#include <tlhelp32.h>
#include <share.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace {
std::atomic_flag started = ATOMIC_FLAG_INIT;

class StartupLog {
public:
    ~StartupLog() { if (file_) fclose(file_); }
    bool Open() {
        PWSTR appData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &appData))) return false;
        struct FreePath { PWSTR value; ~FreePath() { CoTaskMemFree(value); } } cleanup{appData};
        std::wstring directory = appData;
        directory += LR"(\Larian Studios\Baldur's Gate 3\NativeMods)";
        auto result = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
        if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS) return false;
        file_ = _wfsopen((directory + L"\\BG3CameraRaycastFix.log").c_str(), L"w", _SH_DENYNO);
        return file_ != nullptr;
    }
    void Write(char const* message) {
        if (file_) { fprintf(file_, "%llu %s\n", GetTickCount64(), message); fflush(file_); }
        char line[600];
        snprintf(line, sizeof(line), "[BG3 Camera Raycast Fix] %s\r\n", message);
        OutputDebugStringA(line);
        auto output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        if (output && output != INVALID_HANDLE_VALUE)
            WriteConsoleA(output, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    }
private:
    FILE* file_{};
};

class Threads {
public:
    ~Threads() {
        Resume();
        for (auto const& thread : threads_) CloseHandle(thread.handle);
    }
    DWORD Collect() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return GetLastError();
        struct CloseSnapshot { HANDLE value; ~CloseSnapshot() { CloseHandle(value); } } cleanup{snapshot};
        THREADENTRY32 entry{sizeof(entry)};
        if (!Thread32First(snapshot, &entry)) return GetLastError();
        do {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
            auto handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ThreadID);
            if (!handle) {
                auto error = GetLastError();
                if (error == ERROR_INVALID_PARAMETER) continue; // Thread exited since the snapshot.
                return error;
            }
            // The snapshot's thread ID can be recycled before OpenThread.
            auto owner = GetProcessIdOfThread(handle);
            if (!owner) { auto error = GetLastError(); CloseHandle(handle); return error; }
            if (owner != GetCurrentProcessId()) { CloseHandle(handle); continue; }
            try { threads_.push_back({handle, false}); }
            catch (...) { CloseHandle(handle); throw; }
        } while (Thread32Next(snapshot, &entry));
        auto error = GetLastError();
        return error == ERROR_NO_MORE_FILES ? ERROR_SUCCESS : error;
    }
    DWORD Pause(CameraAvoidanceFix::Patch const& patch) {
        // All storage and handles were acquired before suspending any thread.
        // No heap, logging, loader operations, or blocking waits until Resume().
        for (auto& thread : threads_) {
            if (WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0) continue;
            if (SuspendThread(thread.handle) == static_cast<DWORD>(-1)) {
                auto error = GetLastError();
                if (WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0) continue;
                return error;
            }
            thread.paused = true;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(thread.handle, &context)) {
                auto error = GetLastError();
                if (WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0) { thread.paused = false; continue; }
                return error;
            }
            if (patch.ContainsInstruction(context.Rip)) return ERROR_BUSY;
        }
        return ERROR_SUCCESS;
    }
    bool Resume() {
        bool success = true;
        for (auto& thread : threads_) if (thread.paused) {
            if (ResumeThread(thread.handle) != static_cast<DWORD>(-1)
                || WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0) thread.paused = false;
            else success = false;
        }
        return success;
    }
private:
    struct Thread { HANDLE handle; bool paused; };
    std::vector<Thread> threads_;
};

void Start(StartupLog& log) {
    CameraAvoidanceFix::Patch patch;
    std::string reason;
    if (!patch.Prepare(GetModuleHandleW(nullptr), reason)) {
        log.Write(("SKIPPED: " + reason).c_str());
        return;
    }
    Threads threads;
    auto error = threads.Collect();
    if (error == ERROR_SUCCESS) error = threads.Pause(patch);
    bool applied = false, restored = true;
    if (error == ERROR_SUCCESS) {
        applied = patch.Apply();
        if (!applied) {
            restored = patch.Rollback();
            if (!restored) restored = patch.Rollback(); // One bounded retry for transient API failure.
        }
    }
    bool resumed = threads.Resume();
    if (!resumed) resumed = threads.Resume();
    if (!resumed) log.Write("WARNING: could not resume every suspended thread.");
    if (!restored) log.Write("WARNING: patch rollback failed.");
    if (error == ERROR_BUSY) {
        log.Write("SKIPPED: camera helper is executing; no code modified.");
    } else if (error != ERROR_SUCCESS) {
        char message[160];
        snprintf(message, sizeof(message), "SKIPPED: could not safely suspend game threads; Windows error=%lu. No code modified.", error);
        log.Write(message);
    } else if (!applied) {
        log.Write("FAILED: instruction update refused or failed.");
    } else {
        char message[160];
        snprintf(message, sizeof(message), "ACTIVE: hit-position fix %s; helper RVA=%x.", patch.AlreadyCorrected() ? "already present" : "installed", patch.FunctionRva());
        log.Write(message);
    }
}
}

// Native Mod Loader invokes Init after LoadLibrary, outside the loader lock,
// before game CRT startup. No worker thread or Script Extender discovery needed.
extern "C" __declspec(dllexport) void Init() {
    if (started.test_and_set()) return;
    StartupLog log;
    try {
        if (!log.Open()) log.Write("WARNING: startup file log unavailable.");
        Start(log);
    } catch (std::exception const& error) {
        log.Write(error.what());
        log.Write("FAILED: unexpected startup exception.");
    } catch (...) {
        log.Write("FAILED: unexpected startup exception.");
    }
}

// The fix changes only game instruction operands; no callbacks point into this DLL.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
