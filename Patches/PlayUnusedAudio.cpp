#include <windows.h>
#include <mmsystem.h>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstdio>

#include "Common/Settings.h"

#pragma comment(lib, "winmm.lib")

struct AfsEntry
{
    uint32_t offset;
    uint32_t size;
};

static std::string g_dllDir;
static std::string g_logPath;
static std::string voicepath;
static bool g_enableLog = false;

static std::vector<AfsEntry> g_afsTable;

static HWAVEOUT g_waveOut = NULL;
static WAVEHDR* g_waveHeader = NULL;
static BYTE* g_waveData = NULL;

static volatile LONG gStarted = 0;

// ======================================================
// LOG
// ======================================================
static void WriteLog(const char* fmt, ...)
{
    //Logs will only be generated if the PlayUnusedAudio.log file already exists.
    if (!g_enableLog)
        return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    std::ofstream f(g_logPath, std::ios::app);
    if (f.is_open())
    {
        f << buf << "\n";
        f.close();
    }
}

// ======================================================
// STOP AUDIO
// ======================================================
static void StopAlertSound()
{
    if (g_waveOut && g_waveHeader)
    {
        waveOutReset(g_waveOut);
        waveOutUnprepareHeader(g_waveOut, g_waveHeader, sizeof(WAVEHDR));
        delete g_waveHeader;
        g_waveHeader = NULL;

        WriteLog("Sound stopped.");
    }

    if (g_waveData)
    {
        delete[] g_waveData;
        g_waveData = NULL;
    }
}

// ======================================================
// LOAD FILE TO MEMORY
// ======================================================
static bool LoadFileToMemory(const std::string& path, BYTE** outBuf, size_t* outSize)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;

    size_t size = (size_t)f.tellg();
    if (size == 0) return false;

    BYTE* buf = new BYTE[size];
    f.seekg(0);
    f.read((char*)buf, size);

    if (!f)
    {
        delete[] buf;
        return false;
    }

    *outBuf = buf;
    *outSize = size;
    return true;
}

static void ApplyGameMasterVolume()
{
    //It will play the audio at the volume specified by MasterVolume.

    int level = ConfigData.VolumeLevel;
    if (level < 0) level = 0;
    if (level > 15) level = 15;

    int percent = (int)((level / 15.0f) * 100.0f);

    DWORD vol = (DWORD)(65535 * (percent / 100.0f));
    DWORD stereoVol = (vol << 16) | vol;

    waveOutSetVolume(0, stereoVol);

    WriteLog("Master volume applied (SH2EE): %d%%  (level=%d)", percent, level);
}

// ======================================================
// PLAY WAV FROM MEMORY RANGE
// ======================================================
static void PlayWavFromMemoryRange(BYTE* wavBuf, size_t wavSize, float startSec, float endSec)
{
    StopAlertSound();

    if (!wavBuf || wavSize < 44)
    {
        WriteLog("Invalid WAV.");
        return;
    }

    g_waveData = new BYTE[wavSize];
    memcpy(g_waveData, wavBuf, wavSize);

    uint32_t fmtSize;
    memcpy(&fmtSize, g_waveData + 16, sizeof(fmtSize));

    WAVEFORMATEX* wf = (WAVEFORMATEX*)(g_waveData + 20);
    DWORD dataPos = 20 + fmtSize + 8;

    if (dataPos >= wavSize)
    {
        WriteLog("Unexpected WAV format.");
        delete[] g_waveData;
        g_waveData = NULL;
        return;
    }

    BYTE* audioPtr = g_waveData + dataPos;
    DWORD audioSize = (DWORD)(wavSize - dataPos);

    DWORD bytesPerSample = (wf->wBitsPerSample / 8) * wf->nChannels;
    DWORD startByte = (DWORD)(startSec * wf->nSamplesPerSec * bytesPerSample);
    DWORD endByte = (DWORD)(endSec * wf->nSamplesPerSec * bytesPerSample);

    if (startByte > audioSize) startByte = 0;
    if (endByte > audioSize) endByte = audioSize;

    DWORD playBytes = endByte - startByte;

    if (!g_waveOut)
    {
        if (waveOutOpen(&g_waveOut, WAVE_MAPPER, wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        {
            WriteLog("waveOutOpen failed.");
            g_waveOut = NULL;
            delete[] g_waveData;
            g_waveData = NULL;
            return;
        }

        WriteLog("waveOutOpen initialized.");
    }

    g_waveHeader = new WAVEHDR();
    memset(g_waveHeader, 0, sizeof(WAVEHDR));

    g_waveHeader->lpData = (LPSTR)(audioPtr + startByte);
    g_waveHeader->dwBufferLength = playBytes;

    if (EnableMasterVolume)
        ApplyGameMasterVolume();

    // If Master Volume is disabled, the audio volume will be 100 % .
    else
        waveOutSetVolume(0, 0xFFFFFFFF);

    waveOutPrepareHeader(g_waveOut, g_waveHeader, sizeof(WAVEHDR));
    waveOutWrite(g_waveOut, g_waveHeader, sizeof(WAVEHDR));
}

// ======================================================
// PLAY FROM AFS BY OFFSET/SIZE
// ======================================================
static void PlayAFSByOffset(uint32_t offset, uint32_t size, float startSec, float endSec)
{
    WriteLog("PlayAFSByOffset: offset=0x%X size=%u", offset, size);

    std::ifstream f(voicepath, std::ios::binary);
    if (!f.is_open())
    {
        WriteLog("ERROR: Failed to open AFS.");
        return;
    }

    if ((uint64_t)offset + size > (uint64_t)SIZE_MAX)
    {
        WriteLog("ERROR: offset+size overflow.");
        return;
    }

    f.seekg(offset, std::ios::beg);

    BYTE* block = new BYTE[size];
    f.read((char*)block, size);

    if (!f)
    {
        WriteLog("Error reading WAV block from AFS.");
        delete[] block;
        return;
    }

    PlayWavFromMemoryRange(block, size, startSec, endSec);

    delete[] block;
}

static void InitVoiceAFS()
{
    BYTE* buf = nullptr;
    size_t sz = 0;

    if (!LoadFileToMemory(voicepath, &buf, &sz))
    {
        WriteLog("InitVoiceAFS: failed to open AFS file.");
        return;
    }

    if (sz < 16 || memcmp(buf, "AFS\0", 4) != 0)
    {
        WriteLog("InitVoiceAFS: invalid AFS file.");
        delete[] buf;
        return;
    }

    uint32_t fileCount = *(uint32_t*)(buf + 4);
    WriteLog("InitVoiceAFS: fileCount=%u", fileCount);

    size_t tableStart = 8;
    size_t tableSize = fileCount * 8;

    if (tableStart + tableSize > sz)
    {
        WriteLog("InitVoiceAFS: offset/size table exceeds file size.");
        delete[] buf;
        return;
    }

    g_afsTable.resize(fileCount);

    for (uint32_t i = 0; i < fileCount; i++)
    {
        size_t p = tableStart + i * 8;
        g_afsTable[i].offset = *(uint32_t*)(buf + p);
        g_afsTable[i].size = *(uint32_t*)(buf + p + 4);
    }

    delete[] buf;

    WriteLog("InitVoiceAFS: offset/size table loaded.");
}

// ======================================================
// PLAY BY INDEX
// ======================================================
static void PlayByIndex(uint32_t index, float startSec, float endSec)
{
    if (index >= g_afsTable.size())
    {
        WriteLog("Index out of range: %u", index);
        return;
    }

    const AfsEntry& e = g_afsTable[index];

    WriteLog("PlayByIndex: index=%u offset=0x%X size=%u", index, e.offset, e.size);

    PlayAFSByOffset(e.offset, e.size, startSec, endSec);
}

DWORD WINAPI AudioMonitorThread(LPVOID)
{
    Sleep(3000);
    WriteLog("AudioMonitorThread started.");

    HMODULE hGame = GetModuleHandleA("sh2pc.exe");
    if (!hGame)
    {
        WriteLog("Could not find sh2pc.exe.");
        return 0;
    }

    BYTE* base = (BYTE*)hGame;

    const DWORD addrReadyCheck = 0x006C7228;

    const DWORD addrLanguage = 0x00532B5C;
    const DWORD addrJapCheck = 0x01B603EC;
    const DWORD addrSub = 0x019BC007;

    const DWORD addrStartLetter = 0x01B7A944;

    const DWORD addrCheck1 = 0x01B80BC0;
    const DWORD addrCheck2 = 0x00644198;
    const DWORD addrEvent = 0x01B5FEC4;

    int lastEventValue = 0;
    int sequenceIndex = 0;

    float seqStart[12];
    float seqEnd[12];
    int   maxSeq;

    uint16_t ev;
    uint16_t begintext;

    static bool startLetterTriggered = false;

    while (true)
    {
        //--------------------------------------------------------
        // Before checking other addresses and values, verify that the current room is the hotel's restaurant.
        //--------------------------------------------------------

        uint8_t ready = *(uint8_t*)(base + addrReadyCheck);
        uint8_t StartLetter = *(uint8_t*)(base + addrStartLetter);

        if (ready != 152)
        {
            StopAlertSound();
            lastEventValue = 0;
            sequenceIndex = 0;

            WriteLog("Waiting for value 152 at 6C7228... (current value=%u)", ready);

            Sleep(5000);
            continue;
        }

        uint8_t  LanguageCheck = *(uint8_t*)(base + addrLanguage);
        uint8_t vCheck1 = *(uint8_t*)(base + addrCheck1);
        uint8_t vCheck2 = *(uint8_t*)(base + addrCheck2);

        if (!startLetterTriggered)
        {
            if (StartLetter == 3)
            {
                startLetterTriggered = true;
                WriteLog("StartLetter == 3 detected. Continuing normally...");
            }
            else
            {
                WriteLog("StartLetter is not 3. current=%u", StartLetter);
                Sleep(20);
                continue;
            }
        }

        if (ready != 152)
        {
            WriteLog("READY lost! Returning to initial state.");

            StopAlertSound();
            startLetterTriggered = false;
            lastEventValue = 0;
            sequenceIndex = 0;

            Sleep(200);
            continue;
        }

        // The Japanese language is unique in that some values ??are different, so if the language is Japanese, the rule will change slightly.

        if (LanguageCheck != 232)
        {
            WriteLog("Language is NOT Japanese.");
            ev = *(uint16_t*)(base + addrEvent);
            begintext = 0;

            float startEng[11] =
            { 0.0f,7.2f,13.0f,17.1f,19.2f,22.5f,35.2f,47.2f,53.4f,61.0f,64.0f };

            float endEng[11] =
            { 7.0f,12.4f,16.9f,19.0f,22.0f,34.5f,46.2f,52.2f,60.2f,63.5f,67.2f };

            memcpy(seqStart, startEng, sizeof(startEng));
            memcpy(seqEnd, endEng, sizeof(endEng));
            maxSeq = 10;
        }
        else
        {
            WriteLog("Language is Japanese.");
            ev = *(uint16_t*)(base + addrJapCheck);
            begintext = 44906;

            uint8_t SubCheck = *(uint16_t*)(base + addrSub);

            if (SubCheck == 0)
            {
                WriteLog("Subtitles OFF.");
                float startJap[12] =
                { 0.0f,0.0f,7.2f,13.0f,17.1f,19.2f,22.5f,35.2f,47.2f,53.4f,61.0f,64.0f };

                float endJap[12] =
                { 0.0f,6.2f,12.4f,16.5f,19.0f,22.0f,34.5f,46.2f,52.2f,60.2f,63.0f,67.2f };

                memcpy(seqStart, startJap, sizeof(startJap));
                memcpy(seqEnd, endJap, sizeof(endJap));
                maxSeq = 11;
            }
            else
            {
                WriteLog("Subtitles ON.");
                float startJap[11] =
                { 0.0f,7.2f,13.0f,17.1f,19.2f,22.5f,35.2f,47.2f,53.4f,61.0f,64.0f };

                float endJap[11] =
                { 6.2f,12.4f,16.5f,19.0f,22.0f,34.5f,46.2f,52.2f,60.2f,63.0f,67.2f };

                memcpy(seqStart, startJap, sizeof(startJap));
                memcpy(seqEnd, endJap, sizeof(endJap));
                maxSeq = 10;
            }
        }

        WriteLog("DBG: vCheck1=%u, vCheck2=%u, EVENT=%u", vCheck1, vCheck2, ev);

        if (vCheck1 != 96 || vCheck2 != 100)
        {
            WriteLog("Conditions failed. Reset.");
            StopAlertSound();
            lastEventValue = begintext;
            sequenceIndex = 0;
            startLetterTriggered = false;

            Sleep(200);
            continue;
        }

        if (ev == begintext)
        {
            WriteLog("EVENT return to 0. Reset.");
            StopAlertSound();
            sequenceIndex = 0;
            lastEventValue = 0;
            Sleep(20);
            continue;
        }

        if (ev != lastEventValue)
        {
            WriteLog("New event detected: %u ", ev, lastEventValue);

            int idx = sequenceIndex;
            if (idx > maxSeq) idx = maxSeq;

            PlayByIndex(119, seqStart[idx], seqEnd[idx]);

            sequenceIndex++;
            if (sequenceIndex > maxSeq)
                sequenceIndex = maxSeq;

            lastEventValue = ev;
        }

        Sleep(20);
    }
}

// ======================================================
// START
// ======================================================
void PatchUnusedAudio()
{
    if (InterlockedExchange(&gStarted, 1) != 0) return;

    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);

    std::string s = path;
    size_t p = s.find_last_of("\\/");
    if (p != std::string::npos) s = s.substr(0, p);

    g_dllDir = s;

    //Logs will only be generated if the PlayUnusedAudio.log file already exists.
    g_logPath = g_dllDir + "\\PlayUnusedAudio.log";

    {
        std::ifstream lf(g_logPath);
        g_enableLog = lf.is_open();
    }

    WriteLog("Starting PlayUnusedAudio...");

    // AFS path
    voicepath = g_dllDir + "\\sh2e\\sound\\adx\\voice\\voice.afs";

    // Checks if afs exists
    {
        std::ifstream test(voicepath, std::ios::binary);
        if (!test.is_open())
        {
            WriteLog("voice.afs NOT found: %s", voicepath.c_str());
            WriteLog("Monitor will not start. Restart the game and try again.");
            return;
        }
    }

    WriteLog("AFS file found. Continuing...");

    InitVoiceAFS();

    CreateThread(NULL, 0, AudioMonitorThread, NULL, 0, NULL);
}
