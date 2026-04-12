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
#define SHM_PATH_1 "/data/local/tmp/.esp_shm"
#define SHM_PATH_2 "/sdcard/.esp_shm"

static const char* shmActivePath = nullptr;

// Cria/abre shared memory (hook no jogo — roda como UID do app, NÃO root)
// O arquivo deve ser pré-criado com chmod 666 pelo app root
static int shm_create_file() {
    // Tentar abrir arquivo pré-criado pelo app (com permissões corretas)
    int fd = open(SHM_PATH_1, O_RDWR);
    if (fd >= 0) {
        ftruncate(fd, SHARED_MEM_SIZE);
        shmActivePath = SHM_PATH_1;
        return fd;
    }

    // Fallback: tentar criar (pode falhar se sem permissão)
    fd = open(SHM_PATH_1, O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        ftruncate(fd, SHARED_MEM_SIZE);
        shmActivePath = SHM_PATH_1;
        return fd;
    }

    // Fallback 2: /sdcard/ (acessível por todos os apps)
    fd = open(SHM_PATH_2, O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        ftruncate(fd, SHARED_MEM_SIZE);
        shmActivePath = SHM_PATH_2;
        return fd;
    }

    return -1;
}

static int shm_open_file() {
    // Tentar na ordem
    int fd = open(SHM_PATH_1, O_RDWR);
    if (fd >= 0) return fd;
    fd = open(SHM_PATH_2, O_RDWR);
    return fd;
}
