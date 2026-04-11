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
static Vector3 (*fn_WorldToScreenPoint)(void* camera, Vector3 pos) = nullptr;
static void*   (*fn_Camera_get_main)() = nullptr;
static void*   (*fn_get_transform)(void* component) = nullptr;
static Vector3 (*fn_get_position)(void* transform) = nullptr;
static int     (*fn_Screen_get_width)(void*) = nullptr;
static int     (*fn_Screen_get_height)(void*) = nullptr;

// il2cpp_class_get_method_from_name — resolvido direto do libil2cpp.so
// Retorna MethodInfo* (NÃO o function pointer — precisamos do MethodInfo para VMT hook)
static void* (*resolve_method)(void* klass, const char* name, int argsCount) = nullptr;

// Original OnUpdate
static void (*orig_OnUpdate)(void* self) = nullptr;

// Shared memory
static SharedESPData* sharedData = nullptr;
static int shmFd = -1;

// Frame sync: o overlay zera playerCount quando lê
static std::atomic<bool> hookActive{false};

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
// HOOK DO OnUpdate — Chamado pelo jogo para CADA player
// ============================================================
//
// O jogo chama: bl_PlayerNetwork::OnUpdate(this)
// O 'this' é o ponteiro do player — entregue de graça, zero busca
//
// Aqui dentro chamamos as funções do Unity DIRETAMENTE
// usando os mesmos offsets do seu dump — sem ponteiros manuais

static void Hook_OnUpdate(void* self) {
    // Chamar original primeiro (mantém o jogo funcionando)
    if (orig_OnUpdate) {
        orig_OnUpdate(self);
    }

    // Se shared memory não está pronto ou hook desativado, retorna
    if (!sharedData || !self || !hookActive.load()) return;

    // ── Pegar câmera principal ──
    void* camera = fn_Camera_get_main ? fn_Camera_get_main() : nullptr;
    if (!camera) return;

    // ── Pegar transform do player ──
    void* transform = fn_get_transform ? fn_get_transform(self) : nullptr;
    if (!transform) return;

    // ── Pegar posição 3D do player ──
    Vector3 worldPos = fn_get_position(transform);

    // Sanity check
    if (std::isnan(worldPos.x) || std::isnan(worldPos.y) || std::isnan(worldPos.z)) return;

    // ── WorldToScreenPoint — chamado DIRETO, 100% preciso ──
    Vector3 bottomWorld = { worldPos.x, worldPos.y, worldPos.z };
    Vector3 topWorld = { worldPos.x, worldPos.y + 2.0f, worldPos.z };

    Vector3 screenBottom = fn_WorldToScreenPoint(camera, bottomWorld);
    Vector3 screenTop = fn_WorldToScreenPoint(camera, topWorld);

    // Obter tamanho da tela
    int screenH = sharedData->screenH;
    if (screenH <= 0) {
        if (fn_Screen_get_height) {
            screenH = fn_Screen_get_height(nullptr);
            sharedData->screenH = screenH;
        }
        if (fn_Screen_get_width) {
            sharedData->screenW = fn_Screen_get_width(nullptr);
        }
    }

    // Verificar se está na frente da câmera
    if (screenBottom.z < 1.0f || screenTop.z < 1.0f) return;

    // Inverter Y (Unity: Y=0 embaixo, Tela: Y=0 em cima)
    screenBottom.y = (float)screenH - screenBottom.y;
    screenTop.y = (float)screenH - screenTop.y;

    // ── Escrever no SharedMemory ──
    int idx = sharedData->playerCount;
    if (idx >= MAX_ESP_PLAYERS) return;

    ESPEntry& entry = sharedData->players[idx];
    entry.topX = screenTop.x;
    entry.topY = screenTop.y;
    entry.topZ = screenTop.z;
    entry.bottomX = screenBottom.x;
    entry.bottomY = screenBottom.y;
    entry.bottomZ = screenBottom.z;
    entry.distance = screenBottom.z; // z ≈ distância
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

    fn_WorldToScreenPoint = (Vector3(*)(void*, Vector3)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_CAMERA),
        OBFUSCATE(CLS_CAMERA), OBFUSCATE("WorldToScreenPoint"), 1);

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
    if (!fn_Camera_get_main || !fn_WorldToScreenPoint ||
        !fn_get_transform || !fn_get_position) {
        LOGE("Falha ao resolver funções: cam=%p wts=%p trans=%p pos=%p",
             fn_Camera_get_main, fn_WorldToScreenPoint, fn_get_transform, fn_get_position);
        return nullptr;
    }
    LOGI("Funções resolvidas com sucesso");

    // ── Criar shared memory ──
    shmFd = shm_create_file();
    if (shmFd < 0) {
        LOGE("Falha ao criar shared memory");
        return nullptr;
    }

    sharedData = shm_map(shmFd);
    if (!sharedData) {
        close(shmFd);
        LOGE("Falha ao mapear shared memory");
        return nullptr;
    }

    memset(sharedData, 0, sizeof(SharedESPData));
    LOGI("Shared memory criado em /data/local/tmp/.esp_shm");

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
