typedef struct IUnknown IUnknown;

#include <Windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <iostream>
#include <TlHelp32.h>
#include "dsound.h"

#include "include/MinHook.h"
#include "include/InjectHook.h"
#include "Hooks.h"

#pragma comment(lib,"user32.lib") 
#pragma comment(lib,"libs\\libMinHook.x86.lib")

bool isTextSection(const BYTE* arr, size_t size) {
    const char* textSection = ".text";
    return size >= strlen(textSection) && memcmp(arr, textSection, strlen(textSection)) == 0;
}

bool isDataSection(const BYTE* arr, size_t size) {
    const char* textSection = ".data";
    return size >= strlen(textSection) && memcmp(arr, textSection, strlen(textSection)) == 0;
}

void PatchMemory(uintptr_t targetAddress, BYTE jmpOpcode[], int codeSize)
{
    // Write the modified instruction to the process
    SIZE_T bytesWritten;

    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(targetAddress), jmpOpcode, codeSize, &bytesWritten);
}

void PatchMemory(uintptr_t targetAddress, void* value, int codeSize)
{
    // Write the modified instruction to the process
    SIZE_T bytesWritten;

    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(targetAddress), value, codeSize, &bytesWritten);
}

LPDIRECTSOUNDBUFFER* DSndBuf;

int volume = 100;

void SetVolumeDirect() {
	if (DSndBuf == NULL) return;

    if (volume < 0) volume = 0;
    if (volume > 100) {
        volume = 100;
    }

    if (volume == 100)
    {
        return;
    }

    if (volume == 0)
    {
		(*DSndBuf)->SetVolume(-10000);
    }
    else 
    {
        int txtValue = volume; //aka the value set in the text file
        float subtractValue = 415 * log10(txtValue / 100.0f) + 415;
        int newValue = round(20 * log10(txtValue / 100.0f) * 100 - subtractValue);

        (*DSndBuf)->SetVolume(newValue);
    }
}

HRESULT DSoundCreateBuffer(IDirectSound8* directSound, LPCDSBUFFERDESC bufferDesc, LPDIRECTSOUNDBUFFER* soundBuffer, LPUNKNOWN wah)
{
    //LPDIRECTSOUNDBUFFER soundBuffer = nullptr;

    if (directSound)
    {
        HRESULT hr = directSound->CreateSoundBuffer(bufferDesc, soundBuffer, wah);
        if (FAILED(hr))
        {
            return hr; // Handle failure
        }
    }

    return DS_OK;
}

HRESULT __fastcall DSoundSetVol(int32_t arg1, void* ebx, IDirectSoundBuffer* arg2, long gameVolume)
{
    // Ensure volume is clamped between 0 and 100
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    if (volume != 100) {
        if (volume == 0) {
            arg2->SetVolume(-10000);
        }
        else {
            int posVolume = -gameVolume;
            int txtValue = volume; //aka the value set in the text file
            float subtractValue = posVolume * log10(txtValue / 100.0f) + posVolume;
            int newValue = round(20 * log10(txtValue / 100.0f) * 100 - subtractValue);

            arg2->SetVolume(newValue);
        }
    }
    else
    {
        arg2->SetVolume(gameVolume);
    }

    return 0;
}

int callSetVolumeAddr = 0;
int callSetVolumeAddrRet = 0;

int callCreateSndBufferAddr = 0;
int callCreateSndBufferAddrRet = 0;

__declspec(naked) void CreateSndBufferHook() {

    __asm {
        mov DSndBuf, ebx

        call dword ptr[eax + 0x0C]

        push eax
        push ecx
        push edx

		call SetVolumeDirect

        pop edx
        pop ecx
        pop eax

        mov edi, eax

        jmp callCreateSndBufferAddrRet
    }
}

__declspec(naked) void SetVolumeHook() {

    __asm {
        call DSoundSetVol

        mov eax, [edi + 0x28];

        jmp callSetVolumeAddrRet
    }
}

void setVolume() {
    FILE* fptr;
    // Open a file in read mode
    fptr = fopen("volumeSFX.txt", "r");
    if (fptr == NULL) {
        volume = 100;
    }
    else {
        // Store the content of the file
        char strVol[4];
        // Read the content and store it inside strVol
        fgets(strVol, 4, fptr);
        // Close the file
        fclose(fptr);

        char* endptr;
        int newVol = strtol(strVol, &endptr, 10);

        if (*endptr != '\0' || endptr == strVol) {
            //Invalid number, set to default
            volume = 100;
        }
        else {
            //Set Volume to the number in the file
            volume = newVol;
        }
    }
}

struct ThreadData {
    HANDLE directoryHandle;
    const wchar_t* directoryPath;
    const wchar_t* targetFileName;
};

void MonitorDirectoryThread(void* data) {
    struct ThreadData* threadData = (struct ThreadData*)data;
    HANDLE directoryHandle = threadData->directoryHandle;
    const wchar_t* directoryPath = threadData->directoryPath;
    const wchar_t* targetFileName = threadData->targetFileName;

    // Buffer to store the changes
    const int bufferSize = 4096;
    BYTE buffer[4096];

    DWORD bytesRead;
    FILE_NOTIFY_INFORMATION* fileInfo;

    while (ReadDirectoryChangesW(
        directoryHandle,
        buffer,
        bufferSize,
        FALSE, // Ignore subtree
        FILE_NOTIFY_CHANGE_LAST_WRITE, // Monitor file write changes
        &bytesRead,
        NULL,
        NULL
    )) {
        fileInfo = (FILE_NOTIFY_INFORMATION*)buffer;

        //Make sure that the file that got written to is the file we are monitoring
        if (wcsncmp(fileInfo->FileName, targetFileName, fileInfo->FileNameLength / sizeof(wchar_t)) != 0)
            continue;

        do {

            switch (fileInfo->Action) {
            case FILE_ACTION_MODIFIED:
                setVolume();
                break;
            default:
                break;
            }

            // Move to the next entry in the buffer
            fileInfo = (FILE_NOTIFY_INFORMATION*)((char*)fileInfo + fileInfo->NextEntryOffset);

        } while (fileInfo->NextEntryOffset != 0);
    }

    // Close the directory handle when the monitoring loop exits
    CloseHandle(directoryHandle);
}

void MonitorDirectory(const wchar_t* directoryPath, const wchar_t* targetFileName)
{
    // Create a directory handle
    HANDLE directoryHandle = CreateFileW(
        directoryPath,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL
    );

    if (directoryHandle == INVALID_HANDLE_VALUE) {
        wprintf(L"Error opening directory: %d\n", GetLastError());
        return;
    }

    // Prepare data to pass to the thread
    struct ThreadData* threadData = (struct ThreadData*)malloc(sizeof(struct ThreadData));
    if (threadData == NULL) {
        wprintf(L"Memory allocation failed\n");
        CloseHandle(directoryHandle);
        return;
    }
    threadData->directoryHandle = directoryHandle;
    threadData->directoryPath = directoryPath;
    threadData->targetFileName = targetFileName;

    // Create a thread for monitoring
    HANDLE threadHandle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)MonitorDirectoryThread, threadData, 0, NULL);

    //Closes the handle to the thread, however this does not stop the thread
    CloseHandle(threadHandle);
}


bool Initialized = false;

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        callSetVolumeAddr = (int)Hooks::scanPattern2("DSoundSetVolume", "FF 53 3C 8B 47 28 6A 00 50 8B 18");
        callSetVolumeAddrRet = callSetVolumeAddr + 6;

        callCreateSndBufferAddr = (int)Hooks::scanPattern2("DSoundCreateSndBuffer", "FF 50 0C 8B F8 85 FF 75 2E 8B");
        callCreateSndBufferAddrRet = callCreateSndBufferAddr + 5;

        try {
            if (callSetVolumeAddr != NULL)
                HookLib::InstallHook(reinterpret_cast<LPVOID>(callSetVolumeAddr), &SetVolumeHook, HookLib::JMP_LONG, true);

            if (callCreateSndBufferAddr != NULL)
                HookLib::InstallHook(reinterpret_cast<LPVOID>(callCreateSndBufferAddr), &CreateSndBufferHook, HookLib::JMP_LONG, false);
            }
        catch (...)
        {

        }

        //Gets the current working directory, and creates a path containing it and the volumeSFX.txt file that we want to monitor for changes
        wchar_t directoryPath[1024];
        _wgetcwd(directoryPath, sizeof(directoryPath) / sizeof(directoryPath[0]));
        const wchar_t* targetFileName = L"volumeSFX.txt";
        MonitorDirectory(directoryPath, targetFileName);

        //Load the volume
        setVolume();
    }
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

