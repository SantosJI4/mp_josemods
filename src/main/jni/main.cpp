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
// Draw Menu
// ============================================================
void DrawMenu() {
    ImVec4 green = ImVec4(0.00f, 0.90f, 0.46f, 1.00f);
    ImVec4 greenDim = ImVec4(0.00f, 0.55f, 0.28f, 1.00f);
    ImVec4 textDim = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);
    ImVec4 red = ImVec4(1.00f, 0.32f, 0.32f, 1.00f);

    const ImVec2 windowSize = ImVec2(620, 400);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Once);
    ImGui::Begin("JAWMODS", nullptr, ImGuiWindowFlags_None);

    // ── Status ──
    bool shmReady   = shmConnected.load() && sharedData && sharedData->magic == 0xDEADF00D;
    bool vmtApplied = shmReady && sharedData->hookApplied == 0xBEEF1234;
    if (vmtApplied) {
        ImGui::TextColored(green, "Connected");
        ImGui::SameLine();
        ImGui::TextColored(textDim, "| %d inimigos", sharedData->playerCount);
    } else if (shmReady) {
        // SHM criado mas VmtHook ainda nao aplicado (aguardando domain/init)
        ImGui::TextColored(ImVec4(1.0f,0.65f,0.0f,1.0f), "Hook iniciando...");
        ImGui::SameLine();
        ImGui::TextColored(textDim, "stage=%d", sharedData->debugLastCall);
    } else {
        ImGui::TextColored(red, "Waiting for hook...");
    }

    // Debug line: sempre mostra stage, screenW, screenH
    if (sharedData) {
        ImGui::SameLine();
        ImGui::TextColored(textDim, " dbg=%d %dx%d",
            sharedData->debugLastCall, sharedData->screenW, sharedData->screenH);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Filtros ──
    ImGui::TextColored(textDim, "FILTROS");
    ImGui::Spacing();
    ImGui::TextColored(green, "[ON]");
    ImGui::SameLine();
    ImGui::Text("Self filter (auto)");
    ImGui::SameLine(0, 10);
    // Botao de reset do cache do self player (util se cachear errado)
    if (ImGui::SmallButton("Reset Self")) {
        if (sharedData) sharedData->resetSelf = 1;
    }
    ImGui::TextColored(textDim, "[OFF]");
    ImGui::SameLine();
    ImGui::Text("Team filter (aguardando offset do dump)");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── ESP Toggle ──
    ImGui::PushStyleColor(ImGuiCol_CheckMark, green);
    ImGui::Checkbox("  ESP", &esp);
    ImGui::PopStyleColor();

    if (esp) {
        ImGui::Spacing();

        // ── Visuals (inline) ──
        ImGui::TextColored(textDim, "VISUALS");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_CheckMark, green);
        ImGui::Checkbox("Box", &drawEnemyBox);
        ImGui::SameLine(0, 20);
        ImGui::Checkbox("Snapline", &drawSnapLine);
        ImGui::SameLine(0, 20);
        ImGui::Checkbox("Distance", &drawDistance);
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Settings ──
        ImGui::TextColored(textDim, "SETTINGS");
        ImGui::Spacing();
        ImGui::ColorEdit4("##color", (float*)&espLineColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SameLine();
        ImGui::Text("ESP Color");
        ImGui::PushItemWidth(-1);
        ImGui::SliderFloat("##dist", &espMaxDistance, 10.0f, 999.0f, "Max Distance: %.0fm");
        ImGui::SliderFloat("##line", &linePositionX, 0.0f, 1.0f, "Line Origin: %.2f");
        ImGui::PopItemWidth();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Aim Assist ──────────────────────────────────────────────────────────
    ImGui::TextColored(textDim, "AIM ASSIST");
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
        // Indicador de alvo travado
        bool locked = sharedData && sharedData->aimAssistHasTarget;
        ImGui::SameLine(0, 12);
        if (locked)
            ImGui::TextColored(green, "[HEAD LOCK]");
        else
            ImGui::TextColored(textDim, "[buscando...]");

        ImGui::Spacing();
        ImGui::PushItemWidth(-1);

        // ── Sensi (velocidade do puxão em graus/frame) ──
        if (ImGui::SliderFloat("##aimspeed", &aimSpeed, 0.5f, 8.0f, "Sensi: %.1f deg/frame")) {
            if (sharedData) sharedData->aimAssistSpeed = aimSpeed;
        }

        // ── Cone de FOV ──
        if (ImGui::SliderFloat("##aimfov", &aimFovDeg, 5.0f, 60.0f, "FOV Cone: %.0f deg")) {
            if (sharedData) sharedData->aimAssistFovDeg = aimFovDeg;
        }

        // ── Deadzone (anti-jitter) ──
        if (ImGui::SliderFloat("##aimdz", &aimDeadzone, 0.0f, 5.0f, "Deadzone: %.1f deg")) {
            if (sharedData) sharedData->aimAssistDeadzone = aimDeadzone;
        }

        ImGui::PopItemWidth();
        ImGui::Spacing();
        ImGui::TextColored(textDim, "Sensi baixa = mais suave | FOV menor = mais seletivo | Deadzone evita tremida");
    }
    // ────────────────────────────────────────────────────────────────────────

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
}

JNIEXPORT void JNICALL
Java_com_android_support_OverlayService_nativeOnSurfaceDestroyed(
        JNIEnv*, jclass) {

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
