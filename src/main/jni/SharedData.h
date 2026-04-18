#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <sys/mman.h>
#include <sys/stat.h>
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

#define SHARED_MEM_NAME  "gl_cache"
#define SHARED_MEM_PATH  "/dev/ashmem"
#define SHARED_MEM_SIZE  4096
#define MAX_ESP_PLAYERS  64

struct ESPEntry {
    float topX, topY, topZ;       // Posição de tela do topo do player
    float bottomX, bottomY, bottomZ; // Posição de tela da base do player
    float distance;               // Distância do player
    int   team;                   // ID do time
    float health;                 // Vida (legado)
    bool  valid;                  // Entrada válida
    int   curHp;                  // HP atual
    int   maxHp;                  // HP máximo
    bool  knocked;                // Player está derrubado/sangrando
};

struct SharedESPData {
    uint32_t magic;               // 0xDEADF00D = SHM criado (hack_thread rodando)
    uint32_t hookApplied;         // 0xBEEF1234 = VmtHook OK, hookActive=true
    std::atomic<uint32_t> writeSeq; // Sequência de escrita (detectar update)
    int screenW;
    int screenH;
    int playerCount;
    volatile int espEnabled;      // Overlay seta 1/0 → hook verifica
    volatile int debugLastCall;   // Diagnóstico: último il2cpp call completado
    volatile int resetSelf;       // Overlay seta 1 → hook reseta cache do self player

    // ── Aim Assist (Head Magnetism) ──────────────────────────────────────────
    // Overlay escreve configurações → hook aplica rotação suave da câmera
    volatile int aimAssistEnabled;  // Toggle: 0 = off, 1 = on
    float        aimAssistSpeed;    // Máx graus por frame (0.1–8.0). "Sensi" do aim.
    float        aimAssistFovDeg;   // Cone de ativação em graus (ex: 30)
    float        aimAssistDeadzone; // Ângulo mínimo para ativar correção (evita jitter)
    volatile int aimAssistHasTarget;// Hook escreve 1 quando há alvo no cone

    // ── Silent Aim (v31: aimbot direto) ───────────────────────────────────
    // Mover câmera diretamente para a cabeça do inimigo (visível ao jogador).
    // A câmera NÃO é restaurada após orig — mira permanece no alvo.
    volatile int silentAimEnabled;  // Toggle: 0 = off, 1 = on

    // ── Aim Target Priority ──────────────────────────────────────────────────
    // 0 = Nearest (Center Screen) — menor distância ao centro da tela
    // 1 = Lowest HP — inimigo com menor HP dentro do FOV
    // 2 = Nearest Distance — menor profundidade de câmera (clipW)
    volatile int aimTargetPriority;
    // ── Aimbot Mode ──────────────────────────────────────────────────
    // 0 = Legit: interpolação suave (lerp) por frame
    // 1 = Rage:  snap instantâneo + offsetY (capa) para o topo da malha
    volatile int aimMode;
    float        aimLegitSmooth; // legado (não utilizado, manter offset)
    float        aimRageOffsetY; // Rage: offset Y sobre o alvo (capa) ex: 0.05
    float        aimbotSmooth;   // Aimbot: 0.0=instant snap, 0.0-0.95=lerp suave

    // ── Trigger Key (hold-to-aim) ─────────────────────────────────
    // triggerKey  = 0  → aimbot sempre ativo quando aimAssistEnabled
    // triggerKey  = 114/115 → aimbot só ativo enquanto tecla estiver PRESSIONADA
    // triggerHeld = 1 enquanto a tecla estiver segurada (overlay atualiza)
    volatile int triggerHeld; // 1 = tecla trigger pressionada agora
    int32_t      triggerKey;  // keycode (0 = sem trigger / sempre ativo)

    // ── Anti-Recoil ──────────────────────────────────────────────────────
    // Salva euler antes de orig, restaura após orig.
    // Cancela o recoil (variação angular) adicionado pelo jogo por frame.
    // Funciona independente do aimbot.
    volatile int recoilEnabled; // 1 = ativo, 0 = desativado

    // ── Speed Hack ─────────────────────────────────────────────────────
    // Chama set_MoveSpeed(player, speedValue) no player local a cada frame.
    // speedValue normal: ~6.5. Valores acima aceleram o personagem.
    volatile int speedEnabled; // 1 = ativo, 0 = desativado
    float        speedValue;   // velocidade alvo (ex: 15.0 = ~2x normal)

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
// Nomes ofuscados para evitar deteccao por file scanning
#define SHM_PATH_1 "/data/local/tmp/.gl_cache"
#define SHM_PATH_2 "/sdcard/.gl_cache"
#define SHM_FILENAME ".gl_cache"
#define HOOKLOG_FILENAME ".gl_log"

static const char* shmActivePath = nullptr;

// fd pre-aberto em preAppSpecialize (root context) pelo modulo Zygisk.
// SELinux bloqueia untrusted_app de criar shell_data_file,
// mas um fd aberto como root sobrevive ao setuid e pode ser reutilizado.
#ifdef ZYGISK_BUILD
extern int g_zygisk_shm_fd;
#else
static int g_zygisk_shm_fd = -1;
#endif

// Game data dir — hardcode o package pois /proc/self/cmdline nao e confiavel
// (no constructor ainda e 'app_process64' do zygote)
#ifndef HOOK_GAME_PACKAGE
#define HOOK_GAME_PACKAGE "com.dts.freefireth"
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
// Tenta: 1) /data/local/tmp/ (PROVADO funcionar), 2) data dir do jogo, 3) /sdcard/
static int shm_create_file() {
    // ZYGISK: fd pre-aberto em root context (preAppSpecialize antes do setuid)
    // Este fd bypass SELinux pois foi aberto como root e sobrevive ao setuid.
    if (g_zygisk_shm_fd >= 0) {
        ftruncate(g_zygisk_shm_fd, SHARED_MEM_SIZE);
        fchmod(g_zygisk_shm_fd, 0666);
        shmActivePath = SHM_PATH_1;
        return g_zygisk_shm_fd;
    }

    // PRIMARIO: /data/local/tmp/ — pre-criado pelo overlay com chmod 666
    // Este e o path que o hook COMPROVOU funcionar (fd=196 no logcat)
    int fd = open(SHM_PATH_1, O_RDWR);
    if (fd >= 0) {
        ftruncate(fd, SHARED_MEM_SIZE);
        fchmod(fd, 0666);
        shmActivePath = SHM_PATH_1;
        return fd;
    }
    fd = open(SHM_PATH_1, O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        ftruncate(fd, SHARED_MEM_SIZE);
        fchmod(fd, 0666);
        shmActivePath = SHM_PATH_1;
        return fd;
    }

    // Fallback 1: diretorio de dados do jogo
    const char* gamePath = getGameShmPath();
    if (gamePath[0]) {
        fd = open(gamePath, O_RDWR);
        if (fd < 0) fd = open(gamePath, O_CREAT | O_RDWR, 0666);
        if (fd >= 0) {
            ftruncate(fd, SHARED_MEM_SIZE);
            fchmod(fd, 0666);
            shmActivePath = gamePath;
            return fd;
        }
    }

    // Fallback 2: /sdcard/
    fd = open(SHM_PATH_2, O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        ftruncate(fd, SHARED_MEM_SIZE);
        fchmod(fd, 0666);
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
