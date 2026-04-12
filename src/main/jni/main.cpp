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
static bool esp = false;
static bool drawEnemyBox = true;
static bool drawSnapLine = true;
static bool drawDistance = false;
static ImVec4 espLineColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
static float espMaxDistance = 999.0f;
static float linePositionX = 0.5f;

// SharedMemory (leitura do hook injetado no jogo)
static SharedESPData* sharedData = nullptr;
static int shmFd = -1;
static std::atomic<bool> shmConnected{false};
static uint32_t lastWriteSeq = 0;

// Game package — para encontrar SHM no data dir do jogo
#define GAME_PACKAGE "com.fungames.sniper3d"

// ============================================================
// SharedMemory Reader — Conecta ao arquivo criado pelo hook
// Tenta: 1) /data/data/<game>/.esp_shm, 2) /data/local/tmp/, 3) /sdcard/
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

    // SEMPRE setar espEnabled (mesmo se ESP desligado no UI)
    // Isso comunica ao hook no jogo se deve processar players
    sharedData->espEnabled = esp ? 1 : 0;

    if (!esp) return;

    // Verificar se hook escreveu dados válidos
    if (sharedData->magic != 0xDEADF00D) return;

    // Ler dados do shared memory (escrito pelo hook no jogo)
    uint32_t seq = sharedData->writeSeq.load(std::memory_order_acquire);
    int count = sharedData->playerCount;

    if (count <= 0 || count > MAX_ESP_PLAYERS) return;

    ImU32 color = ImGui::ColorConvertFloat4ToU32(espLineColor);
    ImVec2 screenTopLine = ImVec2(screenW * linePositionX, 0.0f);
    auto* draw = ImGui::GetBackgroundDrawList();

    for (int i = 0; i < count; i++) {
        const ESPEntry& entry = sharedData->players[i];
        if (!entry.valid) continue;

        // Filtro de distância
        if (entry.distance > espMaxDistance) continue;

        float boxHeight = fabsf(entry.bottomY - entry.topY);
        if (boxHeight < 3.0f) continue; // Muito pequeno
        float boxWidth = boxHeight * 0.5f; // Proporcao mais justa

        // Usar bottomX como referencia primaria (pes sao mais estaveis)
        float centerX = entry.bottomX;

        // Padding vertical para cobrir melhor o modelo
        float padY = boxHeight * 0.05f;
        ImVec2 boxMin = ImVec2(centerX - (boxWidth * 0.5f), entry.topY - padY);
        ImVec2 boxMax = ImVec2(centerX + (boxWidth * 0.5f), entry.bottomY + padY);

        // Box
        if (drawEnemyBox) {
            draw->AddRect(boxMin, boxMax, color, 0.0f, 15, 1.5f);
        }

        // Snap line
        if (drawSnapLine) {
            draw->AddLine(screenTopLine,
                         ImVec2(centerX, entry.topY), color, 1.5f);
        }

        // Distância
        if (drawDistance) {
            char distText[16];
            snprintf(distText, sizeof(distText), "%.0fm", entry.distance);
            draw->AddText(ImVec2(centerX - 15, entry.bottomY + 2), color, distText);
        }
    }

    // Resetar playerCount para o hook escrever de novo no próximo frame
    // (O hook incrementa, overlay reseta)
    sharedData->playerCount = 0;
    lastWriteSeq = seq;
}

// ============================================================
// Hook Log Reader — lê o arquivo de log do hook para diagnostico
// Tenta: game dir, /data/local/tmp/, /sdcard/
// ============================================================
#define HOOK_LOG_PATH_1 "/data/local/tmp/.hook_log"
#define HOOK_LOG_PATH_2 "/sdcard/.hook_log"
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
    const ImVec2 windowSize = ImVec2(700, 600);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Once);
    ImGui::Begin("OVERLAY MENU", nullptr, ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ── Status ──
    if (shmConnected.load() && sharedData) {
        uint32_t magic = sharedData->magic;
        int count = sharedData->playerCount;
        uint32_t seq = sharedData->writeSeq.load(std::memory_order_relaxed);
        int dbg = sharedData->debugLastCall;
        int espOn = sharedData->espEnabled;

        if (magic == 0xDEADF00D) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "[HOOK] Conectado | Players: %d | Seq: %u",
                              count, seq);
        } else {
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "[SHM] Conectado (magic=0x%08X, hook nao escreveu)",
                              magic);
        }
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 1, 1), "Debug: %d | espEnabled: %d | ESP UI: %s",
                          dbg, espOn, esp ? "ON" : "OFF");
    } else {
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "[SHM] Aguardando conexao...");
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "%s", shmStatus);
    }

    // ── Hook Log (diagnostico do processo do jogo) ──
    readHookLog();
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1), "Hook Log:");
    ImGui::TextWrapped("%s", hookLogBuf);

    ImGui::Separator();

    // ── ESP ──
    ImGui::SeparatorText("ESP");
    ImGui::Checkbox("Ativar ESP", &esp);
    ImGui::Checkbox("Box", &drawEnemyBox);
    ImGui::Checkbox("Snap Line", &drawSnapLine);
    ImGui::Checkbox("Distancia", &drawDistance);

    if (esp) {
        ImGui::ColorEdit4("Cor", (float*)&espLineColor, ImGuiColorEditFlags_NoInputs);
        ImGui::SliderFloat("Distancia Max", &espMaxDistance, 10.0f, 999.0f, "%.0f");
        ImGui::SliderFloat("Linha X", &linePositionX, 0.0f, 1.0f, "%.2f");
    }

    ImGui::End();
}

// ============================================================
// Overlay Draw Callback - Chamado a cada frame pelo Overlay
// ============================================================
void onOverlayDraw(int screenW, int screenH) {
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
