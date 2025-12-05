#include <windows.h>
#include <mmsystem.h>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstdio>

#include "Common/Settings.h"
#include "Patches.h"
#include "InputTweaks.h"
#include <shlwapi.h>

static DWORD* textscreen = nullptr;
static DWORD lastTextValue = 0;

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

struct AfsEntry { uint32_t offset; uint32_t size; };

static std::string g_dllDir;
static std::string g_logPath;
static std::string voicepath;
static bool g_enableLog = false;
extern "C" IMAGE_DOS_HEADER __ImageBase;

static std::vector<AfsEntry> g_afsTable;

static HWAVEOUT g_waveOut = NULL;
static WAVEHDR* g_waveHeader = NULL;
static BYTE* g_waveData = NULL;

static volatile LONG gStarted = 0;


static const int SEQ_COUNT = 12;
static const int MAX_SEQ_IDX = 11;


static void WriteLog(const char* fmt, ...);
static void StopAlertSound();
static bool LoadFileToMemory(const std::string& path, BYTE** outBuf, size_t* outSize);
static void ApplyGameMasterVolume();
static void PlayWavFromMemoryRange(BYTE* wavBuf, size_t wavSize, float startSec, float endSec);
static void InitVoiceAFS();
static bool PlayVoiceAfs(uint32_t index, float startSec, float endSec);


static void LoadSequenceTables(float* outStart, float* outEnd, int& outMax)
{
    static const float start[SEQ_COUNT] = { 0.0f,7.2f,13.0f,17.1f,19.2f,22.5f,35.2f,47.2f,53.4f,61.4f,64.0f, 0.0f };
    static const float end[SEQ_COUNT] = { 7.0f,12.4f,16.9f,19.0f,22.0f,34.5f,46.2f,52.2f,60.2f,63.5f,67.2f, 0.0f };

    memcpy(outStart, start, sizeof(float) * SEQ_COUNT);
    memcpy(outEnd, end, sizeof(float) * SEQ_COUNT);
    outMax = MAX_SEQ_IDX;
}

static bool ShouldActivateSequence(DWORD room, DWORD cutscene, DWORD fade, float cutsceneTime)
{
    return (room == R_HTL_RESTAURANT && cutscene == CS_HTL_LAURA_PIANO && fade == 3 && cutsceneTime >= 1650.000000);
}

static bool StabilizeCutscene()
{
    DWORD c1 = GetCutsceneID();
    float t1 = GetCutsceneTimer();

    bool ok1 = (c1 == CS_HTL_LAURA_PIANO && t1 >= 1600.0f);

    if (ok1)
        return true;

    Sleep(30);

    DWORD c2 = GetCutsceneID();
    float t2 = GetCutsceneTimer();

    bool ok2 = (c2 == CS_HTL_LAURA_PIANO && t2 >= 1600.0f);

    return ok2;
}

static DWORD ReadTextscreenSafe()
{
    if (!textscreen) return 0;
    return *textscreen;
}

static void WriteLog(const char* fmt, ...)
{
    if (!g_enableLog) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    std::ofstream f(g_logPath, std::ios::app);
    if (f.is_open()) { f << buf << "\n"; f.close(); }
}

static void CloseAudioDevice()
{
    if (g_waveOut)
    {
        waveOutClose(g_waveOut);
        g_waveOut = NULL;
        WriteLog("Audio device closed completely.");
    }
}

static void StopAlertSound()
{
    if (g_waveData)
    {
        delete[] g_waveData;
        g_waveData = NULL;
    }

    if (g_waveOut)
    {
        waveOutReset(g_waveOut);

        if (g_waveHeader)
        {
            waveOutUnprepareHeader(g_waveOut, g_waveHeader, sizeof(WAVEHDR));
            delete g_waveHeader;
            g_waveHeader = NULL;
            WriteLog("Current sound stopped and header unprepared.");
        }
    }
}

static void ResetState(int& sequenceIndex, bool& active, bool& initializedAfterStart)
{
    CloseAudioDevice();
    sequenceIndex = 0;
    lastTextValue = 0;
    active = false;
    initializedAfterStart = false;
}

static bool LoadFileToMemory(const std::string& path, BYTE** outBuf, size_t* outSize)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;

    size_t size = (size_t)f.tellg();
    if (size == 0) return false;

    BYTE* buf = new BYTE[size];
    f.seekg(0);
    f.read((char*)buf, size);

    if (!f) { delete[] buf; return false; }

    *outBuf = buf; *outSize = size; return true;
}

static void ApplyGameMasterVolume()
{
    int level = ConfigData.VolumeLevel;
    if (level < 0) level = 0; if (level > 15) level = 15;
    int percent = (int)((level / 15.0f) * 100.0f);
    DWORD vol = (DWORD)(65535 * (percent / 100.0f));
    DWORD stereoVol = (vol << 16) | vol;
    waveOutSetVolume((HWAVEOUT)g_waveOut, stereoVol);
    WriteLog("Master volume applied (SH2EE): %d%%  (level=%d)", percent, level);
}

static void PlayWavFromMemoryRange(BYTE* wavBuf, size_t wavSize, float startSec, float endSec)
{
    StopAlertSound();

    if (!wavBuf || wavSize < 44) { WriteLog("Invalid WAV."); return; }

    g_waveData = new BYTE[wavSize];
    memcpy(g_waveData, wavBuf, wavSize);

    uint32_t fmtSize; memcpy(&fmtSize, g_waveData + 16, sizeof(fmtSize));
    WAVEFORMATEX* wf = (WAVEFORMATEX*)(g_waveData + 20);
    DWORD dataPos = 20 + fmtSize + 8;
    if (dataPos >= wavSize) { WriteLog("Unexpected WAV format."); delete[] g_waveData; g_waveData = NULL; return; }

    BYTE* audioPtr = g_waveData + dataPos;
    DWORD audioSize = (DWORD)(wavSize - dataPos);

    DWORD bytesPerSample = (wf->wBitsPerSample / 8) * wf->nChannels;
    DWORD startByte = (DWORD)(startSec * wf->nSamplesPerSec * bytesPerSample);
    DWORD endByte = (DWORD)(endSec * wf->nSamplesPerSec * bytesPerSample);

    if (startByte > audioSize) startByte = 0; if (endByte > audioSize) endByte = audioSize;
    DWORD playBytes = endByte - startByte;

    if (!g_waveOut)
    {
        if (waveOutOpen(&g_waveOut, WAVE_MAPPER, wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        {
            WriteLog("waveOutOpen failed.");
            g_waveOut = NULL; delete[] g_waveData; g_waveData = NULL; return;
        }
        WriteLog("waveOutOpen initialized.");
    }

    g_waveHeader = new WAVEHDR(); memset(g_waveHeader, 0, sizeof(WAVEHDR));
    g_waveHeader->lpData = (LPSTR)(audioPtr + startByte);
    g_waveHeader->dwBufferLength = playBytes;

    if (EnableMasterVolume) ApplyGameMasterVolume(); else waveOutSetVolume((HWAVEOUT)g_waveOut, 0xFFFFFFFF);

    waveOutPrepareHeader(g_waveOut, g_waveHeader, sizeof(WAVEHDR));
    waveOutWrite(g_waveOut, g_waveHeader, sizeof(WAVEHDR));
}

static void InitVoiceAFS()
{
    std::ifstream f(voicepath, std::ios::binary);
    if (!f.is_open()) { WriteLog("InitVoiceAFS: failed to open AFS file."); return; }

    // read signature + fileCount (8 bytes)
    uint8_t header8[8];
    f.read((char*)header8, sizeof(header8));
    if (!f) { WriteLog("InitVoiceAFS: cannot read AFS header."); return; }

    if (memcmp(header8, "AFS\0", 4) != 0) { WriteLog("InitVoiceAFS: invalid AFS file."); return; }

    uint32_t fileCount = *(uint32_t*)(header8 + 4);
    WriteLog("InitVoiceAFS: fileCount=%u", fileCount);

    // validate reasonable fileCount
    if (fileCount == 0 || fileCount > 1000000) { WriteLog("InitVoiceAFS: suspicious fileCount=%u", fileCount); return; }

    // read just the offset/size table
    size_t tableSize = (size_t)fileCount * 8;
    std::vector<uint8_t> tableBuf(tableSize);
    f.read((char*)tableBuf.data(), tableSize);
    if (!f) { WriteLog("InitVoiceAFS: failed reading offset table."); return; }

    g_afsTable.clear();
    g_afsTable.resize(fileCount);
    for (uint32_t i = 0; i < fileCount; ++i)
    {
        size_t p = i * 8;
        g_afsTable[i].offset = *(uint32_t*)(tableBuf.data() + p);
        g_afsTable[i].size = *(uint32_t*)(tableBuf.data() + p + 4);
    }

    WriteLog("InitVoiceAFS: offset/size table loaded (no full-file allocation).");
}

static bool PlayVoiceAfs(uint32_t index, float startSec, float endSec)
{
    if (index >= g_afsTable.size()) { WriteLog("PlayVoiceAfs ERROR: invalid index %u", index); return false; }
    const AfsEntry& e = g_afsTable[index];
    WriteLog("PlayVoiceAfs: index=%u offset=0x%X size=%u", index, e.offset, e.size);

    std::ifstream f(voicepath, std::ios::binary);
    if (!f.is_open()) { WriteLog("PlayVoiceAfs ERROR: cannot open AFS."); return false; }

    f.seekg(e.offset, std::ios::beg);
    BYTE* wavData = new BYTE[e.size];
    f.read((char*)wavData, e.size);
    if (!f) { WriteLog("PlayVoiceAfs ERROR: reading block failed."); delete[] wavData; return false; }

    PlayWavFromMemoryRange(wavData, e.size, startSec, endSec);
    delete[] wavData; return true;
}

DWORD WINAPI AudioMonitorThread(LPVOID)
{
    char selfName[MAX_PATH];
    GetModuleFileNameA((HMODULE)&__ImageBase, selfName, MAX_PATH);
    strcpy_s(selfName, MAX_PATH, PathFindFileNameA(selfName));
    WriteLog("Monitor running inside DLL: %s", selfName);
    Sleep(3000);
    WriteLog("AudioMonitorThread started.");

    int sequenceIndex = 0;
    float seqStart[SEQ_COUNT]; float seqEnd[SEQ_COUNT]; int maxSeq;
    bool active = false;
    bool initializedAfterStart = false;

    while (true)
    {
        if (!GetModuleHandleA(selfName))
        {
            CloseAudioDevice();
            WriteLog("DLL unload detected. Terminating monitor thread.");
            return 0;
        }

        DWORD room = GetRoomID();
        DWORD cutscene = GetCutsceneID();
        DWORD fade = GetTransitionState();
        float cutsceneTime = GetCutsceneTimer();
        WriteLog("Cutscene Time = %f", cutsceneTime);

        // quick out if not in room
        if (room != R_HTL_RESTAURANT)
        {
            ResetState(sequenceIndex, active, initializedAfterStart);
            Sleep(5000);
            WriteLog("Room: %u (not restaurant). Reset state and sleeping.", room);
            continue;
        }

        // load sequence tables
        LoadSequenceTables(seqStart, seqEnd, maxSeq);

        // Activate sequence only when both cutscene and fade conditions are met
        if (!active)
        {
            if (ShouldActivateSequence(room, cutscene, fade, cutsceneTime))
            {
                WriteLog("Activation: room, cutscene, fade, and CutsceneTime OK. Enabling sequence.");
                active = true; sequenceIndex = 0; initializedAfterStart = false; lastTextValue = 0;
                Sleep(20);
                continue;
            }
            else
            {
                WriteLog("Waiting conditions: room=%u cutscene=%u fade=%u cutsceneTime=%u", room, cutscene, fade, cutsceneTime);
                Sleep(20);
                continue;
            }
        }

        if (!StabilizeCutscene())
        {
            WriteLog("Cutscene unstable. Resetting.");
            ResetState(sequenceIndex, active, initializedAfterStart);
            continue;
        }
      
        // safe read
        DWORD value = ReadTextscreenSafe();

        // first read after activation only initializes lastTextValue
        if (!initializedAfterStart)
        {
            WriteLog("[INIT] First read after activation: lastTextValue=%u", value);
            lastTextValue = value;
            initializedAfterStart = true;
            Sleep(20);
            continue;
        }

        // play on change (non-zero and different)
        if (value != 0 && value != lastTextValue)
        {
            WriteLog("[PLAY] Change detected: last=%u : value=%u (seq=%d)", lastTextValue, value, sequenceIndex);

            // Only play the audio if the current sequence is smaller than the maximum sequence.
            if (sequenceIndex <= maxSeq)
                PlayVoiceAfs(119, seqStart[sequenceIndex], seqEnd[sequenceIndex]);

            lastTextValue = value;
            sequenceIndex++;
        }
        else
        {
            WriteLog("[NOCHANGE] value=%u | last=%u", value, lastTextValue);
        }

        // if we finished all sequences, wait until cutscene OR room change, then reset and require new activation
        if (sequenceIndex > maxSeq)
        {
            WriteLog("[END] Final sequence reached. Stopping audio and waiting for room/cutscene change...");
            CloseAudioDevice();

            while (true)
            {
                if (!GetModuleHandleA(selfName))
                {
                    WriteLog("DLL unload detected. Terminating monitor thread.");
                    return 0;
                }

                if (!StabilizeCutscene())
                {
                    WriteLog("Cutscene unstable. Resetting.");
                    ResetState(sequenceIndex, active, initializedAfterStart);
                    WriteLog("[RESET] Values reset. Waiting for next activation (fade==3).");
                    break;
                }
                else
                {
                    WriteLog("It's a false positive.");
                }

                Sleep(200);
            }

            continue;
        }

        Sleep(20);
    }
}

void PatchUnusedAudio()
{
    // LOCATE TARGET ADDRESS
    BYTE pattern[] = { 0xA3, 0xEC, 0x03, 0xF6, 0x01 };
    BYTE* addr = (BYTE*)SearchAndGetAddresses(
        0x0048042F, 0x004806AF, 0x0047FE3F,
        pattern, sizeof(pattern), 0, __FUNCTION__);

    if (!addr) WriteLog("ERROR: Could not locate sh2pc.exe+1B603EC through pattern scan!");
    else
    {
        textscreen = (DWORD*)(*(DWORD*)(addr + 1));
        WriteLog("Found target address = %p", textscreen);
    }

    if (InterlockedExchange(&gStarted, 1) != 0) return;

    char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string s = path; size_t p = s.find_last_of("\\/"); if (p != std::string::npos) s = s.substr(0, p);
    g_dllDir = s; g_logPath = g_dllDir + "\\PlayUnusedAudio.log";

    { std::ifstream lf(g_logPath); g_enableLog = lf.is_open(); }
    WriteLog("Starting PlayUnusedAudio...");

    voicepath = g_dllDir + "\\sh2e\\sound\\adx\\voice\\voice.afs";
    {
        std::ifstream test(voicepath, std::ios::binary);
        if (!test.is_open()) { WriteLog("voice.afs NOT found: %s", voicepath.c_str()); WriteLog("Monitor will not start. Restart the game and try again."); return; }
    }

    WriteLog("AFS file found. Continuing...");
    InitVoiceAFS();
    CreateThread(NULL, 0, AudioMonitorThread, NULL, 0, NULL);
}
