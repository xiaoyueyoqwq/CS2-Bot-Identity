#include "shm_pub.h"

#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace botid {

// Persistent shared memory state
static int s_ShmFd = -1;
static unsigned char* s_ShmView = nullptr;

static unsigned char* SlotStatePtr() {
    return s_ShmView + kOff_SlotState;
}

static uint64_t* SidPtr(int slot) {
    return reinterpret_cast<uint64_t*>(s_ShmView + kOff_SyntheticSid + slot * 8);
}

static char* NamePtr(int slot) {
    return reinterpret_cast<char*>(s_ShmView + kOff_PersonaName + slot * kShmNameLen);
}

static uint64_t* IncarnationPtr(int slot) {
    return reinterpret_cast<uint64_t*>(s_ShmView + kOff_Incarnation + slot * 8);
}

static int* PingPtr(int slot) {
    return reinterpret_cast<int*>(s_ShmView + kOff_CurrentPing + slot * 4);
}

static char* CrosshairPtr(int slot) {
    return reinterpret_cast<char*>(s_ShmView + kOff_Crosshair + slot * kCrosshairLen);
}

static uint32_t* FlairPtr(int slot) {
    return reinterpret_cast<uint32_t*>(s_ShmView + kOff_ScoreboardFlair + slot * 4);
}

bool InitSharedMemory() {
    if (s_ShmView) return true;

    s_ShmFd = shm_open("/CS2BotHider_Slots", O_RDWR, 0600);
    if (s_ShmFd < 0) {
        // C# BotHiderImpl may not have created it yet — create ourselves
        s_ShmFd = shm_open("/CS2BotHider_Slots", O_RDWR | O_CREAT, 0600);
        if (s_ShmFd < 0) return false;
        if (ftruncate(s_ShmFd, (off_t)kShmTotalSize) != 0) {
            close(s_ShmFd);
            s_ShmFd = -1;
            return false;
        }
    }

    s_ShmView = reinterpret_cast<unsigned char*>(
        mmap(nullptr, kShmTotalSize, PROT_READ | PROT_WRITE, MAP_SHARED, s_ShmFd, 0));
    if (s_ShmView == MAP_FAILED) {
        s_ShmView = nullptr;
        close(s_ShmFd);
        s_ShmFd = -1;
        return false;
    }
    close(s_ShmFd);
    s_ShmFd = -1;
    return true;
}

void PublishAdopt(int slot, uint64_t steamId, const char* name) {
    if (slot < 0 || slot >= kShmMaxSlots) return;
    if (!s_ShmView) {
        // shm_init may have failed; skip silently
        return;
    }

    SlotStatePtr()[slot] = 1;
    *SidPtr(slot) = steamId;
    msync(s_ShmView, kShmTotalSize, MS_SYNC);

    if (name) {
        std::memset(NamePtr(slot), 0, kShmNameLen);
        std::strncpy(NamePtr(slot), name, kShmNameLen - 1);
        msync(s_ShmView, kShmTotalSize, MS_SYNC);
    }
}

void PublishRelease(int slot) {
    if (!s_ShmView || slot < 0 || slot >= kShmMaxSlots) return;
    SlotStatePtr()[slot] = 0;
    *SidPtr(slot) = 0;
    std::memset(NamePtr(slot), 0, kShmNameLen);
}

void PublishPing(int slot, int ping) {
    if (!s_ShmView || slot < 0 || slot >= kShmMaxSlots) return;
    *PingPtr(slot) = ping;
    msync(s_ShmView, kShmTotalSize, MS_ASYNC);
}

void PublishCrosshair(int slot, const char* code) {
    if (!s_ShmView || slot < 0 || slot >= kShmMaxSlots) return;
    if (!code) code = "";
    char* dst = CrosshairPtr(slot);
    std::memset(dst, 0, kCrosshairLen);
    std::strncpy(dst, code, kCrosshairLen - 1);
    msync(s_ShmView, kShmTotalSize, MS_ASYNC);
}

void PublishScoreboardFlair(int slot, uint32_t itemDefIndex) {
    if (!s_ShmView || slot < 0 || slot >= kShmMaxSlots) return;
    *FlairPtr(slot) = itemDefIndex;
    msync(s_ShmView, kShmTotalSize, MS_ASYNC);
}

void BumpIncarnation(int slot) {
    if (!s_ShmView || slot < 0 || slot >= kShmMaxSlots) return;
    *IncarnationPtr(slot) = *IncarnationPtr(slot) + 1;
    msync(s_ShmView, kShmTotalSize, MS_ASYNC);
}

void ShutdownSharedMemory() {
    if (s_ShmView) {
        munmap(s_ShmView, kShmTotalSize);
        s_ShmView = nullptr;
    }
    if (s_ShmFd >= 0) {
        close(s_ShmFd);
        s_ShmFd = -1;
    }
    // Don't shm_unlink — the .so may be reloaded and other plugins read it
}

}  // namespace botid
