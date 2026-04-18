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
// Silent Aim State
// ============================================================
static bool  silentAim = false;

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

    // ── Sincronizar configurações do Aim Assist com o hook ──────────────────
    sharedData->aimAssistEnabled  = aimAssist ? 1 : 0;
    sharedData->aimAssistSpeed    = aimSpeed;
    sharedData->aimAssistFovDeg   = aimFovDeg;
    sharedData->aimAssistDeadzone = aimDeadzone;
    // ── Sincronizar Silent Aim ───────────────────────────────────────────────
    sharedData->silentAimEnabled  = silentAim ? 1 : 0;
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
    ImVec2 screenTopLine = ImVec2(screenW * linePositionX, 0.0f);
    auto* draw = ImGui::GetBackgroundDrawList();

    // Verificar se há pelo menos um alvo válido na tela
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

        // Snap line: from top-center of screen to center of body
        if (drawSnapLine) {
            float bodyCenterY = (top + bot) * 0.5f;
            draw->AddLine(screenTopLine,
                         ImVec2(centerX, bodyCenterY), boxColor, 1.2f);
        }

        // Distancia embaixo do box
        if (drawDistance) {
            char distText[16];
            snprintf(distText, sizeof(distText), "%.0fm", entry.distance);
            draw->AddText(ImVec2(centerX - 12, bot + padY + 2), boxColor, distText);
        }
    }

    // ── Aim Assist: círculo de FOV visual ────────────────────────────────────
    if (aimAssist) {
        float fovRadiusPx = (float)screenW * (aimFovDeg / 90.0f) * 0.5f;
        bool  locked      = sharedData && sharedData->aimAssistHasTarget;
        ImU32 circleColor = locked
            ? IM_COL32(0, 230, 100, 140)   // verde quando travado
            : IM_COL32(255, 255, 255, 55);  // branco tênue quando procurando
        draw->AddCircle(
            ImVec2(screenW * 0.5f, screenH * 0.5f),
            fovRadiusPx, circleColor, 64, 1.2f);
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
#define JAW_CONFIG_MAGIC 0x4A415703u  // "JAW" v3

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
    c.silentAim    = silentAim;
    c.hotkeyAim    = hotkeyAim;
    c.hotkeySilent = hotkeySilent;
    c.hotkeyEsp    = hotkeyEsp;
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
    silentAim     = c.silentAim;
    hotkeyAim     = c.hotkeyAim;
    hotkeySilent  = c.hotkeySilent;
    hotkeyEsp     = c.hotkeyEsp;
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
        if (ev.type != EV_KEY || ev.value != 1) continue;  // só key-press
        if (hotkeyAim    > 0 && ev.code == (uint16_t)hotkeyAim) {
            aimAssist = !aimAssist;
            if (sharedData) {
                sharedData->aimAssistEnabled = aimAssist ? 1 : 0;
                if (!aimAssist) sharedData->aimAssistHasTarget = 0;
            }
        }
        if (hotkeySilent > 0 && ev.code == (uint16_t)hotkeySilent) {
            silentAim = !silentAim;
            if (sharedData) sharedData->silentAimEnabled = silentAim ? 1 : 0;
        }
        if (hotkeyEsp > 0 && ev.code == (uint16_t)hotkeyEsp)
            esp = !esp;
    }
    close(evfd);
    return nullptr;
}

// ============================================================
// Draw Menu — Tabs: SOBRE / ESP / AIM / MISC
// ============================================================
void DrawMenu() {
    ImVec4 green   = ImVec4(0.00f, 0.90f, 0.46f, 1.00f);
    ImVec4 textDim = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);
    ImVec4 red     = ImVec4(1.00f, 0.32f, 0.32f, 1.00f);
    ImVec4 yellow  = ImVec4(1.00f, 0.80f, 0.00f, 1.00f);

    bool shmReady   = shmConnected.load() && sharedData && sharedData->magic == 0xDEADF00D;
    bool vmtApplied = shmReady && sharedData->hookApplied == 0xBEEF1234;

    ImGui::SetNextWindowSize(ImVec2(580, 480), ImGuiCond_Once);
    ImGui::Begin("##jw", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);

    // ── Header ───────────────────────────────────────────────────────────────
    ImGui::TextColored(green, "JAWMODS");
    ImGui::SameLine(0, 10);
    if (vmtApplied)
        ImGui::TextColored(green, "[HOOK]");
    else if (shmReady)
        ImGui::TextColored(yellow, "[INIT %d]", sharedData->debugLastCall);
    else
        ImGui::TextColored(red, "[WAIT]");
    ImGui::SameLine(0, 10);
    ImGui::TextColored(g_serverOnline.load() ? green : textDim, "SRV: %s", g_serverStatus);
    if (vmtApplied && sharedData) {
        ImGui::SameLine(0, 10);
        ImGui::TextColored(textDim, "| %d enemy | %dx%d",
            sharedData->playerCount, sharedData->screenW, sharedData->screenH);
    }
    ImGui::Separator();

    // ── Tab Bar ──────────────────────────────────────────────────────────────
    if (ImGui::BeginTabBar("##tabs")) {

        // ═══════════════════════════════════════════════════════════════════
        // TAB: SOBRE
        // ═══════════════════════════════════════════════════════════════════
        if (ImGui::BeginTabItem("SOBRE")) {
            ImGui::Spacing();

            float bw = ImGui::CalcTextSize("  J A W M O D S  ").x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
            ImGui::TextColored(green, "  J A W M O D S  ");
            ImGui::Spacing();

            ImGui::Columns(2, "##about", false);
            ImGui::SetColumnWidth(0, 110.0f);
            ImGui::TextColored(textDim, "Versao");
            ImGui::TextColored(textDim, "Jogo");
            ImGui::TextColored(textDim, "Servidor");
            ImGui::TextColored(textDim, "Hook");
            ImGui::TextColored(textDim, "Criado por");
            ImGui::NextColumn();
            ImGui::TextColored(green, "v24");
            ImGui::Text("Free Fire 1.123.1");
            if (g_serverOnline.load())
                ImGui::TextColored(green, "Online");
            else if (strncmp(g_serverStatus, "Verif", 5) == 0)
                ImGui::TextColored(yellow, "%s", g_serverStatus);
            else
                ImGui::TextColored(red, "%s  (%s)", g_serverStatus, SERVER_CHECK_HOST);
            if (vmtApplied)
                ImGui::TextColored(green, "Ativo  (%d inimigo(s))", sharedData ? sharedData->playerCount : 0);
            else if (shmReady)
                ImGui::TextColored(yellow, "Iniciando (stage=%d)", sharedData ? sharedData->debugLastCall : 0);
            else
                ImGui::TextColored(red, "Aguardando...");
            ImGui::TextColored(green, "jawmods");
            ImGui::Columns(1);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Verificar servidor", ImVec2(180, 0))) {
                snprintf(g_serverStatus, sizeof(g_serverStatus), "Verificando...");
                g_serverOnline.store(false);
                pthread_t t;
                pthread_create(&t, nullptr, serverCheckThread, nullptr);
                pthread_detach(t);
            }
            ImGui::EndTabItem();
        }

        // ═══════════════════════════════════════════════════════════════════
        // TAB: ESP
        // ═══════════════════════════════════════════════════════════════════
        if (ImGui::BeginTabItem("ESP")) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_CheckMark, green);
            ImGui::Checkbox("  ESP Ativo", &esp);
            ImGui::PopStyleColor();

            if (esp) {
                ImGui::Spacing();
                ImGui::TextColored(textDim, "VISUALS");
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_CheckMark, green);
                ImGui::Checkbox("Box", &drawEnemyBox);
                ImGui::SameLine(0, 16);
                ImGui::Checkbox("Snapline", &drawSnapLine);
                ImGui::SameLine(0, 16);
                ImGui::Checkbox("Distancia", &drawDistance);
                ImGui::PopStyleColor();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(textDim, "CORES");
                ImGui::Spacing();
                ImGui::ColorEdit4("Inimigo##ce", (float*)&espLineColor,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                ImGui::SameLine(0, 8);
                ImGui::TextColored(textDim, "(box + snapline)");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(textDim, "CONFIGURACOES");
                ImGui::Spacing();
                ImGui::PushItemWidth(-1);
                ImGui::SliderFloat("##dist", &espMaxDistance, 10.0f, 999.0f, "Dist Max: %.0fm");
                ImGui::SliderFloat("##line", &linePositionX, 0.0f, 1.0f, "Origem Linha: %.2f");
                ImGui::PopItemWidth();
            }
            ImGui::EndTabItem();
        }

        // ═══════════════════════════════════════════════════════════════════
        // TAB: AIM
        // ═══════════════════════════════════════════════════════════════════
        if (ImGui::BeginTabItem("AIM")) {
            ImGui::Spacing();
            ImGui::TextColored(textDim, "HEAD MAGNETISM");
            ImGui::Spacing();

            bool prevAim = aimAssist;
            ImGui::PushStyleColor(ImGuiCol_CheckMark, green);
            ImGui::Checkbox("  Head Magnetism", &aimAssist);
            ImGui::PopStyleColor();
            if (aimAssist != prevAim && sharedData) {
                sharedData->aimAssistEnabled = aimAssist ? 1 : 0;
                if (!aimAssist) sharedData->aimAssistHasTarget = 0;
            }
            if (aimAssist) {
                bool locked = sharedData && sharedData->aimAssistHasTarget;
                ImGui::SameLine(0, 12);
                if (locked) ImGui::TextColored(green, "[HEAD LOCK]");
                else        ImGui::TextColored(textDim, "[buscando...]");
                ImGui::Spacing();
                ImGui::PushItemWidth(-1);
                if (ImGui::SliderFloat("##as", &aimSpeed,    0.5f, 8.0f,  "Sensi: %.1f graus/frame"))
                    if (sharedData) sharedData->aimAssistSpeed = aimSpeed;
                if (ImGui::SliderFloat("##af", &aimFovDeg,   5.0f, 60.0f, "FOV Cone: %.0f graus"))
                    if (sharedData) sharedData->aimAssistFovDeg = aimFovDeg;
                if (ImGui::SliderFloat("##ad", &aimDeadzone, 0.0f, 5.0f,  "Deadzone: %.1f graus"))
                    if (sharedData) sharedData->aimAssistDeadzone = aimDeadzone;
                ImGui::PopItemWidth();
                ImGui::Spacing();
                ImGui::TextColored(textDim, "Sensi baixa = suave  |  FOV menor = seletivo  |  Deadzone = sem jitter");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(textDim, "SILENT AIM");
            ImGui::Spacing();
            bool prevSilent = silentAim;
            ImGui::PushStyleColor(ImGuiCol_CheckMark, green);
            ImGui::Checkbox("  Silent Aim", &silentAim);
            ImGui::PopStyleColor();
            if (silentAim != prevSilent && sharedData)
                sharedData->silentAimEnabled = silentAim ? 1 : 0;
            if (silentAim) {
                ImGui::SameLine(0, 12);
                ImGui::TextColored(green, "[HEADSHOT AUTO]");
                ImGui::Spacing();
                ImGui::TextColored(textDim, "Todo tiro vai na cabeca — mira nao se move visivelmente");
            }
            ImGui::EndTabItem();
        }

        // ═══════════════════════════════════════════════════════════════════
        // TAB: MISC
        // ═══════════════════════════════════════════════════════════════════
        if (ImGui::BeginTabItem("MISC")) {
            ImGui::Spacing();
            ImGui::TextColored(textDim, "HOTKEYS  (Volume Keys)");
            ImGui::Spacing();

            const char* knames[] = { "OFF", "Vol-  (114)", "Vol+  (115)" };
            int aidx = (hotkeyAim    == 114) ? 1 : (hotkeyAim    == 115) ? 2 : 0;
            int sidx = (hotkeySilent == 114) ? 1 : (hotkeySilent == 115) ? 2 : 0;
            int eidx = (hotkeyEsp    == 114) ? 1 : (hotkeyEsp    == 115) ? 2 : 0;

            ImGui::Text("Head Mag:");   ImGui::SameLine(130); ImGui::SetNextItemWidth(160);
            if (ImGui::Combo("##hka", &aidx, knames, 3))
                hotkeyAim = (aidx == 1) ? 114 : (aidx == 2) ? 115 : 0;
            ImGui::Text("Silent Aim:"); ImGui::SameLine(130); ImGui::SetNextItemWidth(160);
            if (ImGui::Combo("##hks", &sidx, knames, 3))
                hotkeySilent = (sidx == 1) ? 114 : (sidx == 2) ? 115 : 0;
            ImGui::Text("ESP:");        ImGui::SameLine(130); ImGui::SetNextItemWidth(160);
            if (ImGui::Combo("##hke", &eidx, knames, 3))
                hotkeyEsp = (eidx == 1) ? 114 : (eidx == 2) ? 115 : 0;

            ImGui::Spacing();
            ImGui::TextColored(textDim, "Pressione o botao de volume para toggle rapido");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(textDim, "FILTROS");
            ImGui::Spacing();
            ImGui::TextColored(green, "[AUTO]");
            ImGui::SameLine(0, 8);
            ImGui::Text("Self + Team (automatico)");
            ImGui::SameLine(0, 8);
            if (ImGui::SmallButton("Reset Self")) {
                if (sharedData) sharedData->resetSelf = 1;
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(textDim, "CONFIGURACOES");
            ImGui::Spacing();
            if (ImGui::Button("Salvar Configuracoes", ImVec2(-1, 0)))
                saveConfig();
            ImGui::Spacing();
            ImGui::TextColored(textDim, "Salvo em: " JAW_CONFIG_PATH);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
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
