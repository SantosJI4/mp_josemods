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
#include <cstdarg>
#include <ctime>
#include <android/log.h>

#include "Utils.h"
#include "Vector3.h"
#include "obfuscate.h"
#include "SharedData.h"

// ByNameModding
#include "ByNameModding/fake_dlfcn.h"
#include "ByNameModding/Il2Cpp.h"

#define HOOK_TAG "GameHook"
#define HOOK_BUILD_VER "v15-ptrace"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, HOOK_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, HOOK_TAG, __VA_ARGS__)

// ============================================================
// Hook Log File — escreve diagnostico que o overlay pode ler
// Usa diretorio de dados do jogo como primario (sempre acessivel)
// ============================================================
#define HOOK_LOG_PATH_1 "/data/local/tmp/.hook_log"
#define HOOK_LOG_PATH_2 "/sdcard/.hook_log"

static void hookLogWrite(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // PRIMARIO: diretorio do jogo (sempre acessivel pelo processo)
    const char* gamePath = getGameLogPath();
    if (gamePath && gamePath[0]) {
        int fd = open(gamePath, O_CREAT | O_WRONLY | O_APPEND, 0666);
        if (fd >= 0) {
            write(fd, buf, strlen(buf));
            write(fd, "\n", 1);
            close(fd);
        }
    }

    // Fallback: tentar paths classicos
    const char* paths[] = { HOOK_LOG_PATH_1, HOOK_LOG_PATH_2 };
    for (int i = 0; i < 2; i++) {
        int fd = open(paths[i], O_CREAT | O_WRONLY | O_APPEND, 0666);
        if (fd >= 0) {
            write(fd, buf, strlen(buf));
            write(fd, "\n", 1);
            close(fd);
        }
    }

    // Logcat tambem
    __android_log_print(ANDROID_LOG_INFO, HOOK_TAG, "%s", buf);
}

// ============================================================
// CONFIGURAÇÃO DE NOMES (do dump.cs)
// Se os nomes estão obfuscados no seu jogo, troque aqui
// ============================================================
#define ASSEMBLY_CS   "Assembly-CSharp.dll"
#define ASSEMBLY_UE   "UnityEngine.CoreModule.dll"

#define NS_PLAYER     "COW.GamePlay"        // namespace do Player
#define CLS_PLAYER    "Player"              // nome da classe
#define MTD_ONUPDATE  "LateUpdate"          // método hookado

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
// Il2Cpp ABI: TODOS os metodos recebem MethodInfo* como ultimo param
// Estaticos: fn(MethodInfo*), Instancia: fn(self, MethodInfo*)
// Se a assinatura nao bater, ARM64 passa lixo nos registros → SIGSEGV
// ============================================================
static void*   (*fn_Camera_get_main)(void* method) = nullptr;
static void*   (*fn_get_transform)(void* component, void* method) = nullptr;
static Vector3 (*fn_get_position)(void* transform, void* method) = nullptr;
static int     (*fn_Screen_get_width)(void* method) = nullptr;
static int     (*fn_Screen_get_height)(void* method) = nullptr;

// Para VP Matrix (Camera)
struct Matrix4x4 { float m[16]; };
// ARM64 ABI: structs > 16 bytes (Matrix4x4 = 64 bytes) são retornadas via
// hidden register x8. Declarar como RETURN TYPE faz o compilador setar x8.
static Matrix4x4 (*fn_get_worldToCameraMatrix)(void* camera, void* method) = nullptr;
static Matrix4x4 (*fn_get_projectionMatrix)(void* camera, void* method) = nullptr;

// Fallback: WorldToScreenPoint(self, Vector3 pos, int eye, MethodInfo*)
static Vector3 (*fn_WorldToScreenPoint)(void* camera, Vector3 pos, int eye, void* method) = nullptr;
static bool useManualW2S = true; // true = VP matrix, false = fallback direto

// il2cpp_class_get_method_from_name — resolvido direto do libil2cpp.so
static void* (*resolve_method)(void* klass, const char* name, int argsCount) = nullptr;

// Original OnUpdate — il2cpp ABI: TODOS os metodos recebem (self, methodInfo)
static void (*orig_OnUpdate)(void* self, void* methodInfo) = nullptr;

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

static void Hook_OnUpdate(void* self, void* methodInfo) {
    // Chamar original primeiro (mantém o jogo funcionando)
    if (orig_OnUpdate) {
        orig_OnUpdate(self, methodInfo);
    }

    // Se shared memory não está pronto ou hook desativado, retorna
    if (!sharedData || !self || !hookActive.load()) return;

    // Sempre manter magic ativo para overlay detectar
    sharedData->magic = 0xDEADF00D;

    // Overlay controla se o hook processa ESP
    if (!sharedData->espEnabled) return;

    // ── Detectar início de novo frame por tempo ──
    // Unity chama Update() para todos os objetos em sequência dentro do mesmo frame.
    // Se passaram >2ms desde a última chamada, é um novo frame.
    // Isso é independente do overlay — VP matrix SEMPRE atualiza a cada frame.
    static long long g_lastCallMs = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long nowMs = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    bool newFrame = (nowMs - g_lastCallMs) > 2;
    g_lastCallMs = nowMs;

    if (newFrame) {
        // Reset player count no HOOK, não depende do overlay
        sharedData->playerCount = 0;
        g_frameId++;
        sharedData->debugLastCall = 10;

        // Camera.get_main — UMA VEZ por frame
        g_cachedCamera = fn_Camera_get_main ? fn_Camera_get_main(nullptr) : nullptr;
        sharedData->debugLastCall = 11;

        // VP Matrix — SEMPRE atualiza (captura FOV atual: scope vs hip-fire)
        g_vpValid = false;
        if (useManualW2S && g_cachedCamera && fn_get_worldToCameraMatrix && fn_get_projectionMatrix) {
            Matrix4x4 viewMat = fn_get_worldToCameraMatrix(g_cachedCamera, nullptr);
            sharedData->debugLastCall = 12;
            Matrix4x4 projMat = fn_get_projectionMatrix(g_cachedCamera, nullptr);
            sharedData->debugLastCall = 13;
            MultiplyMatrix(projMat, viewMat, g_vpMatrix);
            g_vpValid = true;
        }

        // Tela (atualiza sempre — pode mudar com rotação)
        if (fn_Screen_get_width && fn_Screen_get_height) {
            sharedData->screenW = fn_Screen_get_width(nullptr);
            sharedData->screenH = fn_Screen_get_height(nullptr);
        }
    }

    int idx = sharedData->playerCount;
    if (idx >= MAX_ESP_PLAYERS || idx >= 20) return;

    void* camera = g_cachedCamera;
    if (!camera) return;

    // Precisamos de VP matrix OU WorldToScreenPoint fallback
    if (!g_vpValid && !fn_WorldToScreenPoint) return;

    int screenH = sharedData->screenH;
    int screenW = sharedData->screenW;
    if (screenW <= 0 || screenH <= 0) return;

    // ── Pegar transform do player ──
    sharedData->debugLastCall = 20 + idx;
    void* transform = fn_get_transform ? fn_get_transform(self, nullptr) : nullptr;
    if (!transform) return;

    // ── Pegar posição 3D do player ──
    sharedData->debugLastCall = 40 + idx;
    Vector3 worldPos = fn_get_position(transform, nullptr);

    // Sanity check
    if (std::isnan(worldPos.x) || std::isnan(worldPos.y) || std::isnan(worldPos.z)) return;

    // Unity humanoid pivot pode ser nos pes OU no centro do capsule.
    // Sniper3D PersonTarget: pivot nos pes (ground level).
    // Pés = y, Cabeça = y + ~1.75m (humano medio)
    // Adicionamos margem embaixo (-0.05) e em cima (+0.05) para cobrir modelo
    Vector3 bottomWorld(worldPos.x, worldPos.y - 0.05f, worldPos.z);
    Vector3 topWorld(worldPos.x, worldPos.y + 1.75f, worldPos.z);

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
        screenBottom = fn_WorldToScreenPoint(camera, bottomWorld, 2, nullptr);
        screenTop = fn_WorldToScreenPoint(camera, topWorld, 2, nullptr);
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
    hookLogWrite("Thread iniciada, aguardando libil2cpp.so...");
    LOGI("Hook thread iniciada, aguardando libil2cpp.so...");

    // ── Aguardar il2cpp carregar ──
    while (!isLibraryLoaded("libil2cpp.so")) {
        sleep(1);
    }
    LOGI("libil2cpp.so detectada");
    hookLogWrite("libil2cpp.so detectada");

    // ── Resolver APIs do il2cpp via ByNameModding ──
    Il2CppAttach("libil2cpp.so");
    sleep(3); // Esperar assemblies carregarem

    hookLogWrite("Game data dir: %s", getGameDataDir());

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
    fn_Camera_get_main = (void*(*)(void*)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_CAMERA),
        OBFUSCATE(CLS_CAMERA), OBFUSCATE("get_main"), 0);

    // VP Matrix: ARM64 ABI — Matrix4x4 retornada via x8 (return type, nao param)
    fn_get_worldToCameraMatrix = (Matrix4x4(*)(void*, void*)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_CAMERA),
        OBFUSCATE(CLS_CAMERA), OBFUSCATE("get_worldToCameraMatrix"), 0);

    fn_get_projectionMatrix = (Matrix4x4(*)(void*, void*)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_CAMERA),
        OBFUSCATE(CLS_CAMERA), OBFUSCATE("get_projectionMatrix"), 0);

    // Fallback: WorldToScreenPoint(Vector3 pos, MonoOrStereoscopicEye eye) — 2 args
    fn_WorldToScreenPoint = (Vector3(*)(void*, Vector3, int, void*)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_CAMERA),
        OBFUSCATE(CLS_CAMERA), OBFUSCATE("WorldToScreenPoint"), 2);
    // Tentar versão com 1 arg se 2 args falhou
    if (!fn_WorldToScreenPoint) {
        fn_WorldToScreenPoint = (Vector3(*)(void*, Vector3, int, void*)) Il2CppGetMethodOffset(
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

    fn_get_transform = (void*(*)(void*, void*)) Il2CppGetMethodOffset(
        OBFUSCATE(ASSEMBLY_UE), OBFUSCATE(NS_COMPONENT),
        OBFUSCATE(CLS_COMPONENT), OBFUSCATE("get_transform"), 0);

    fn_get_position = (Vector3(*)(void*, void*)) Il2CppGetMethodOffset(
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
    // IMPORTANTE: usar /data/local/tmp/ como PRIMARIO
    // O overlay tambem tenta /data/local/tmp/ PRIMEIRO
    // O game dir falha quando /proc/self/cmdline retorna zygote
    hookLogWrite("Tentando shm [%s]... uid=%d gid=%d", HOOK_BUILD_VER, getuid(), getgid());
    hookLogWrite("Paths: %s, /data/data/%s/%s, %s", SHM_PATH_1, HOOK_GAME_PACKAGE, SHM_FILENAME, SHM_PATH_2);

    shmFd = shm_create_file();
    if (shmFd < 0) {
        LOGE("Falha ao criar shared memory: errno=%d (%s)", errno, strerror(errno));
        hookLogWrite("FALHA shm_create_file: errno=%d (%s) uid=%d", errno, strerror(errno), getuid());
        return nullptr;
    }
    hookLogWrite("shm_create_file OK: fd=%d path=%s", shmFd, shmActivePath ? shmActivePath : "???");

    sharedData = shm_map(shmFd);
    if (!sharedData) {
        close(shmFd);
        LOGE("Falha ao mapear shared memory: errno=%d (%s)", errno, strerror(errno));
        hookLogWrite("FALHA shm_map: errno=%d (%s)", errno, strerror(errno));
        return nullptr;
    }

    memset(sharedData, 0, sizeof(SharedESPData));
    sharedData->magic = 0xDEADF00D;  // Overlay espera isso para conectar
    LOGI("Shared memory criado em %s (magic=0xDEADF00D)", shmActivePath ? shmActivePath : "???");
    hookLogWrite("SHM OK: path=%s magic=0xDEADF00D mmap=%p", shmActivePath ? shmActivePath : "???", sharedData);

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
    hookLogWrite("=== HOOK ATIVO === VMT em %s::%s", CLS_PLAYER, MTD_ONUPDATE);

    return nullptr;
}

// ============================================================
// ENTRY POINT: CONSTRUCTOR (ptrace + dlopen)
// Quando o injector faz dlopen() no processo do jogo,
// esta funcao roda automaticamente via __attribute__((constructor))
// ============================================================

static bool g_hookStarted = false;

__attribute__((constructor))
void lib_main() {
    LOGI("lib_main() LOADED [%s] pid=%d uid=%d", HOOK_BUILD_VER, getpid(), getuid());

    // Verificar se estamos no processo do jogo
    char cmdline[256] = {0};
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (f) { fread(cmdline, 1, 255, f); fclose(f); }

    LOGI("lib_main() cmdline=[%s]", cmdline);

    if (strstr(cmdline, "freefireth") == nullptr &&
        strstr(cmdline, "freefire") == nullptr &&
        strstr(cmdline, "sniper3d") == nullptr) {
        LOGI("lib_main() skipping (not game process)");
        return;
    }

    if (g_hookStarted) return;
    g_hookStarted = true;

    LOGI("libHook.so loaded (ptrace) [%s] pid=%d", HOOK_BUILD_VER, getpid());
    hookLogWrite("=== HOOK CARREGADO (ptrace) [%s] === pid=%d uid=%d", HOOK_BUILD_VER, getpid(), getuid());
    pthread_t t;
    pthread_create(&t, nullptr, hack_thread, nullptr);
    pthread_detach(t);
}

// ============================================================
// Agent_OnAttach — Fallback (mantido por compatibilidade)
// ============================================================
extern "C" JNIEXPORT jint JNICALL
Agent_OnAttach(JavaVM* vm, char* options, void* reserved) {
    if (g_hookStarted) return 0;
    g_hookStarted = true;

    LOGI("Agent_OnAttach [%s] pid=%d", HOOK_BUILD_VER, getpid());
    hookLogWrite("=== HOOK CARREGADO (Agent) [%s] === pid=%d uid=%d",
                 HOOK_BUILD_VER, getpid(), getuid());
    pthread_t t;
    pthread_create(&t, nullptr, hack_thread, nullptr);
    pthread_detach(t);
    return 0;
}
