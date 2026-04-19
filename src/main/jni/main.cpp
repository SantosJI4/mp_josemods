/*
 * ============================================================
 * EXTERNAL OVERLAY - Processo separado do jogo
 * ============================================================
 *
 * Arquitetura HÍBRIDA:
 *   - APK roda como processo independente (NÃO injeta no jogo)
 *   - Overlay via WindowManager com FLAG_SECURE (exclusão de captura)
 *   - DADOS vêm do GameHook.cpp via SharedMemory (arquivo mmap)
 *   - GameHook.cpp é injetado no jogo via script root — faz VMT hook
 *   - O hook escreve posições de tela → overlay lê e desenha
 *   - ImGui renderiza em EGL context próprio sobre o overlay
 *
 * main.cpp = APENAS LEITURA + DESENHO (zero acesso ao jogo)
 * GameHook.cpp = VMT hook dentro do jogo (coleta dados)
 *
 * Código antigo salvo em: main_injected.cpp.bak
 * ============================================================
 */

#include <jni.h>
#include <pthread.h>
#include <android/native_window_jni.h>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <unistd.h>
#include <cerrno>
#include <ctime>
#include <GLES3/gl3.h>
#include <EGL/egl.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"

#include "Overlay.h"
#include "SharedData.h"

#include <android/log.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/input.h>
#define MTAG "MeowESP"
#define MLOGI(...) __android_log_print(ANDROID_LOG_INFO, MTAG, __VA_ARGS__)

// ============================================================
// ESP State
// ============================================================
static bool esp = true;   // ligado por padrao
static bool drawEnemyBox = true;
static bool drawSnapLine = true;
static bool drawDistance = false;
static ImVec4 espLineColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
static float espMaxDistance = 999.0f;
static float linePositionX = 0.5f;

// ============================================================
// Aim Assist State
// ============================================================
static bool  aimAssist       = false;
static float aimSpeed        = 2.5f;  // Graus por frame (sensi do aim)
static float aimFovDeg       = 30.0f; // Cone de ativação em graus
static float aimDeadzone     = 1.5f;  // Ângulo mínimo para ativar (anti-jitter)

// ============================================================
// Aimbot State (v31)
// ============================================================
static bool  silentAim      = false;
static bool  recoilEnabled  = false;  // v31: anti-recoil
static float aimbotSmooth   = 0.0f;   // 0.0=snap instant, 0.0-0.95=lerp suave
static bool  speedEnabled   = false;  // v32: speed hack
static float speedValue     = 13.0f;  // velocidade (normal ~6.5)

// ============================================================
// Aim Target Priority
// 0 = Nearest Center, 1 = Lowest HP, 2 = Nearest Distance
// ============================================================
static int   aimTargetPriority = 0;

// ============================================================
// Aimbot Mode (v27)
// 0 = Legit (suave/lerp), 1 = Rage (snap instantâneo)
// ============================================================
static int   aimMode        = 0;      // 0=Legit, 1=Rage
static float aimLegitSmooth = 0.12f;  // fator lerp (0.01–0.50)
static float aimRageOffsetY = 0.0f;   // offset Y sobre a cabeça (0=bone da cabeça exato)
static int   triggerKey     = 0;      // 0=sempre ativo, 114=Vol-, 115=Vol+

// ============================================================
// Player Hacks (v49)
// ============================================================
static bool ammoEnabled       = false;
static bool medkitFastEnabled = false;
static bool fastWeaponSwitch  = false;
static bool medkitRunEnabled  = false;
static bool drawNickName      = true;

// ============================================================
// Hotkeys State  (Linux keycodes: Vol- = 114, Vol+ = 115)
// ============================================================
static int hotkeyAim    = 115;  // KEY_VOLUMEUP  — toggle Head Magnetism
static int hotkeySilent = 114;  // KEY_VOLUMEDOWN — toggle Silent Aim
static int hotkeyEsp    = 0;    // 0 = desativado

// SharedMemory (leitura do hook injetado no jogo)
static SharedESPData* sharedData = nullptr;
static int shmFd = -1;
static std::atomic<bool> shmConnected{false};
static uint32_t lastWriteSeq = 0;

// Game package — para encontrar SHM no data dir do jogo
#define GAME_PACKAGE "com.dts.freefireth"

// ============================================================
// SharedMemory Reader — Conecta ao arquivo criado pelo hook
// Tenta: 1) /data/data/<game>/.gl_cache, 2) /data/local/tmp/, 3) /sdcard/
// ============================================================
static std::atomic<bool> readerRunning{false};
static pthread_t readerThread = 0;
static char shmStatus[256] = "Iniciando...";

void* shmReaderLoop(void*) {
    int attempt = 0;
    MLOGI("shmReaderLoop started (build: v7-fixed-paths)");

    // Paths para tentar: /data/local/tmp/ PRIMEIRO (o hook comprovou que funciona la)
    // game dir como fallback (pode falhar por SELinux/namespace)
    char gameShmPath[512];
    snprintf(gameShmPath, sizeof(gameShmPath), "/data/data/%s/%s", GAME_PACKAGE, SHM_FILENAME);
    const char* paths[] = { SHM_PATH_1, gameShmPath, SHM_PATH_2 };
    const int numPaths = 3;

    while (readerRunning.load()) {
        attempt++;

        int fd = -1;
        const char* usedPath = nullptr;
        for (int p = 0; p < numPaths; p++) {
            fd = open(paths[p], O_RDWR);
            if (fd >= 0) {
                usedPath = paths[p];
                MLOGI("Tentativa %d: aberto %s (fd=%d)", attempt, usedPath, fd);
                break;
            }
        }
        if (fd < 0) {
            snprintf(shmStatus, sizeof(shmStatus),
                "Tentativa %d: nenhum shm acessivel (errno=%d: %s)",
                attempt, errno, strerror(errno));
            MLOGI("%s", shmStatus);
            sleep(1);
            continue;
        }

        // Verificar tamanho
        off_t sz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        MLOGI("Tentativa %d: tamanho=%ld", attempt, (long)sz);

        if (sz < (off_t)SHARED_MEM_SIZE) {
            snprintf(shmStatus, sizeof(shmStatus),
                "Tentativa %d: arquivo pequeno (%ld < %d)",
                attempt, (long)sz, SHARED_MEM_SIZE);
            MLOGI("%s", shmStatus);
            close(fd);
            sleep(1);
            continue;
        }

        // Mapear
        SharedESPData* data = shm_map(fd);
        if (!data) {
            snprintf(shmStatus, sizeof(shmStatus),
                "Tentativa %d: mmap falhou (errno=%d: %s)",
                attempt, errno, strerror(errno));
            MLOGI("%s", shmStatus);
            close(fd);
            sleep(1);
            continue;
        }

        // Log magic value
        MLOGI("Tentativa %d: mmap OK, magic=0x%08X (esperado 0xDEADF00D)", attempt, data->magic);

        // FORÇAR CONEXÃO: conecta se o arquivo existe e é mapeável
        // Não espera magic — o hook pode não ter escrito ainda
        shmFd = fd;
        sharedData = data;
        shmConnected.store(true);
        snprintf(shmStatus, sizeof(shmStatus),
            "Conectado! magic=0x%08X", data->magic);
        MLOGI("SharedMemory CONECTADO (forçado). magic=0x%08X", data->magic);
        break;
    }

    // Manter thread viva para monitoramento
    while (readerRunning.load()) {
        if (sharedData) {
            // Atualizar status periodicamente
            snprintf(shmStatus, sizeof(shmStatus),
                "magic=0x%08X seq=%u players=%d esp=%d dbg=%d",
                sharedData->magic,
                sharedData->writeSeq.load(std::memory_order_relaxed),
                sharedData->playerCount,
                sharedData->espEnabled,
                sharedData->debugLastCall);
        }
        usleep(500000);
    }

    // Cleanup
    if (sharedData) {
        shm_unmap(sharedData);
        sharedData = nullptr;
    }
    if (shmFd >= 0) {
        close(shmFd);
        shmFd = -1;
    }
    shmConnected.store(false);
    return nullptr;
}

// ============================================================
// Draw ESP — Lê direto do SharedMemory (sem cópia)
// ============================================================
void DrawESP(int screenW, int screenH) {
    if (!sharedData || !shmConnected.load()) return;

    // Hook SEMPRE coleta players (espEnabled=1 fixo) — draw controlado pelo toggle
    sharedData->espEnabled = 1;

    // ── Sincronizar Aimbot (v31: aimbot direto + anti-recoil) ────────────────
    sharedData->aimAssistEnabled  = 0;
    sharedData->aimAssistFovDeg   = aimFovDeg;
    sharedData->silentAimEnabled   = silentAim ? 1 : 0;
    sharedData->aimTargetPriority  = aimTargetPriority;
    sharedData->aimRageOffsetY     = aimRageOffsetY;
    sharedData->triggerKey         = triggerKey;
    sharedData->recoilEnabled      = recoilEnabled ? 1 : 0;
    sharedData->aimbotSmooth       = aimbotSmooth;
    sharedData->speedEnabled       = speedEnabled ? 1 : 0;
    sharedData->speedValue         = speedValue;
    // Player Hacks (v49)
    sharedData->ammoEnabled        = ammoEnabled       ? 1 : 0;
    sharedData->medkitFastEnabled  = medkitFastEnabled ? 1 : 0;
    sharedData->fastWeaponSwitch   = fastWeaponSwitch  ? 1 : 0;
    sharedData->medkitRunEnabled   = medkitRunEnabled  ? 1 : 0;
    // triggerHeld é gerenciado exclusivamente pelo hotkeyThread
    // ────────────────────────────────────────────────────────────────────────

    if (!esp) return;

    // Verificar se hook escreveu dados válidos
    if (sharedData->magic != 0xDEADF00D) return;

    // Ler dados do shared memory (escrito pelo hook no jogo)
    uint32_t seq = sharedData->writeSeq.load(std::memory_order_acquire);
    int count = sharedData->playerCount;

    if (count <= 0 || count > MAX_ESP_PLAYERS) return;

    // Escala de resolução: o hook usa Screen.width/height do jogo (pode ser 720p)
    // enquanto o overlay desenha na surface nativa do device (ex: 1080p).
    // Sem scaling, as caixas aparecem ao lado/fora dos players.
    int gameW = sharedData->screenW;
    int gameH = sharedData->screenH;
    if (gameW <= 0 || gameH <= 0) return;
    float scaleX = (float)screenW / (float)gameW;
    float scaleY = (float)screenH / (float)gameH;

    ImU32 color = ImGui::ColorConvertFloat4ToU32(espLineColor);
    // Snapline: origem no centro inferior/topo da tela — usa scaleX para alinhar com o jogo
    ImVec2 snapOrigin = ImVec2((float)gameW * linePositionX * scaleX, 0.0f);
    auto* draw = ImGui::GetBackgroundDrawList();

    // ── Círculo de FOV visual ─ desenhado ANTES do early-return, sempre visível quando aimbot ativo
    // Centro e raio calculados em game coords e escalados → alinha perfeitamente com a mira
    // independente da resolução do device ou do jogo.
    if (silentAim || aimAssist) {
        float cx          = (float)gameW * 0.5f * scaleX;   // centro X do viewport do jogo
        float cy          = (float)gameH * 0.5f * scaleY;   // centro Y do viewport do jogo
        float fovRadiusPx = (float)gameW * (aimFovDeg / 90.0f) * 0.5f * scaleX;
        bool  locked      = sharedData && sharedData->aimAssistHasTarget;
        ImU32 circleColor = locked
            ? IM_COL32(255, 80,  0,  200)
            : IM_COL32(255, 255, 255, 55);
        draw->AddCircle(ImVec2(cx, cy), fovRadiusPx, circleColor, 64, 1.5f);
    }

    bool hasTarget = false;
    for (int i = 0; i < count; i++) {
        const ESPEntry& e = sharedData->players[i];
        if (!e.valid) continue;
        if (e.distance > espMaxDistance) continue;
        if (e.curHp <= 0) continue;  // morto
        hasTarget = true;
        break;
    }
    if (!hasTarget) return;

    for (int i = 0; i < count; i++) {
        const ESPEntry& entry = sharedData->players[i];
        if (!entry.valid) continue;

        // Ignorar players mortos (HP <= 0)
        if (entry.curHp <= 0) continue;

        // Filtro de distância
        if (entry.distance > espMaxDistance) continue;

        // Escalar coordenadas do espaço do jogo para o espaço do overlay
        float rawTopX    = entry.topX    * scaleX;
        float rawTopY    = entry.topY    * scaleY;
        float rawBottomX = entry.bottomX * scaleX;
        float rawBottomY = entry.bottomY * scaleY;

        // topY = screen Y da cabeca, bottomY = screen Y dos pes
        // Em screen coords: topY < bottomY (Y cresce pra baixo)
        float top = fminf(rawTopY, rawBottomY);
        float bot = fmaxf(rawTopY, rawBottomY);
        float boxHeight = bot - top;
        if (boxHeight < 2.0f) continue;

        // Largura proporcional: corpo humano ~0.45x da altura
        float boxWidth = boxHeight * 0.45f;

        // Centro X: media entre top e bottom (perspectiva)
        float centerX = (rawTopX + rawBottomX) * 0.5f;

        // Box com margem minima
        float padY = boxHeight * 0.03f;
        float halfW = boxWidth * 0.5f;
        ImVec2 boxMin = ImVec2(centerX - halfW, top - padY);
        ImVec2 boxMax = ImVec2(centerX + halfW, bot + padY);

        // Cor: vermelho se knocked, branco se normal
        ImU32 boxColor = entry.knocked
            ? IM_COL32(255, 0, 0, 255)
            : color;

        // Box
        if (drawEnemyBox) {
            draw->AddRect(boxMin, boxMax, boxColor, 1.0f, 15, 1.5f);
        }

        // Nick name acima da caixa (v49)
        if (drawNickName && entry.nick[0] != '\0') {
            ImVec2 textSz = ImGui::CalcTextSize(entry.nick);
            float  tx     = centerX - textSz.x * 0.5f;
            float  ty     = boxMin.y - textSz.y - 2.0f;
            // Sombra
            draw->AddText(ImVec2(tx + 1, ty + 1), IM_COL32(0, 0, 0, 200), entry.nick);
            draw->AddText(ImVec2(tx,     ty),     boxColor,               entry.nick);
        }

        // Barra de HP: lado esquerdo do box, vertical
        {
            float hpRatio = (entry.maxHp > 0)
                ? (float)entry.curHp / (float)entry.maxHp
                : 1.0f;
            if (hpRatio < 0.0f) hpRatio = 0.0f;
            if (hpRatio > 1.0f) hpRatio = 1.0f;

            float barX   = boxMin.x - 4.0f;
            float barTop = boxMin.y;
            float barBot = boxMax.y;
            float barH   = barBot - barTop;

            // Fundo da barra (cinza escuro)
            draw->AddRectFilled(
                ImVec2(barX - 2.0f, barTop),
                ImVec2(barX,        barBot),
                IM_COL32(40, 40, 40, 200));

            // Preenchimento: verde → amarelo → vermelho por ratio
            ImU32 hpColor;
            if (hpRatio > 0.5f)
                hpColor = IM_COL32(
                    (int)((1.0f - hpRatio) * 2.0f * 255), 255, 0, 220);
            else
                hpColor = IM_COL32(
                    255, (int)(hpRatio * 2.0f * 255), 0, 220);

            float fillTop = barTop + barH * (1.0f - hpRatio);
            draw->AddRectFilled(
                ImVec2(barX - 2.0f, fillTop),
                ImVec2(barX,        barBot),
                hpColor);
        }

        // Snap line: from center of screen (crosshair) to center of body
        if (drawSnapLine) {
            float bodyCenterY = (top + bot) * 0.5f;
            draw->AddLine(snapOrigin,
                         ImVec2(centerX, bodyCenterY), boxColor, 1.2f);
        }

        // Distancia embaixo do box
        if (drawDistance) {
            char distText[16];
            snprintf(distText, sizeof(distText), "%.0fm", entry.distance);
            draw->AddText(ImVec2(centerX - 12, bot + padY + 2), boxColor, distText);
        }
    }

    // ── Círculo de FOV visual (silentAim OU aimAssist ativo) ─────────────────
    if (silentAim || aimAssist) {
        float fovRadiusPx = (float)screenW * (aimFovDeg / 90.0f) * 0.5f;
        bool  locked      = sharedData && sharedData->aimAssistHasTarget;
        ImU32 circleColor = locked
            ? IM_COL32(255, 80,  0,  200)   // laranja quando travado no alvo
            : IM_COL32(255, 255, 255, 55);   // branco tênue quando procurando
        draw->AddCircle(
            ImVec2(screenW * 0.5f, screenH * 0.5f),
            fovRadiusPx, circleColor, 64, 1.5f);
    }
    // ────────────────────────────────────────────────────────────────────────

    // Hook gerencia playerCount reset por frame (detecao por tempo)
    lastWriteSeq = seq;
}

// ============================================================
// Hook Log Reader — lê o arquivo de log do hook para diagnostico
// Tenta: game dir, /data/local/tmp/, /sdcard/
// ============================================================
#define HOOK_LOG_PATH_1 "/data/local/tmp/.gl_log"
#define HOOK_LOG_PATH_2 "/sdcard/.gl_log"
static char hookLogBuf[2048] = "Nenhum log do hook";
static time_t hookLogLastRead = 0;

static void readHookLog() {
    // Ler no maximo a cada 2 segundos
    time_t now = time(nullptr);
    if (now - hookLogLastRead < 2) return;
    hookLogLastRead = now;

    // Paths: /data/local/tmp/ PRIMEIRO (onde o hook escreve)
    char gameLogPath[512];
    snprintf(gameLogPath, sizeof(gameLogPath), "/data/data/%s/%s", GAME_PACKAGE, HOOKLOG_FILENAME);
    const char* paths[] = { HOOK_LOG_PATH_1, gameLogPath, HOOK_LOG_PATH_2 };
    for (int i = 0; i < 3; i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd >= 0) {
            off_t sz = lseek(fd, 0, SEEK_END);
            if (sz > 0) {
                // Ler ultimos 2000 bytes
                off_t start = (sz > 2000) ? sz - 2000 : 0;
                lseek(fd, start, SEEK_SET);
                int toRead = (sz - start < (off_t)sizeof(hookLogBuf) - 1) ? (int)(sz - start) : (int)sizeof(hookLogBuf) - 1;
                int rd = read(fd, hookLogBuf, toRead);
                if (rd > 0) hookLogBuf[rd] = '\0';
            }
            close(fd);
            return; // Encontrou, para
        }
    }
}

// ============================================================
// Config — persiste em /data/local/tmp/.jawmods_cfg
// ============================================================
#define JAW_CONFIG_PATH  "/data/local/tmp/.jawmods_cfg"
#define JAW_CONFIG_MAGIC 0x4A415709u  // "JAW" v9

#pragma pack(push, 1)
struct JawConfig {
    uint32_t magic;
    uint8_t  esp, drawBox, drawSnap, drawDist;
    float    espColor[4];
    float    espMaxDist, lineX;
    uint8_t  aimAssist;
    float    aimSpeed, aimFovDeg, aimDeadzone;
    uint8_t  silentAim;
    int32_t  hotkeyAim, hotkeySilent, hotkeyEsp;
    int32_t  aimTargetPriority;
    // v5 fields
    int32_t  aimMode;
    float    aimLegitSmooth;
    float    aimRageOffsetY;
    int32_t  triggerKey;
    // v7 fields
    uint8_t  recoilEnabled;
    float    aimbotSmooth;
    // v8 fields
    uint8_t  speedEnabled;
    float    speedValue;
    // v9 fields (player hacks)
    uint8_t  ammoEnabled;
    uint8_t  medkitFastEnabled;
    uint8_t  fastWeaponSwitch;
    uint8_t  medkitRunEnabled;
    uint8_t  drawNickName;
};
#pragma pack(pop)

static void saveConfig() {
    JawConfig c{};
    c.magic        = JAW_CONFIG_MAGIC;
    c.esp          = esp;
    c.drawBox      = drawEnemyBox;
    c.drawSnap     = drawSnapLine;
    c.drawDist     = drawDistance;
    c.espColor[0]  = espLineColor.x;
    c.espColor[1]  = espLineColor.y;
    c.espColor[2]  = espLineColor.z;
    c.espColor[3]  = espLineColor.w;
    c.espMaxDist   = espMaxDistance;
    c.lineX        = linePositionX;
    c.aimAssist    = aimAssist;
    c.aimSpeed     = aimSpeed;
    c.aimFovDeg    = aimFovDeg;
    c.aimDeadzone  = aimDeadzone;
    c.silentAim          = silentAim;
    c.hotkeyAim          = hotkeyAim;
    c.hotkeySilent       = hotkeySilent;
    c.hotkeyEsp          = hotkeyEsp;
    c.aimTargetPriority  = aimTargetPriority;
    // v5
    c.aimMode            = aimMode;
    c.aimLegitSmooth     = aimLegitSmooth;
    c.aimRageOffsetY     = aimRageOffsetY;
    c.triggerKey         = triggerKey;
    // v7
    c.recoilEnabled      = recoilEnabled;
    c.aimbotSmooth       = aimbotSmooth;
    // v8
    c.speedEnabled       = speedEnabled;
    c.speedValue         = speedValue;
    // v9
    c.ammoEnabled        = ammoEnabled;
    c.medkitFastEnabled  = medkitFastEnabled;
    c.fastWeaponSwitch   = fastWeaponSwitch;
    c.medkitRunEnabled   = medkitRunEnabled;
    c.drawNickName       = drawNickName;
    int fd = open(JAW_CONFIG_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd >= 0) { write(fd, &c, sizeof(c)); close(fd); }
}

static void loadConfig() {
    JawConfig c{};
    int fd = open(JAW_CONFIG_PATH, O_RDONLY);
    if (fd < 0) return;
    ssize_t rd = read(fd, &c, sizeof(c));
    close(fd);
    if (rd != (ssize_t)sizeof(c) || c.magic != JAW_CONFIG_MAGIC) return;
    esp           = c.esp;
    drawEnemyBox  = c.drawBox;
    drawSnapLine  = c.drawSnap;
    drawDistance  = c.drawDist;
    espLineColor  = ImVec4(c.espColor[0], c.espColor[1], c.espColor[2], c.espColor[3]);
    espMaxDistance= c.espMaxDist;
    linePositionX = c.lineX;
    aimAssist     = c.aimAssist;
    aimSpeed      = c.aimSpeed;
    aimFovDeg     = c.aimFovDeg;
    aimDeadzone   = c.aimDeadzone;
    silentAim          = c.silentAim;
    hotkeyAim          = c.hotkeyAim;
    hotkeySilent       = c.hotkeySilent;
    hotkeyEsp          = c.hotkeyEsp;
    aimTargetPriority  = c.aimTargetPriority;
    // v5
    aimMode            = c.aimMode;
    aimLegitSmooth     = c.aimLegitSmooth;
    aimRageOffsetY     = c.aimRageOffsetY;
    triggerKey         = c.triggerKey;
    // v7
    recoilEnabled      = c.recoilEnabled;
    aimbotSmooth       = c.aimbotSmooth;
    // v8
    speedEnabled       = c.speedEnabled;
    speedValue         = c.speedValue;
    // v9
    ammoEnabled        = c.ammoEnabled;
    medkitFastEnabled  = c.medkitFastEnabled;
    fastWeaponSwitch   = c.fastWeaponSwitch;
    medkitRunEnabled   = c.medkitRunEnabled;
    drawNickName       = c.drawNickName;
}

// ============================================================
// Server Status Check — TCP connect ao servidor jawmods
// Atualizar SERVER_CHECK_HOST se a URL do SquareCloud mudar.
// ============================================================
#define SERVER_CHECK_HOST "jawmods-key-server.squareweb.app"
#define SERVER_CHECK_PORT "443"

static std::atomic<bool> g_serverOnline{false};
static char g_serverStatus[64] = "Verificando...";

static void* serverCheckThread(void*) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(SERVER_CHECK_HOST, SERVER_CHECK_PORT, &hints, &res) != 0 || !res) {
        snprintf(g_serverStatus, sizeof(g_serverStatus), "Offline (DNS)");
        g_serverOnline.store(false);
        return nullptr;
    }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        snprintf(g_serverStatus, sizeof(g_serverStatus), "Offline");
        g_serverOnline.store(false);
        return nullptr;
    }
    struct timeval tv{};
    tv.tv_sec = 5;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int r = connect(sock, res->ai_addr, res->ai_addrlen);
    close(sock);
    freeaddrinfo(res);
    if (r == 0) {
        g_serverOnline.store(true);
        snprintf(g_serverStatus, sizeof(g_serverStatus), "Online");
    } else {
        g_serverOnline.store(false);
        snprintf(g_serverStatus, sizeof(g_serverStatus), "Offline");
    }
    return nullptr;
}

// ============================================================
// Hotkeys — lê eventos de volume do /dev/input/event*
// Múltiplos processos podem ler o mesmo eventX sem interferir.
// ============================================================
static std::atomic<bool> g_hotkeyRunning{false};
static pthread_t         g_hotkeyThread = 0;

static void* hotkeyThread(void*) {
    // Encontrar device com KEY_VOLUMEUP (bit 115)
    int evfd = -1;
    for (int i = 0; i < 20 && evfd < 0; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        uint8_t keybits[128] = {};  // 1024 bits, cobre KEY_VOLUMEUP=115
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0 &&
            (keybits[115 / 8] & (1u << (115 % 8)))) {
            evfd = fd;
            break;
        }
        close(fd);
    }
    if (evfd < 0) return nullptr;

    struct input_event ev{};
    while (g_hotkeyRunning.load()) {
        ssize_t rd = read(evfd, &ev, sizeof(ev));
        if (rd < (ssize_t)sizeof(ev)) { usleep(8000); continue; }
        if (ev.type != EV_KEY) continue;
        // ── Trigger key: detectar HOLD/RELEASE em tempo real ──────────────
        // Se triggerKey está configurado, essa tecla faz "hold-to-aim".
        // Tecla de trigger não aciona toggles de features.
        if (triggerKey > 0 && ev.code == (uint16_t)triggerKey) {
            if (sharedData)
                sharedData->triggerHeld = (ev.value >= 1) ? 1 : 0;
            continue;  // não cai nos toggles abaixo
        }
        // ── Toggles normais só em key-press (value==1) ────────────────────
        if (ev.value != 1) continue;
        // hotkeyAim agora controla silentAim (v29: único aimbot)
        if (hotkeyAim > 0 && ev.code == (uint16_t)hotkeyAim) {
            silentAim = !silentAim;
            if (sharedData) {
                sharedData->silentAimEnabled  = silentAim ? 1 : 0;
                if (!silentAim) sharedData->aimAssistHasTarget = 0;
            }
        }
        if (hotkeyEsp > 0 && ev.code == (uint16_t)hotkeyEsp)
            esp = !esp;
    }
    close(evfd);
    return nullptr;
}

// ============================================================
// Animated Toggle Switch — iOS-style, suave, usa ImGui storage
// ============================================================
static bool AnimatedToggle(const char* id, bool* v) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID       sid     = ImGui::GetID(id);
    float anim = storage->GetFloat(sid, *v ? 1.0f : 0.0f);
    anim += ((*v ? 1.0f : 0.0f) - anim) * ImGui::GetIO().DeltaTime * 14.0f;
    if (anim > 0.99f) anim = 1.0f;
    if (anim < 0.01f) anim = 0.0f;
    storage->SetFloat(sid, anim);

    const float H = 18.0f, W = 34.0f, R = H * 0.5f;
    ImVec2 p  = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::InvisibleButton(id, ImVec2(W, H));
    bool changed = false;
    if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }

    // Track: lerp de cinza para verde
    int tr = (int)(50  + anim * (-50));
    int tg = (int)(55  + anim * (195 - 55));
    int tb = (int)(60  + anim * (95  - 60));
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), IM_COL32(tr, tg, tb, 255), R);

    // Knob deslizante
    float knobX = p.x + R + anim * (W - R * 2.0f);
    dl->AddCircleFilled(ImVec2(knobX, p.y + R), R - 2.5f, IM_COL32(230, 232, 235, 255));

    return changed;
}

// ============================================================
// Draw Menu — Interface polida com scroll e animações (v26)
// ============================================================
void DrawMenu() {
    // ── Paleta ──────────────────────────────────────────────────────────────
    const ImVec4 cGreen    = ImVec4(0.00f, 0.88f, 0.46f, 1.00f);
    const ImVec4 cGreenDim = ImVec4(0.00f, 0.50f, 0.26f, 1.00f);
    const ImVec4 cDimText  = ImVec4(0.48f, 0.50f, 0.54f, 1.00f);
    const ImVec4 cFgText   = ImVec4(0.82f, 0.84f, 0.86f, 1.00f);
    const ImVec4 cRed      = ImVec4(1.00f, 0.32f, 0.32f, 1.00f);
    const ImVec4 cYellow   = ImVec4(1.00f, 0.80f, 0.10f, 1.00f);

    bool shmReady   = shmConnected.load() && sharedData && sharedData->magic == 0xDEADF00D;
    bool vmtApplied = shmReady && sharedData->hookApplied == 0xBEEF1234;

    // Fade-in na abertura
    static float fadeAlpha    = 0.0f;
    static bool  menuMinimized = false;
    if (fadeAlpha < 1.0f) {
        fadeAlpha += ImGui::GetIO().DeltaTime * 4.0f;
        if (fadeAlpha > 1.0f) fadeAlpha = 1.0f;
    }

    // ── Estilos globais ──────────────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(6.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,  4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,  8.0f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,             ImVec4(0.07f, 0.07f, 0.08f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,              ImVec4(0.00f, 0.00f, 0.00f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,           cGreen);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,     ImVec4(0.0f, 1.0f, 0.55f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,            cGreen);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,              ImVec4(0.13f, 0.14f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,       ImVec4(0.17f, 0.18f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,        ImVec4(0.21f, 0.22f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header,               ImVec4(0.00f, 0.40f, 0.21f, 0.50f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,        ImVec4(0.00f, 0.54f, 0.28f, 0.65f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,         ImVec4(0.00f, 0.70f, 0.37f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_Button,               ImVec4(0.00f, 0.42f, 0.22f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,        ImVec4(0.00f, 0.60f, 0.32f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,         ImVec4(0.00f, 0.80f, 0.43f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,              ImVec4(0.08f, 0.09f, 0.10f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          ImVec4(0.05f, 0.05f, 0.06f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        cGreenDim);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, cGreen);
    ImGui::PushStyleColor(ImGuiCol_Separator,            ImVec4(0.16f, 0.17f, 0.19f, 1.00f));

    // Tamanho: minimizado = só header, expandido = normal
    const float FULL_H = 490.0f;
    const float MINI_H = 44.0f;
    float targetH = menuMinimized ? MINI_H : FULL_H;
    // Largura do menu: 3 tamanhos ciclados pelo botão [P/M/G]
    static const float kMenuWidths[] = { 300.0f, 360.0f, 420.0f };
    static int  menuSizeLevel   = 1;     // 0=Pequeno, 1=Médio(default), 2=Grande
    static bool prevMinimized   = false; // detectar transição
    float menuW = kMenuWidths[menuSizeLevel];
    // Posição inicial: canto superior esquerdo (só na primeira vez)
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Once);
    // Forçar tamanho apenas: na primeira abertura, ao minimizar ou ao restaurar.
    // Fora dessas transições o usuário pode arrastar o canto para redimensionar.
    bool sizeTransition = (menuMinimized != prevMinimized);
    if (sizeTransition) {
        ImGui::SetNextWindowSize(ImVec2(menuW, targetH), ImGuiCond_Always);
    } else if (!menuMinimized) {
        // Só limita altura mínima; largura livre
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(260.0f, 300.0f), ImVec2(600.0f, 800.0f));
    } else {
        // Minimizado: travar no tamanho mini
        ImGui::SetNextWindowSize(ImVec2(menuW, MINI_H), ImGuiCond_Always);
    }
    prevMinimized = menuMinimized;
    ImGui::SetNextWindowBgAlpha(0.96f * fadeAlpha);
    ImGui::Begin("##jw", nullptr,
        ImGuiWindowFlags_NoTitleBar      |
        ImGuiWindowFlags_NoScrollbar     |
        ImGuiWindowFlags_NoScrollWithMouse);

    float W    = ImGui::GetWindowWidth();
    float winH = ImGui::GetWindowHeight();
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImVec2      wpos = ImGui::GetWindowPos();
    float       t    = (float)ImGui::GetTime();
    float       pulse = sinf(t * 2.8f) * 0.5f + 0.5f;

    // ═══════════════════════════════════════════════════════════════════════
    // HEADER fixo
    // ═══════════════════════════════════════════════════════════════════════
    const float HDR_H  = 42.0f;
    dl->AddRectFilled(wpos, ImVec2(wpos.x + W, wpos.y + HDR_H),
        IM_COL32(9, 9, 10, 255), 10.0f, ImDrawFlags_RoundCornersTop);
    // Linha de acento verde pulsante na base do header
    dl->AddRectFilled(
        ImVec2(wpos.x + 1.0f, wpos.y + HDR_H - 2.0f),
        ImVec2(wpos.x + W - 1.0f, wpos.y + HDR_H),
        IM_COL32(0, (int)(140 + pulse * 80), (int)(65 + pulse * 35), 150));

    // Ponto de status animado
    ImVec4 dotC = vmtApplied
        ? ImVec4(0.0f, 0.42f + pulse * 0.46f, 0.22f + pulse * 0.22f, 1.0f)
        : ImVec4(0.40f, 0.40f, 0.42f, 0.70f);
    dl->AddCircleFilled(
        ImVec2(wpos.x + 14.0f, wpos.y + HDR_H * 0.5f),
        4.5f, ImGui::ColorConvertFloat4ToU32(dotC));

    float textLineH = ImGui::GetTextLineHeight();
    ImGui::SetCursorPos(ImVec2(26.0f, (HDR_H - textLineH) * 0.5f));
    ImGui::TextColored(cGreen, "JAWMODS");

    // Botões do header: [P/M/G] tamanho  e  [-/+] minimizar
    {
        const float BTN_W = 44.0f;   // touch-friendly (era 22)
        const float BTN_H = 34.0f;   // touch-friendly (era 17)
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.55f, 0.28f, 0.40f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.0f, 0.80f, 0.42f, 0.55f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

        // Botão de tamanho: cicla P→M→G
        const char* szLabels[] = { "P##sz", "M##sz", "G##sz" };
        ImGui::SetCursorPos(ImVec2(W - BTN_W * 2.0f - 12.0f, (HDR_H - BTN_H) * 0.5f));
        if (ImGui::Button(szLabels[menuSizeLevel], ImVec2(BTN_W, BTN_H)))
            menuSizeLevel = (menuSizeLevel + 1) % 3;

        // Botão minimizar/restaurar
        ImGui::SetCursorPos(ImVec2(W - BTN_W - 6.0f, (HDR_H - BTN_H) * 0.5f));
        if (ImGui::Button(menuMinimized ? "+##min" : "-##min", ImVec2(BTN_W, BTN_H)))
            menuMinimized = !menuMinimized;

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    }

    const char* verStr = menuMinimized ? "v37" : "v38  FF1.123";
    float verW = ImGui::CalcTextSize(verStr).x;
    ImGui::SetCursorPos(ImVec2(W - verW - 108.0f, (HDR_H - textLineH) * 0.5f));
    ImGui::TextColored(cDimText, "%s", verStr);
    ImGui::SetCursorPosY(HDR_H + 2.0f);

    if (!menuMinimized) {
    // ═══════════════════════════════════════════════════════════════════════
    // CORPO com abas
    // ═══════════════════════════════════════════════════════════════════════
    const float FOOTER_H = 34.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
    ImGui::BeginChild("##body", ImVec2(0.0f, winH - HDR_H - FOOTER_H - 4.0f),
        false, ImGuiWindowFlags_None);
    ImGui::PopStyleVar();

    float cw = ImGui::GetContentRegionAvail().x;

    // Helper: separador leve
    auto Sep = [&]() {
        ImVec2 cp = ImGui::GetCursorScreenPos();
        float  av = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(cp.x, cp.y + 1.0f), ImVec2(cp.x + av, cp.y + 1.0f),
            IM_COL32(38, 40, 44, 255));
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
    };

    // Helper: row com label + toggle alinhado à direita
    auto ToggleRow = [&](const char* label, bool* val) {
        float rw = ImGui::GetContentRegionAvail().x;
        ImGui::TextColored(cFgText, "%s", label);
        ImGui::SameLine(rw - 38.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 1.0f);
        AnimatedToggle(label, val);
    };

    // Helper: row com label + valor verde à direita
    auto ValueLabel = [&](const char* label, const char* fmt, ...) {
        float rw = ImGui::GetContentRegionAvail().x;
        ImGui::TextColored(cDimText, "%s", label);
        char buf[48];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        float vw = ImGui::CalcTextSize(buf).x;
        ImGui::SameLine(rw - vw);
        ImGui::TextColored(cGreen, "%s", buf);
    };

    // ─── TAB BAR ─────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Tab,        ImVec4(0.10f, 0.11f, 0.12f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabActive,  ImVec4(0.00f, 0.42f, 0.22f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.00f, 0.32f, 0.16f, 0.70f));

    if (ImGui::BeginTabBar("##tabs")) {

        // ──────────────────────────────────────────────────────────────────
        // ABA: STATUS
        // ──────────────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("STATUS")) {
            ImGui::Spacing();
            ImGui::TextColored(cDimText, "Hook");
            ImGui::SameLine(90.0f);
            if (vmtApplied)
                ImGui::TextColored(cGreen, "Ativo  (%d inimigos)",
                    sharedData ? sharedData->playerCount : 0);
            else if (shmReady)
                ImGui::TextColored(cYellow, "Iniciando (%d)",
                    sharedData ? sharedData->debugLastCall : 0);
            else
                ImGui::TextColored(cRed, "Aguardando");

            ImGui::TextColored(cDimText, "Servidor");
            ImGui::SameLine(90.0f);
            if (g_serverOnline.load())
                ImGui::TextColored(cGreen, "Online");
            else
                ImGui::TextColored(cRed, "%s", g_serverStatus);

            if (shmReady && sharedData && sharedData->screenW > 0) {
                ImGui::TextColored(cDimText, "Resolucao");
                ImGui::SameLine(90.0f);
                ImGui::TextColored(cFgText, "%dx%d",
                    sharedData->screenW, sharedData->screenH);
            }
            Sep();
            if (ImGui::Button("Verificar Servidor", ImVec2(-1.0f, 26.0f))) {
                snprintf(g_serverStatus, sizeof(g_serverStatus), "Verificando...");
                g_serverOnline.store(false);
                pthread_t tt;
                pthread_create(&tt, nullptr, serverCheckThread, nullptr);
                pthread_detach(tt);
            }
            ImGui::EndTabItem();
        }

        // ──────────────────────────────────────────────────────────────────
        // ABA: ESP
        // ──────────────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("ESP")) {
            ImGui::Spacing();
            ToggleRow("ESP", &esp);
            if (esp) {
                Sep();
                ToggleRow("Caixas",    &drawEnemyBox);
                ToggleRow("Snapline",  &drawSnapLine);
                ToggleRow("Distancia", &drawDistance);
                ToggleRow("Nick Name", &drawNickName);
                Sep();
                ValueLabel("Max Distance", "%.0f m", espMaxDistance);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderFloat("##emd", &espMaxDistance, 10.0f, 999.0f, "");
                ImGui::Spacing();
                ImGui::TextColored(cDimText, "Cor dos inimigos");
                ImGui::SameLine();
                ImGui::ColorEdit4("##ec", (float*)&espLineColor,
                    ImGuiColorEditFlags_NoLabel |
                    ImGuiColorEditFlags_AlphaBar |
                    ImGuiColorEditFlags_NoInputs);
            }
            ImGui::EndTabItem();
        }

        // ──────────────────────────────────────────────────────────────────
        // ABA: AIM
        // ──────────────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("AIM")) {
            ImGui::Spacing();

            // Toggle Aimbot (v31: move câmera visivelmente para cabeça do inimigo)
            bool prevAim = silentAim;
            ToggleRow("Aimbot", &silentAim);
            if (silentAim != prevAim && sharedData) {
                sharedData->silentAimEnabled  = silentAim ? 1 : 0;
                if (!silentAim) sharedData->aimAssistHasTarget = 0;
            }
            Sep();

            if (silentAim) {
                // Status lock
                bool hasLock = sharedData && sharedData->aimAssistHasTarget;
                bool trigOk  = !sharedData ||
                               sharedData->triggerKey == 0 ||
                               sharedData->triggerHeld == 1;
                ImGui::Spacing();
                if (hasLock && trigOk) {
                    float rw = ImGui::GetContentRegionAvail().x;
                    float tw = ImGui::CalcTextSize("[ HEAD LOCKED ]").x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (rw - tw) * 0.5f);
                    ImGui::TextColored(cGreen, "[ HEAD LOCKED ]");
                } else if (!trigOk) {
                    float rw = ImGui::GetContentRegionAvail().x;
                    float tw = ImGui::CalcTextSize("[ HOLD TRIGGER ]").x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (rw - tw) * 0.5f);
                    ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.0f, 1.0f), "[ HOLD TRIGGER ]");
                }
                Sep();

                // Prioridade de alvo
                ImGui::TextColored(cDimText, "Prioridade");
                ImGui::Spacing();
                const char* priorities[] = {
                    "Mais Proximo (Tela)",
                    "Menos HP",
                    "Menor Distancia"
                };
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##tp", &aimTargetPriority, priorities, 3))
                    if (sharedData) sharedData->aimTargetPriority = aimTargetPriority;
                Sep();

                // FOV
                ValueLabel("FOV", "%.0f Deg", aimFovDeg);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##fov", &aimFovDeg, 5.0f, 90.0f, ""))
                    if (sharedData) sharedData->aimAssistFovDeg = aimFovDeg;
                ImGui::Spacing();

                // Suavidade do aimbot
                ValueLabel("Suavidade", "%.2f", aimbotSmooth);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##smo", &aimbotSmooth, 0.0f, 0.95f, ""))
                    if (sharedData) sharedData->aimbotSmooth = aimbotSmooth;
                ImGui::Spacing();
                Sep();

                // Trigger Key
                ImGui::TextColored(cDimText, "Trigger (HOLD para ativar)");
                ImGui::Spacing();
                const char* tkeys[] = { "Sempre Ativo", "Vol-  (114)", "Vol+  (115)" };
                int tidx = (triggerKey == 114) ? 1 : (triggerKey == 115) ? 2 : 0;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##trk", &tidx, tkeys, 3)) {
                    triggerKey = (tidx == 1) ? 114 : (tidx == 2) ? 115 : 0;
                    if (sharedData) {
                        sharedData->triggerKey  = triggerKey;
                        sharedData->triggerHeld = 0;
                    }
                }
                if (triggerKey > 0) {
                    ImGui::Spacing();
                    ImGui::TextColored(cDimText, "  Segure o botao enquanto atira");
                }
            }
            Sep();

            // Anti-Recoil
            bool prevRC = recoilEnabled;
            ToggleRow("Anti-Recoil", &recoilEnabled);
            if (recoilEnabled != prevRC && sharedData)
                sharedData->recoilEnabled = recoilEnabled ? 1 : 0;
            if (recoilEnabled) {
                ImGui::Spacing();
                float rw = ImGui::GetContentRegionAvail().x;
                float tw = ImGui::CalcTextSize("SEM RECUO").x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (rw - tw) * 0.5f);
                ImGui::TextColored(cGreen, "SEM RECUO");
            }
            Sep();

            // Speed Hack
            bool prevSP = speedEnabled;
            ToggleRow("Speed", &speedEnabled);
            if (speedEnabled != prevSP && sharedData)
                sharedData->speedEnabled = speedEnabled ? 1 : 0;
            if (speedEnabled) {
                ImGui::Spacing();
                ValueLabel("Velocidade", "%.1f", speedValue);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##spd", &speedValue, 6.5f, 30.0f, ""))
                    if (sharedData) sharedData->speedValue = speedValue;
                ImGui::Spacing();
                float rw = ImGui::GetContentRegionAvail().x;
                float tw = ImGui::CalcTextSize("SPEED ON").x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (rw - tw) * 0.5f);
                ImGui::TextColored(cGreen, "SPEED ON");
            }
            ImGui::EndTabItem();
        }

        // ──────────────────────────────────────────────────────────────────
        // ABA: MISC
        // ──────────────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("MISC")) {
            ImGui::Spacing();
            ImGui::TextColored(cDimText, "HOTKEYS  (Botoes de Volume)");
            ImGui::Spacing();

            const char* knames[] = { "OFF", "Vol-  (114)", "Vol+  (115)" };
            int aidx = (hotkeyAim == 114) ? 1 : (hotkeyAim == 115) ? 2 : 0;
            int eidx = (hotkeyEsp == 114) ? 1 : (hotkeyEsp == 115) ? 2 : 0;

            ImGui::TextColored(cFgText, "Aimbot");
            ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##hka", &aidx, knames, 3))
                hotkeyAim = (aidx == 1) ? 114 : (aidx == 2) ? 115 : 0;

            ImGui::TextColored(cFgText, "ESP");
            ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##hke", &eidx, knames, 3))
                hotkeyEsp = (eidx == 1) ? 114 : (eidx == 2) ? 115 : 0;

            Sep();
            if (ImGui::Button("Salvar Configuracoes", ImVec2(-1.0f, 28.0f)))
                saveConfig();
            ImGui::EndTabItem();
        }

        // ──────────────────────────────────────────────────────────────────
        // ABA: PLAYER
        // ──────────────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("PLAYER")) {
            ImGui::Spacing();
            ToggleRow("Municao Infinita",     &ammoEnabled);
            Sep();
            ToggleRow("Medkit Rapido",        &medkitFastEnabled);
            Sep();
            ToggleRow("Trocar Arma Rapido",   &fastWeaponSwitch);
            Sep();
            ToggleRow("Medkit Andando",       &medkitRunEnabled);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    } // EndTabBar

    ImGui::PopStyleColor(3); // Tab colors
    ImGui::EndChild();

    // ═══════════════════════════════════════════════════════════════════════
    // FOOTER fixo
    // ═══════════════════════════════════════════════════════════════════════
    dl->AddRectFilled(
        ImVec2(wpos.x + 1.0f, wpos.y + winH - FOOTER_H),
        ImVec2(wpos.x + W - 1.0f, wpos.y + winH),
        IM_COL32(7, 7, 8, 255), 10.0f, ImDrawFlags_RoundCornersBottom);
    dl->AddLine(
        ImVec2(wpos.x + 1.0f, wpos.y + winH - FOOTER_H),
        ImVec2(wpos.x + W - 1.0f, wpos.y + winH - FOOTER_H),
        IM_COL32(28, 30, 33, 255));

    ImGui::SetCursorPos(ImVec2(14.0f, winH - FOOTER_H + 8.0f));
    ImGui::TextColored(cDimText, "jawmods.app  v49");

    } // end !menuMinimized

    ImGui::End();

    ImGui::PopStyleColor(19);
    ImGui::PopStyleVar(6);
}

// ============================================================
// Overlay Draw Callback - Chamado a cada frame pelo Overlay
// ============================================================
void onOverlayDraw(int screenW, int screenH) {
    // Gravar dimensoes como fallback SOMENTE se o hook ainda nao inicializou.
    // Quando o hook esta ativo, ele sobrescreve screenW/H com a resolucao
    // interna do jogo (Screen.width/height), que pode ser menor que a surface
    // do overlay (ex: jogo em 720p, overlay em 1080p).
    // Assim o overlay pode escalar corretamente as coordenadas W2S.
    if (sharedData && shmConnected.load() && sharedData->screenW <= 0) {
        sharedData->screenW = screenW;
        sharedData->screenH = screenH;
    }
    DrawESP(screenW, screenH);
    DrawMenu();
}

// ============================================================
// JNI - Chamados pelo OverlayService.java
// ============================================================
extern "C" {

JNIEXPORT void JNICALL
Java_com_android_support_OverlayService_nativeOnSurfaceCreated(
        JNIEnv* env, jclass, jobject surface, jint width, jint height) {

    MLOGI("nativeOnSurfaceCreated: w=%d h=%d surface=%p", width, height, surface);

    Overlay& overlay = Overlay::get();
    overlay.onDraw = onOverlayDraw;
    bool ok = overlay.init(env, surface, width, height);
    MLOGI("nativeOnSurfaceCreated: init=%s", ok ? "OK" : "FAILED");

    // Inicia thread de leitura SharedMemory
    if (!readerRunning.load()) {
        readerRunning.store(true);
        pthread_create(&readerThread, nullptr, shmReaderLoop, nullptr);
        MLOGI("shmReaderLoop started");
    }

    // Carregar configuracoes salvas
    loadConfig();

    // Verificar status do servidor (background)
    {
        pthread_t t;
        pthread_create(&t, nullptr, serverCheckThread, nullptr);
        pthread_detach(t);
    }

    // Iniciar thread de hotkeys (volume keys)
    if (!g_hotkeyRunning.load()) {
        g_hotkeyRunning.store(true);
        pthread_create(&g_hotkeyThread, nullptr, hotkeyThread, nullptr);
    }
}

JNIEXPORT void JNICALL
Java_com_android_support_OverlayService_nativeOnSurfaceDestroyed(
        JNIEnv*, jclass) {

    // Para hotkey thread
    g_hotkeyRunning.store(false);
    if (g_hotkeyThread) {
        pthread_join(g_hotkeyThread, nullptr);
        g_hotkeyThread = 0;
    }

    // Para reader thread
    readerRunning.store(false);
    if (readerThread) {
        pthread_join(readerThread, nullptr);
        readerThread = 0;
    }

    Overlay::get().destroy();
}

JNIEXPORT void JNICALL
Java_com_android_support_OverlayService_nativeOnTouch(
        JNIEnv*, jclass, jint action, jfloat x, jfloat y) {

    Overlay::get().handleTouch(action, x, y);
}

JNIEXPORT void JNICALL
Java_com_android_support_OverlayService_nativeSetScreenSize(
        JNIEnv*, jclass, jint width, jint height) {

    Overlay::get().setScreenSize(width, height);
}

JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}

} // extern "C"
