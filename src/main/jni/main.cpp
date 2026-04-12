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

// ============================================================
// SharedMemory Reader — Conecta ao arquivo criado pelo hook
// ============================================================
static std::atomic<bool> readerRunning{false};
static pthread_t readerThread = 0;

void* shmReaderLoop(void*) {
    while (readerRunning.load()) {
        // Tentar abrir o arquivo compartilhado
        int fd = shm_open_file();
        if (fd >= 0) {
            SharedESPData* data = shm_map(fd);
            if (data && data->magic == 0xDEADF00D) {
                shmFd = fd;
                sharedData = data;
                shmConnected.store(true);
                break;
            }
            if (data) shm_unmap(data);
            close(fd);
        }
        sleep(1); // Retry a cada segundo
    }

    // Mantém a thread viva para reconexão se necessário
    while (readerRunning.load()) {
        if (sharedData && sharedData->magic != 0xDEADF00D) {
            shmConnected.store(false);
            // Hook saiu, tentar reconectar
            shm_unmap(sharedData);
            sharedData = nullptr;
            close(shmFd);
            shmFd = -1;

            // Retry
            while (readerRunning.load()) {
                int fd = shm_open_file();
                if (fd >= 0) {
                    SharedESPData* data = shm_map(fd);
                    if (data && data->magic == 0xDEADF00D) {
                        shmFd = fd;
                        sharedData = data;
                        shmConnected.store(true);
                        break;
                    }
                    if (data) shm_unmap(data);
                    close(fd);
                }
                sleep(1);
            }
        }
        usleep(500000); // Check a cada 500ms
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

    // Controle: overlay diz ao hook se deve processar
    sharedData->espEnabled = esp ? 1 : 0;

    if (!esp) return;

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
        if (boxHeight < 5.0f) continue; // Muito pequeno
        float boxWidth = boxHeight * 0.6f;

        float centerX = (entry.topX + entry.bottomX) * 0.5f;

        ImVec2 boxMin = ImVec2(centerX - (boxWidth * 0.5f), entry.topY);
        ImVec2 boxMax = ImVec2(centerX + (boxWidth * 0.5f), entry.bottomY);

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
// Draw Menu
// ============================================================
void DrawMenu() {
    const ImVec2 windowSize = ImVec2(700, 600);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Once);
    ImGui::Begin("OVERLAY MENU", nullptr, ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ── Status ──
    if (shmConnected.load()) {
        int count = sharedData ? sharedData->playerCount : 0;
        uint32_t seq = sharedData ? sharedData->writeSeq.load(std::memory_order_relaxed) : 0;
        int dbg = sharedData ? sharedData->debugLastCall : -1;
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "[HOOK] Conectado | Players: %d | Seq: %u",
                          count, seq);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 1, 1), "Debug: %d | ESP: %s",
                          dbg, esp ? "ON" : "OFF");
    } else {
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "[HOOK] Aguardando hook no jogo...");
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1),
            "Execute o script root para injetar o hook");
    }

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
