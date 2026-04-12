#pragma once

#include <cstdint>
#include <cstring>
#include <atomic>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/ashmem.h>
#include <sys/ioctl.h>

/*
 * SharedData - Comunicação entre o HOOK (no jogo) e o OVERLAY (externo)
 *
 * Usa ashmem (Android Shared Memory) — memória compartilhada entre processos.
 * O hook escreve os dados ESP, o overlay lê e desenha.
 *
 * Fluxo:
 *   Hook (jogo) → escreve posições de tela no SharedData
 *   Overlay (APK) → lê SharedData e desenha ImGui
 */

#define SHARED_MEM_NAME  "esp_shm"
#define SHARED_MEM_PATH  "/dev/ashmem"
#define SHARED_MEM_SIZE  4096
#define MAX_ESP_PLAYERS  64

struct ESPEntry {
    float topX, topY, topZ;       // Posição de tela do topo do player
    float bottomX, bottomY, bottomZ; // Posição de tela da base do player
    float distance;               // Distância do player
    int   team;                   // ID do time
    float health;                 // Vida
    bool  valid;                  // Entrada válida
};

struct SharedESPData {
    uint32_t magic;               // 0xDEADF00D = dados válidos
    std::atomic<uint32_t> writeSeq; // Sequência de escrita (detectar update)
    int screenW;
    int screenH;
    int playerCount;
    volatile int espEnabled;      // Overlay seta 1/0 → hook verifica
    volatile int debugLastCall;   // Diagnóstico: último il2cpp call completado
    ESPEntry players[MAX_ESP_PLAYERS];
};

// ============================================================
// Helpers para criar/abrir shared memory
// ============================================================

// Cria shared memory (chamado pelo HOOK no jogo)
static int shm_create() {
    int fd = open(SHARED_MEM_PATH, O_RDWR);
    if (fd < 0) return -1;

    // Dá nome à região
    ioctl(fd, ASHMEM_SET_NAME, SHARED_MEM_NAME);
    ioctl(fd, ASHMEM_SET_SIZE, SHARED_MEM_SIZE);

    return fd;
}

// Mapeia shared memory
static SharedESPData* shm_map(int fd) {
    void* ptr = mmap(nullptr, SHARED_MEM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) return nullptr;
    return static_cast<SharedESPData*>(ptr);
}

// Desmapeia
static void shm_unmap(SharedESPData* data) {
    if (data) munmap(data, SHARED_MEM_SIZE);
}

// ============================================================
// Alternativa: Arquivo simples no /data/local/tmp/
// Mais simples que ashmem, funciona com root
// ============================================================

// Paths para shared memory (o hook tenta em ordem)
// PATH_1 e PATH_2 sao fallbacks — o PRIMARY e o data dir do jogo
#define SHM_PATH_1 "/data/local/tmp/.esp_shm"
#define SHM_PATH_2 "/sdcard/.esp_shm"
#define SHM_FILENAME ".esp_shm"
#define HOOKLOG_FILENAME ".hook_log"

static const char* shmActivePath = nullptr;

// Game data dir — hardcode o package pois /proc/self/cmdline nao e confiavel
// (no constructor ainda e 'app_process64' do zygote)
#ifndef HOOK_GAME_PACKAGE
#define HOOK_GAME_PACKAGE "com.fungames.sniper3d"
#endif

static char g_gameDataDir[256] = {0};
static char g_shmGamePath[512] = {0};
static char g_logGamePath[512] = {0};

static const char* getGameDataDir() {
    if (g_gameDataDir[0]) return g_gameDataDir;
    snprintf(g_gameDataDir, sizeof(g_gameDataDir), "/data/data/%s", HOOK_GAME_PACKAGE);
    return g_gameDataDir;
}

static const char* getGameShmPath() {
    if (g_shmGamePath[0]) return g_shmGamePath;
    const char* dir = getGameDataDir();
    if (dir[0]) snprintf(g_shmGamePath, sizeof(g_shmGamePath), "%s/%s", dir, SHM_FILENAME);
    return g_shmGamePath;
}

static const char* getGameLogPath() {
    if (g_logGamePath[0]) return g_logGamePath;
    const char* dir = getGameDataDir();
    if (dir[0]) snprintf(g_logGamePath, sizeof(g_logGamePath), "%s/%s", dir, HOOKLOG_FILENAME);
    return g_logGamePath;
}

// Cria/abre shared memory (hook no jogo — roda como UID do app)
// Tenta: 1) data dir do jogo, 2) /data/local/tmp/, 3) /sdcard/
static int shm_create_file() {
    // PRIMARIO: diretorio de dados do jogo (sempre acessivel pelo app)
    const char* gamePath = getGameShmPath();
    if (gamePath[0]) {
        int fd = open(gamePath, O_RDWR);
        if (fd < 0) fd = open(gamePath, O_CREAT | O_RDWR, 0666);
        if (fd >= 0) {
            ftruncate(fd, SHARED_MEM_SIZE);
            shmActivePath = gamePath;
            return fd;
        }
    }

    // Fallback 1: pre-criado pelo app
    int fd = open(SHM_PATH_1, O_RDWR);
    if (fd >= 0) {
        ftruncate(fd, SHARED_MEM_SIZE);
        shmActivePath = SHM_PATH_1;
        return fd;
    }
    fd = open(SHM_PATH_1, O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        ftruncate(fd, SHARED_MEM_SIZE);
        shmActivePath = SHM_PATH_1;
        return fd;
    }

    // Fallback 2: /sdcard/
    fd = open(SHM_PATH_2, O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        ftruncate(fd, SHARED_MEM_SIZE);
        shmActivePath = SHM_PATH_2;
        return fd;
    }

    return -1;
}

// Abre shared memory para leitura (overlay)
// Tenta: 1) data dir do jogo, 2) /data/local/tmp/, 3) /sdcard/
static int shm_open_file(const char* gamePackage) {
    // Tentar data dir do jogo especifico
    if (gamePackage && gamePackage[0]) {
        char path[512];
        snprintf(path, sizeof(path), "/data/data/%s/%s", gamePackage, SHM_FILENAME);
        int fd = open(path, O_RDWR);
        if (fd >= 0) {
            off_t sz = lseek(fd, 0, SEEK_END);
            lseek(fd, 0, SEEK_SET);
            if (sz >= (off_t)SHARED_MEM_SIZE) return fd;
            close(fd);
        }
    }
    // Fallback paths
    int fd = open(SHM_PATH_1, O_RDWR);
    if (fd >= 0) {
        off_t sz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        if (sz >= (off_t)SHARED_MEM_SIZE) return fd;
        close(fd);
    }
    fd = open(SHM_PATH_2, O_RDWR);
    if (fd >= 0) {
        off_t sz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        if (sz >= (off_t)SHARED_MEM_SIZE) return fd;
        close(fd);
    }
    return -1;
}
