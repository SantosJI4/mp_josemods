/*
 * ============================================================
 * GameHook.cpp - Injetado no processo do JOGO via script root
 * ============================================================
 *
 * TÉCNICA: VMT Hook (Virtual Method Table)
 *   - NÃO usa Dobby/Inline Hook (sem JMP no código)
 *   - Troca o ponteiro methodPointer dentro do MethodInfo
 *   - O anti-cheat escaneia código por patches → nós não patcheamos código
 *   - Só trocamos um ponteiro na estrutura de dados do il2cpp
 *
 * FLUXO:
 *   1. lib_main() → aguarda libil2cpp.so carregar
 *   2. Il2CppAttach → resolve APIs
 *   3. Resolve seus 5 offsets (OnUpdate, Camera, Transform, etc.)
 *   4. VMT hook no OnUpdate → jogo chama nosso hook automaticamente
 *   5. No hook: chama get_main, get_transform, get_position, WorldToScreenPoint
 *   6. Escreve resultado no SharedMemory
 *   7. Overlay externo lê e desenha
 *
 * ZERO busca de ponteiros - o jogo entrega tudo no hook
 * ============================================================
 */

#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <cstring>
#include <cmath>
#include <atomic>
#include <cerrno>
#include <android/log.h>

#include "Utils.h"
#include "Vector3.h"
#include "obfuscate.h"
#include "SharedData.h"

// ByNameModding
#include "ByNameModding/fake_dlfcn.h"
#include "ByNameModding/Il2Cpp.h"

#define HOOK_TAG "GameHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, HOOK_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, HOOK_TAG, __VA_ARGS__)

// ============================================================
// CONFIGURAÇÃO DE NOMES (do dump.cs)
// Se os nomes estão obfuscados no seu jogo, troque aqui
// ============================================================
#define ASSEMBLY_CS   "Assembly-CSharp.dll"
#define ASSEMBLY_UE   "UnityEngine.dll"

#define NS_PLAYER     ""                    // namespace do PersonTarget
#define CLS_PLAYER    "PersonTarget"        // nome da classe
#define MTD_ONUPDATE  "Update"              // método hookado

#define NS_CAMERA     "UnityEngine"
#define CLS_CAMERA    "Camera"

#define NS_COMPONENT  "UnityEngine"
#define CLS_COMPONENT "Component"

#define NS_TRANSFORM  "UnityEngine"
#define CLS_TRANSFORM "Transform"

#define NS_SCREEN     "UnityEngine"
#define CLS_SCREEN    "Screen"

// ============================================================
// Function Pointers — resolvidos em runtime pelo il2cpp
// ============================================================
static void*   (*fn_Camera_get_main)() = nullptr;
static void*   (*fn_get_transform)(void* component) = nullptr;
static Vector3 (*fn_get_position)(void* transform) = nullptr;
static int     (*fn_Screen_get_width)(void*) = nullptr;
static int     (*fn_Screen_get_height)(void*) = nullptr;

// Para VP Matrix (Camera) — Matrix4x4 tem 64 bytes, usa hidden return pointer no ARM64
struct Matrix4x4 { float m[16]; };
static void (*fn_get_worldToCameraMatrix)(Matrix4x4* ret, void* camera) = nullptr;
static void (*fn_get_projectionMatrix)(Matrix4x4* ret, void* camera) = nullptr;

// Fallback: WorldToScreenPoint direto (pode travar se chamado muitas vezes)
static Vector3 (*fn_WorldToScreenPoint)(void* camera, Vector3 pos, int eye) = nullptr;
static bool useManualW2S = true; // true = VP matrix, false = fallback direto

// il2cpp_class_get_method_from_name — resolvido direto do libil2cpp.so
static void* (*resolve_method)(void* klass, const char* name, int argsCount) = nullptr;

// Original OnUpdate
static void (*orig_OnUpdate)(void* self) = nullptr;

// Shared memory
static SharedESPData* sharedData = nullptr;
static int shmFd = -1;

// Frame sync
static std::atomic<bool> hookActive{false};

// Cache por frame
static void* g_cachedCamera = nullptr;
static Matrix4x4 g_vpMatrix;
static bool g_vpValid = false;
static int g_frameId = 0;

// ============================================================
// VMT HOOK — Troca methodPointer no MethodInfo
// ============================================================
//
// MethodInfo (il2cpp internals):
//   struct MethodInfo {
//       Il2CppMethodPointer methodPointer; // offset 0x00 ← trocamos isso
//       void* invoker_method;              // offset 0x08
//       const char* name;                  // offset 0x10
//       ...
//   };
//
// il2cpp_class_get_method_from_name retorna MethodInfo*
// O primeiro campo é o ponteiro da função
// Trocar esse campo = VMT Hook
//   → Sem patchear código executável
//   → Só muda um ponteiro em data section
//   → Muito mais difícil de detectar

static bool VmtHook(void* methodInfo, void* newFunc, void** origFunc) {
    if (!methodInfo) return false;

    // MethodInfo->methodPointer está no offset 0
    void** methodPtr = (void**)methodInfo;

    // Salvar original
    *origFunc = *methodPtr;

    // Tornar a página writable (pode cruzar boundary de página)
    uintptr_t pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t pageAddr = (uintptr_t)methodPtr & ~(pageSize - 1);
    if (mprotect((void*)pageAddr, pageSize * 2, PROT_READ | PROT_WRITE) != 0) {
        LOGE("mprotect falhou: %s", strerror(errno));
        return false;
    }

    // Trocar o ponteiro (VMT swap)
    *methodPtr = newFunc;

    // Restaurar proteção (read-only)
    mprotect((void*)pageAddr, pageSize * 2, PROT_READ);

    LOGI("VMT Hook aplicado: orig=%p → new=%p", *origFunc, newFunc);
    return true;
}

// ============================================================
// Manual WorldToScreen — Evita chamar Camera.WorldToScreenPoint
// que pode causar deadlock no Update loop
// ============================================================
static void MultiplyMatrix(const Matrix4x4& a, const Matrix4x4& b, Matrix4x4& out) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            out.m[j * 4 + i] = 0;
            for (int k = 0; k < 4; k++) {
                out.m[j * 4 + i] += a.m[k * 4 + i] * b.m[j * 4 + k];
            }
        }
    }
}

static Vector3 ManualWorldToScreen(Vector3 world, const Matrix4x4& vp, int sw, int sh) {
    const float* m = vp.m;
    // Unity Matrix4x4 is column-major: m[col*4+row]
    float clipX = world.x * m[0] + world.y * m[4] + world.z * m[8]  + m[12];
    float clipY = world.x * m[1] + world.y * m[5] + world.z * m[9]  + m[13];
    float clipW = world.x * m[3] + world.y * m[7] + world.z * m[11] + m[15];

    if (clipW < 0.001f) return Vector3(0, 0, -1);

    float ndcX = clipX / clipW;
    float ndcY = clipY / clipW;

    float screenX = (ndcX * 0.5f + 0.5f) * (float)sw;
    float screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * (float)sh;

    return Vector3(screenX, screenY, clipW);
}

// ============================================================
// HOOK DO OnUpdate — Chamado pelo jogo para CADA player
// ============================================================

static void Hook_OnUpdate(void* self) {
    // Chamar original primeiro (mantém o jogo funcionando)
    if (orig_OnUpdate) {
        orig_OnUpdate(self);
    }

    // Se shared memory não está pronto ou hook desativado, retorna
    if (!sharedData || !self || !hookActive.load()) return;

    // Overlay controla se o hook processa
    if (!sharedData->espEnabled) return;

    int idx = sharedData->playerCount;
    if (idx >= MAX_ESP_PLAYERS || idx >= 20) return; // Limitar a 20 players

    // ── Primeiro player do frame: atualiza cache ──
    if (idx == 0) {
        g_frameId++;
        sharedData->debugLastCall = 10;

        // Camera.get_main — UMA VEZ por frame
        g_cachedCamera = fn_Camera_get_main ? fn_Camera_get_main() : nullptr;
        sharedData->debugLastCall = 11;

        // VP Matrix — UMA VEZ por frame (se disponível)
        g_vpValid = false;
        if (useManualW2S && g_cachedCamera && fn_get_worldToCameraMatrix && fn_get_projectionMatrix) {
            Matrix4x4 viewMat, projMat;
            fn_get_worldToCameraMatrix(&viewMat, g_cachedCamera);
            sharedData->debugLastCall = 12;
            fn_get_projectionMatrix(&projMat, g_cachedCamera);
            sharedData->debugLastCall = 13;
            MultiplyMatrix(projMat, viewMat, g_vpMatrix);
            g_vpValid = true;
        }

        // Tela
        if (sharedData->screenW <= 0 && fn_Screen_get_width && fn_Screen_get_height) {
            sharedData->screenW = fn_Screen_get_width(nullptr);
            sharedData->screenH = fn_Screen_get_height(nullptr);
        }
    }

    void* camera = g_cachedCamera;
    if (!camera) return;

    // Precisamos de VP matrix OU WorldToScreenPoint fallback
    if (!g_vpValid && !fn_WorldToScreenPoint) return;

    int screenH = sharedData->screenH;
    int screenW = sharedData->screenW;
    if (screenW <= 0 || screenH <= 0) return;

    // ── Pegar transform do player ──
    sharedData->debugLastCall = 20 + idx;
    void* transform = fn_get_transform ? fn_get_transform(self) : nullptr;
    if (!transform) return;

    // ── Pegar posição 3D do player ──
    sharedData->debugLastCall = 40 + idx;
    Vector3 worldPos = fn_get_position(transform);

    // Sanity check
    if (std::isnan(worldPos.x) || std::isnan(worldPos.y) || std::isnan(worldPos.z)) return;

    Vector3 bottomWorld(worldPos.x, worldPos.y, worldPos.z);
    Vector3 topWorld(worldPos.x, worldPos.y + 2.0f, worldPos.z);

    Vector3 screenBottom, screenTop;

    if (g_vpValid) {
        // ── Modo VP Matrix — MATH PURA, zero chamada il2cpp ──
        sharedData->debugLastCall = 60 + idx;
        screenBottom = ManualWorldToScreen(bottomWorld, g_vpMatrix, screenW, screenH);
        screenTop = ManualWorldToScreen(topWorld, g_vpMatrix, screenW, screenH);
    } else {
        // ── Fallback: WorldToScreenPoint direto ──
        // Chamado POR PLAYER — mais lento, mas funciona se VP matrix não resolve
        sharedData->debugLastCall = 80 + idx;
        screenBottom = fn_WorldToScreenPoint(camera, bottomWorld, 2); // MonoOrStereoscopicEye.Mono=2
        screenTop = fn_WorldToScreenPoint(camera, topWorld, 2);
        // Unity Y invertido
        screenBottom.y = (float)screenH - screenBottom.y;
        screenTop.y = (float)screenH - screenTop.y;
    }

    // Verificar se está na frente da câmera
    if (screenBottom.z < 0.1f || screenTop.z < 0.1f) return;

    // ── Escrever no SharedMemory ──
    ESPEntry& entry = sharedData->players[idx];
    entry.topX = screenTop.x;
    entry.topY = screenTop.y;
    entry.topZ = screenTop.z;
    entry.bottomX = screenBottom.x;
    entry.bottomY = screenBottom.y;
    entry.bottomZ = screenBottom.z;
    entry.distance = screenBottom.z;
    entry.team = 0;
    entry.health = 100.0f;
    entry.valid = true;

    sharedData->playerCount = idx + 1;
    sharedData->magic = 0xDEADF00D;
    sharedData->writeSeq.fetch_add(1, std::memory_order_release);
}

// ============================================================
// INIT — Resolve funções + aplica VMT hook
// ============================================================

static void* hack_thread(void*) {
    LOGI("Hook thread iniciada, aguardando libil2cpp.so...");

    // ── Aguardar il2cpp carregar ──
    while (!isLibraryLoaded("libil2cpp.so")) {
        sleep(1);
    }
    LOGI("libil2cpp.so detectada");

    // ── Resolver APIs do il2cpp via ByNameModding ──
    Il2CppAttach("libil2cpp.so");
    sleep(3); // Esperar assemblies carregarem

    // ── Resolver il2cpp_class_get_method_from_name DIRETO do libil2cpp.so ──
    // Precisamos do MethodInfo* para VMT hook (Il2CppGetMethodOffset retorna *method, não MethodInfo*)
    {
        void* handle = dlopen_ex("libil2cpp.so", 0);
        if (handle) {
            resolve_method = (void*(*)(void*, const char*, int))
                dlsym_ex(handle, "il2cpp_class_get_method_from_name");
            dlclose_ex(handle);
        }
    }
    if (!resolve_method) {
        LOGE("Falha ao resolver il2cpp_class_get_method_from_name");
        return nullptr;
    }

    // ── Resolver funções pelo NOME (usa seus offsets do dump automaticamente) ──
    fn_Camera_get_main = (void*(*)()) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_CAMERA),
        OBFUSCATE(CLS_CAMERA), OBFUSCATE("get_main"), 0);

    // VP Matrix: hidden return pointer para Matrix4x4 (64 bytes > 16 → ARM64 usa x8)
    fn_get_worldToCameraMatrix = (void(*)(Matrix4x4*, void*)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_CAMERA),
        OBFUSCATE(CLS_CAMERA), OBFUSCATE("get_worldToCameraMatrix"), 0);

    fn_get_projectionMatrix = (void(*)(Matrix4x4*, void*)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_CAMERA),
        OBFUSCATE(CLS_CAMERA), OBFUSCATE("get_projectionMatrix"), 0);

    // Fallback: WorldToScreenPoint(Vector3 pos, MonoOrStereoscopicEye eye) — 2 args
    fn_WorldToScreenPoint = (Vector3(*)(void*, Vector3, int)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_CAMERA),
        OBFUSCATE(CLS_CAMERA), OBFUSCATE("WorldToScreenPoint"), 2);
    // Tentar versão com 1 arg se 2 args falhou
    if (!fn_WorldToScreenPoint) {
        fn_WorldToScreenPoint = (Vector3(*)(void*, Vector3, int)) Il2CppGetMethodOffset(
            OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_CAMERA),
            OBFUSCATE(CLS_CAMERA), OBFUSCATE("WorldToScreenPoint"), 1);
    }

    // Decidir método de W2S
    if (fn_get_worldToCameraMatrix && fn_get_projectionMatrix) {
        useManualW2S = true;
        LOGI("Usando VP Matrix manual para W2S");
    } else if (fn_WorldToScreenPoint) {
        useManualW2S = false;
        LOGI("VP Matrix nao disponivel, usando WorldToScreenPoint fallback");
    } else {
        LOGE("Nenhum metodo W2S disponivel!");
    }

    fn_get_transform = (void*(*)(void*)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_COMPONENT),
        OBFUSCATE(CLS_COMPONENT), OBFUSCATE("get_transform"), 0);

    fn_get_position = (Vector3(*)(void*)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_TRANSFORM),
        OBFUSCATE(CLS_TRANSFORM), OBFUSCATE("get_position"), 0);

    fn_Screen_get_width = (int(*)(void*)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_SCREEN),
        OBFUSCATE(CLS_SCREEN), OBFUSCATE("get_width"), 0);

    fn_Screen_get_height = (int(*)(void*)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_SCREEN),
        OBFUSCATE(CLS_SCREEN), OBFUSCATE("get_height"), 0);

    // Verificar se tudo resolveu
    if (!fn_Camera_get_main || !fn_get_transform || !fn_get_position) {
        LOGE("Falha ao resolver funções: cam=%p trans=%p pos=%p",
             fn_Camera_get_main, fn_get_transform, fn_get_position);
        return nullptr;
    }
    LOGI("Funções resolvidas: cam=%p viewMat=%p projMat=%p wts=%p trans=%p pos=%p",
         fn_Camera_get_main, fn_get_worldToCameraMatrix, fn_get_projectionMatrix,
         fn_WorldToScreenPoint, fn_get_transform, fn_get_position);

    // ── Criar shared memory ──
    LOGI("Tentando criar shared memory... uid=%d", getuid());
    shmFd = shm_create_file();
    if (shmFd < 0) {
        LOGE("Falha ao criar shared memory: errno=%d (%s)", errno, strerror(errno));
        LOGE("uid=%d gid=%d", getuid(), getgid());
        return nullptr;
    }

    sharedData = shm_map(shmFd);
    if (!sharedData) {
        close(shmFd);
        LOGE("Falha ao mapear shared memory: errno=%d (%s)", errno, strerror(errno));
        return nullptr;
    }

    memset(sharedData, 0, sizeof(SharedESPData));
    LOGI("Shared memory criado em %s", shmActivePath ? shmActivePath : "???");

    // Preencher tamanho de tela
    if (fn_Screen_get_width && fn_Screen_get_height) {
        sharedData->screenW = fn_Screen_get_width(nullptr);
        sharedData->screenH = fn_Screen_get_height(nullptr);
        LOGI("Tela: %dx%d", sharedData->screenW, sharedData->screenH);
    }

    // ── VMT Hook no OnUpdate ──
    // Pegar o Il2CppClass* para bl_PlayerNetwork
    void* playerClass = Il2CppGetClassType(
        OBFUSCATE(ASSEMBLY_CS), OBFUSCATE(NS_PLAYER), OBFUSCATE(CLS_PLAYER));

    if (!playerClass) {
        LOGE("Classe %s não encontrada", CLS_PLAYER);
        return nullptr;
    }

    // Pegar MethodInfo* para OnUpdate (via il2cpp_class_get_method_from_name)
    void* onUpdateMethodInfo = resolve_method(playerClass, OBFUSCATE(MTD_ONUPDATE), 0);

    if (!onUpdateMethodInfo) {
        LOGE("Método %s não encontrado em %s", MTD_ONUPDATE, CLS_PLAYER);
        return nullptr;
    }

    // Aplicar VMT Hook
    if (!VmtHook(onUpdateMethodInfo, (void*)Hook_OnUpdate, (void**)&orig_OnUpdate)) {
        LOGE("VMT Hook falhou");
        return nullptr;
    }

    hookActive.store(true);
    LOGI("=== HOOK ATIVO === VMT hook em %s::%s", CLS_PLAYER, MTD_ONUPDATE);

    return nullptr;
}

// ============================================================
// ENTRY POINT — Carregado quando o .so é injetado no jogo
// ============================================================
__attribute__((constructor))
void lib_main() {
    LOGI("libHook.so carregada no processo do jogo");
    pthread_t t;
    pthread_create(&t, nullptr, hack_thread, nullptr);
    pthread_detach(t);
}
