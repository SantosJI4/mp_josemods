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
 *   2. base + offset → resolve funções direto (zero il2cpp API)
 *   3. dlsym real → resolve Player::LateUpdate MethodInfo
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
#include <fcntl.h>
#include <elf.h>
#include <sys/prctl.h>
#include <android/log.h>

#include "Utils.h"
#include "Vector3.h"
#include "obfuscate.h"
#include "SharedData.h"

// ============================================================
// STEALTH CONFIG — desativar logging e traces em release
// Comentar a linha abaixo para build de RELEASE (sem log)
// ============================================================
//#define STEALTH_DEBUG  // ← DESCOMENTADO = modo debug com logs

#ifdef STEALTH_DEBUG
  #define HOOK_TAG "GameHook"
  #define LOGI(...) __android_log_print(ANDROID_LOG_INFO, HOOK_TAG, __VA_ARGS__)
  #define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, HOOK_TAG, __VA_ARGS__)
#else
  #define LOGI(...) ((void)0)
  #define LOGE(...) ((void)0)
#endif

#define HOOK_BUILD_VER "v17-stealth"

// ============================================================
// Hook Log File — SOMENTE em modo debug
// ============================================================
#ifdef STEALTH_DEBUG
#define HOOK_LOG_PATH_1 "/data/local/tmp/.gl_log"
#define HOOK_LOG_PATH_2 "/sdcard/.gl_log"

static void hookLogWrite(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    const char* gamePath = getGameLogPath();
    if (gamePath && gamePath[0]) {
        int fd = open(gamePath, O_CREAT | O_WRONLY | O_APPEND, 0666);
        if (fd >= 0) {
            write(fd, buf, strlen(buf));
            write(fd, "\n", 1);
            close(fd);
        }
    }

    const char* paths[] = { HOOK_LOG_PATH_1, HOOK_LOG_PATH_2 };
    for (int i = 0; i < 2; i++) {
        int fd = open(paths[i], O_CREAT | O_WRONLY | O_APPEND, 0666);
        if (fd >= 0) {
            write(fd, buf, strlen(buf));
            write(fd, "\n", 1);
            close(fd);
        }
    }
    __android_log_print(ANDROID_LOG_INFO, "GameHook", "%s", buf);
}
#else
static void hookLogWrite(const char*, ...) { /* NO-OP em release */ }
#endif

// ============================================================
// STEALTH: Esconder libHook.so do /proc/self/maps
//
// Técnica: Para cada segmento file-backed da nossa .so em maps,
// cria um mmap anônimo MAP_FIXED no mesmo endereço, copia o
// conteúdo, e restaura as permissões. O kernel substitui o VMA
// file-backed por anônimo → some do maps.
// ============================================================
static void hideFromMaps() {
    char line[1024];
    struct { uintptr_t start; uintptr_t end; int prot; } segs[8];
    int nsegs = 0;

    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return;

    while (fgets(line, sizeof(line), maps) && nsegs < 8) {
        // Procurar por libgl2.so (nosso .so injetado)
        if (!strstr(line, "libgl2.so")) continue;

        uintptr_t start, end;
        char perms[5] = {0};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) continue;

        // CRITICO: NÃO remap segmentos r-xp (executáveis).
        // Estamos rodando DE DENTRO desse segmento — mmap(MAP_FIXED) sobre
        // ele zera as páginas enquanto a CPU executa aqui → freeze/crash.
        // Apenas r-- e rw- são seguros de remap.
        if (perms[2] == 'x') continue;

        int prot = 0;
        if (perms[0] == 'r') prot |= PROT_READ;
        if (perms[1] == 'w') prot |= PROT_WRITE;

        segs[nsegs].start = start;
        segs[nsegs].end = end;
        segs[nsegs].prot = prot;
        nsegs++;
    }
    fclose(maps);

    for (int i = 0; i < nsegs; i++) {
        size_t size = segs[i].end - segs[i].start;
        if (size == 0) continue;

        // 1. Copiar conteúdo do segmento
        void *backup = malloc(size);
        if (!backup) continue;
        memcpy(backup, (void*)segs[i].start, size);

        // 2. Sobrescrever com mapeamento anônimo (MAP_FIXED)
        // Isso substitui o VMA file-backed por anônimo
        void *anon = mmap((void*)segs[i].start, size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                          -1, 0);
        if (anon == MAP_FAILED) {
            free(backup);
            continue;
        }

        // 3. Copiar conteúdo de volta
        memcpy(anon, backup, size);
        free(backup);

        // 4. Restaurar permissões originais
        mprotect(anon, size, segs[i].prot);
    }

    LOGI("hideFromMaps: %d segmentos remapeados como anonimos", nsegs);
}

// ============================================================
// ELF Symbol Resolver — resolve symbols direto da MEMÓRIA
// Usa Program Headers (PT_DYNAMIC → DT_SYMTAB/DT_STRTAB)
// Funciona em .so STRIPPED (sem section headers)
// ============================================================

#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif

static uintptr_t resolveElfSymbol(uintptr_t loadBase, const char *symName) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)loadBase;

    // Validar ELF
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        LOGE("resolveElfSymbol: invalid ELF at 0x%lx", (unsigned long)loadBase);
        return 0;
    }

    Elf64_Phdr *phdrs = (Elf64_Phdr *)(loadBase + ehdr->e_phoff);

    // Calcular bias (loadBase - primeiro PT_LOAD vaddr)
    uintptr_t bias = loadBase;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            bias = loadBase - phdrs[i].p_vaddr;
            break;
        }
    }

    // Achar PT_DYNAMIC
    Elf64_Dyn *dyn = nullptr;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn = (Elf64_Dyn *)(bias + phdrs[i].p_vaddr);
            break;
        }
    }
    if (!dyn) {
        LOGE("resolveElfSymbol: PT_DYNAMIC nao encontrado");
        return 0;
    }

    // Extrair DT_SYMTAB, DT_STRTAB, DT_HASH, DT_GNU_HASH
    Elf64_Sym *symtab = nullptr;
    const char *strtab = nullptr;
    uint32_t *hashTab = nullptr;
    uint32_t *gnuHash = nullptr;

    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:
                symtab = (Elf64_Sym *)(bias + d->d_un.d_ptr);
                break;
            case DT_STRTAB:
                strtab = (const char *)(bias + d->d_un.d_ptr);
                break;
            case DT_HASH:
                hashTab = (uint32_t *)(bias + d->d_un.d_ptr);
                break;
            case DT_GNU_HASH:
                gnuHash = (uint32_t *)(bias + d->d_un.d_ptr);
                break;
        }
    }

    if (!symtab || !strtab) {
        LOGE("resolveElfSymbol: symtab=%p strtab=%p", (void*)symtab, (void*)strtab);
        return 0;
    }

    // Obter contagem de símbolos via DT_HASH ou DT_GNU_HASH
    uint32_t nSyms = 0;
    if (hashTab) {
        // SysV hash: { nbucket, nchain } — nchain = total symbols
        nSyms = hashTab[1];
    } else if (gnuHash) {
        // GNU hash: precisa calcular o índice máximo
        uint32_t nbuckets = gnuHash[0];
        uint32_t symoffset = gnuHash[1];
        uint32_t bloom_size = gnuHash[2];
        // bloom array: bloom_size * 8 bytes (64-bit ELF)
        uint32_t *buckets = (uint32_t *)((uint8_t *)gnuHash + 16 + bloom_size * 8);
        uint32_t *chain = buckets + nbuckets;

        uint32_t lastSym = 0;
        for (uint32_t i = 0; i < nbuckets; i++) {
            if (buckets[i] > lastSym) lastSym = buckets[i];
        }
        if (lastSym >= symoffset) {
            uint32_t ci = lastSym - symoffset;
            while (!(chain[ci] & 1)) { lastSym++; ci++; }
        }
        nSyms = lastSym + 1;
    }

    if (nSyms == 0) {
        LOGE("resolveElfSymbol: sem DT_HASH/DT_GNU_HASH, nSyms=0");
        return 0;
    }

    // Buscar símbolo
    for (uint32_t i = 0; i < nSyms; i++) {
        if (symtab[i].st_name && symtab[i].st_value) {
            if (strcmp(strtab + symtab[i].st_name, symName) == 0) {
                return bias + symtab[i].st_value;
            }
        }
    }

    LOGE("resolveElfSymbol: '%s' nao encontrado (%u symbols)", symName, nSyms);
    return 0;
}

// ============================================================
// OFFSETS DO DUMP (Free Fire v12 — il2cppdumper RVA)
// Atualizar quando o jogo atualizar
// ============================================================
#define OFF_LateUpdate              0x67BE774
#define OFF_Camera_get_main         0x9C0764C
#define OFF_WorldToScreenPoint      0x9C072D0
#define OFF_get_worldToCameraMatrix 0x9C06D7C
#define OFF_get_projectionMatrix    0x9C06E2C
#define OFF_get_transform           0x9C4DF54
#define OFF_get_position            0x9C5C0F4
#define OFF_Screen_get_width        0x9C14D60
#define OFF_Screen_get_height       0x9C14D88

// ============================================================
// Function Pointers — resolvidos via base + offset direto
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
static Matrix4x4 (*fn_get_worldToCameraMatrix)(void* camera, void* method) = nullptr;
static Matrix4x4 (*fn_get_projectionMatrix)(void* camera, void* method) = nullptr;

// WorldToScreenPoint(self, Vector3 pos, MethodInfo*) — 1 arg version
// O dump mostra: WorldToScreenPoint(Vector3 position) → 1 arg
static Vector3 (*fn_WorldToScreenPoint)(void* camera, Vector3 pos, void* method) = nullptr;
static bool useManualW2S = true;

// Original LateUpdate
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
        sharedData->debugLastCall = 80 + idx;
        screenBottom = fn_WorldToScreenPoint(camera, bottomWorld, nullptr);
        screenTop = fn_WorldToScreenPoint(camera, topWorld, nullptr);
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

void* hack_thread(void*) {
    // Esconder nome da thread (anti-cheat enumera threads)
    prctl(PR_SET_NAME, "Binder:default", 0, 0, 0);

    hookLogWrite("Thread iniciada, aguardando libil2cpp.so...");
    LOGI("Hook thread iniciada, aguardando libil2cpp.so...");

    // ── Aguardar il2cpp carregar ──
    while (!isLibraryLoaded("libil2cpp.so")) {
        sleep(1);
    }
    LOGI("libil2cpp.so detectada");
    hookLogWrite("libil2cpp.so detectada");

    // ── Pegar base address do libil2cpp.so ──
    // findLibrary retorna o PRIMEIRO mapeamento que contém o nome.
    // No Android ARM64, o primeiro mapeamento é r--p offset=0 = load base.
    // Os offsets do il2cppdumper (RVA) são relativos a esse endereço.
    uintptr_t il2cpp_base = findLibrary("libil2cpp.so");
    if (!il2cpp_base) {
        LOGE("findLibrary(libil2cpp.so) retornou 0!");
        hookLogWrite("ERRO: il2cpp base = 0");
        return nullptr;
    }
    LOGI("libil2cpp.so base = 0x%lx", (unsigned long)il2cpp_base);
    hookLogWrite("il2cpp base = 0x%lx", (unsigned long)il2cpp_base);

    // Validar que a base é realmente o inicio do ELF (magic: 7f 45 4c 46)
    {
        unsigned char elfMagic[4] = {0};
        memcpy(elfMagic, (void*)il2cpp_base, 4);
        if (elfMagic[0] != 0x7f || elfMagic[1] != 'E' || elfMagic[2] != 'L' || elfMagic[3] != 'F') {
            LOGE("ERRO: base 0x%lx NAO é ELF! magic=%02x%02x%02x%02x",
                 (unsigned long)il2cpp_base, elfMagic[0], elfMagic[1], elfMagic[2], elfMagic[3]);
            hookLogWrite("ERRO: base nao e ELF! magic=%02x%02x%02x%02x",
                         elfMagic[0], elfMagic[1], elfMagic[2], elfMagic[3]);
            return nullptr;
        }
        LOGI("ELF magic validado: OK (7f454c46)");
    }

    // ── Resolver TODAS as funções via base + offset direto ──
    // ZERO dependência de il2cpp_domain_get, fake_dlfcn, ByNameModding
    #define RESOLVE_OFFSET(type, offset) (type)(il2cpp_base + offset)

    fn_Camera_get_main         = RESOLVE_OFFSET(void*(*)(void*),                   OFF_Camera_get_main);
    fn_get_worldToCameraMatrix = RESOLVE_OFFSET(Matrix4x4(*)(void*, void*),        OFF_get_worldToCameraMatrix);
    fn_get_projectionMatrix    = RESOLVE_OFFSET(Matrix4x4(*)(void*, void*),        OFF_get_projectionMatrix);
    fn_WorldToScreenPoint      = RESOLVE_OFFSET(Vector3(*)(void*, Vector3, void*), OFF_WorldToScreenPoint);
    fn_get_transform           = RESOLVE_OFFSET(void*(*)(void*, void*),            OFF_get_transform);
    fn_get_position            = RESOLVE_OFFSET(Vector3(*)(void*, void*),          OFF_get_position);
    fn_Screen_get_width        = RESOLVE_OFFSET(int(*)(void*),                     OFF_Screen_get_width);
    fn_Screen_get_height       = RESOLVE_OFFSET(int(*)(void*),                     OFF_Screen_get_height);

    LOGI("Offsets resolvidos:");
    LOGI("  get_main          = %p (base+0x%X)", fn_Camera_get_main, OFF_Camera_get_main);
    LOGI("  worldToCam        = %p (base+0x%X)", fn_get_worldToCameraMatrix, OFF_get_worldToCameraMatrix);
    LOGI("  projMatrix        = %p (base+0x%X)", fn_get_projectionMatrix, OFF_get_projectionMatrix);
    LOGI("  WorldToScreenPoint= %p (base+0x%X)", fn_WorldToScreenPoint, OFF_WorldToScreenPoint);
    LOGI("  get_transform     = %p (base+0x%X)", fn_get_transform, OFF_get_transform);
    LOGI("  get_position      = %p (base+0x%X)", fn_get_position, OFF_get_position);
    LOGI("  Screen_get_width  = %p (base+0x%X)", fn_Screen_get_width, OFF_Screen_get_width);
    LOGI("  Screen_get_height = %p (base+0x%X)", fn_Screen_get_height, OFF_Screen_get_height);
    hookLogWrite("Offsets: cam=%p trans=%p pos=%p", fn_Camera_get_main, fn_get_transform, fn_get_position);

    #undef RESOLVE_OFFSET

    // VP Matrix disponivel
    useManualW2S = true;
    LOGI("Usando VP Matrix manual para W2S");

    // Verificar se tudo resolveu
    if (!fn_Camera_get_main || !fn_get_transform || !fn_get_position) {
        LOGE("Falha nos offsets: cam=%p trans=%p pos=%p",
             fn_Camera_get_main, fn_get_transform, fn_get_position);
        hookLogWrite("ERRO: offsets invalidos");
        return nullptr;
    }

    hookLogWrite("Game data dir: %s", getGameDataDir());

    // ── Criar shared memory ──
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
    sharedData->magic = 0xDEADF00D;
    LOGI("Shared memory criado em %s (magic=0xDEADF00D)", shmActivePath ? shmActivePath : "???");
    hookLogWrite("SHM OK: path=%s magic=0xDEADF00D mmap=%p", shmActivePath ? shmActivePath : "???", sharedData);

    // NOTA: Screen::get_width/height chamado DEPOIS do domain pronto
    // (il2cpp runtime precisa estar inicializado antes de chamar metodos)

    // ── VMT Hook no LateUpdate ──
    // Precisamos do MethodInfo* do LateUpdate pra trocar o methodPointer.
    // dlopen/dlsym NAO funciona (namespace do linker Android 7+).
    // Solucao: resolver simbolos il2cpp lendo o ELF do disco direto.
    LOGI("[1/5] Resolvendo il2cpp API via ELF (memoria)...");
    hookLogWrite("[1/5] Resolvendo il2cpp API via ELF (memoria)...");

    // Resolver as funcoes il2cpp necessarias direto da memoria
    // (PT_DYNAMIC → DT_SYMTAB/DT_STRTAB — funciona em .so stripped)
    auto p_domain_get = (void*(*)())
        resolveElfSymbol(il2cpp_base, "il2cpp_domain_get");
    auto p_domain_get_assemblies = (void**(*)(const void*, size_t*))
        resolveElfSymbol(il2cpp_base, "il2cpp_domain_get_assemblies");
    auto p_assembly_get_image = (const void*(*)(const void*))
        resolveElfSymbol(il2cpp_base, "il2cpp_assembly_get_image");
    auto p_image_get_name = (const char*(*)(void*))
        resolveElfSymbol(il2cpp_base, "il2cpp_image_get_name");
    auto p_class_from_name = (void*(*)(const void*, const char*, const char*))
        resolveElfSymbol(il2cpp_base, "il2cpp_class_from_name");
    auto p_class_get_method = (void*(*)(void*, const char*, int))
        resolveElfSymbol(il2cpp_base, "il2cpp_class_get_method_from_name");

    LOGI("ELF resolved: domain_get=%p assemblies=%p get_image=%p get_name=%p class_from_name=%p get_method=%p",
         (void*)p_domain_get, (void*)p_domain_get_assemblies,
         (void*)p_assembly_get_image, (void*)p_image_get_name,
         (void*)p_class_from_name, (void*)p_class_get_method);
    hookLogWrite("ELF: domain=%p class=%p method=%p",
         (void*)p_domain_get, (void*)p_class_from_name, (void*)p_class_get_method);

    if (!p_domain_get || !p_domain_get_assemblies || !p_assembly_get_image ||
        !p_image_get_name || !p_class_from_name || !p_class_get_method) {
        LOGE("ELF resolver falhou para uma ou mais funcoes il2cpp");
        hookLogWrite("ERRO: ELF resolver falhou");
        return nullptr;
    }

    // Validar que domain_get aponta pra codigo ARM64 valido
    {
        uint32_t firstInst = *(uint32_t*)((uintptr_t)p_domain_get);
        LOGI("domain_get first instruction: 0x%08x", firstInst);
        hookLogWrite("domain_get inst: 0x%08x", firstInst);
        if (firstInst == 0 || firstInst == 0xffffffff) {
            LOGE("ERRO: domain_get aponta pra codigo invalido!");
            hookLogWrite("ERRO: domain_get codigo invalido");
            return nullptr;
        }
    }

    // Aguardar il2cpp_init() completar.
    // IMPORTANTE: NAO usar sigsetjmp/siglongjmp — quebra o ART (signal chain).
    // Em vez disso, esperar global-metadata.dat aparecer em maps.
    // il2cpp_init() faz mmap do metadata file → quando aparece em maps,
    // o runtime ja esta inicializado o suficiente pra domain_get funcionar.
    LOGI("[2/5] Aguardando il2cpp runtime inicializar...");
    hookLogWrite("[2/5] Aguardando il2cpp init...");

    // Fase 1: Esperar global-metadata.dat em /proc/self/maps
    bool metadataFound = false;
    for (int i = 0; i < 60; i++) {
        FILE *maps = fopen("/proc/self/maps", "r");
        if (maps) {
            char line[1024];
            while (fgets(line, sizeof(line), maps)) {
                if (strstr(line, "global-metadata.dat")) {
                    metadataFound = true;
                    break;
                }
            }
            fclose(maps);
        }
        if (metadataFound) {
            LOGI("  global-metadata.dat encontrado (tentativa %d)", i+1);
            hookLogWrite("metadata encontrado (tentativa %d)", i+1);
            break;
        }
        if (i % 5 == 0) {
            LOGI("  aguardando global-metadata.dat... (%d/60)", i+1);
        }
        sleep(1);
    }
    if (!metadataFound) {
        LOGE("global-metadata.dat timeout 60s");
        hookLogWrite("ERRO: metadata timeout");
        return nullptr;
    }

    // Fase 2: Esperar domain ficar pronto (metadata existe, mas init pode
    // nao ter terminado ainda — domain_get agora eh seguro de chamar)
    sleep(3); // margem de seguranca apos metadata aparecer
    LOGI("  Metadata encontrado + 3s margem. Tentando domain_get...");

    void *domain = nullptr;
    for (int i = 0; i < 30; i++) {
        domain = p_domain_get();
        if (domain) {
            size_t sz = 0;
            void **assemblies = p_domain_get_assemblies(domain, &sz);
            if (assemblies && sz > 0) {
                LOGI("il2cpp domain PRONTO (tentativa %d): domain=%p assemblies=%zu", i+1, domain, sz);
                hookLogWrite("domain PRONTO (tentativa %d) assemblies=%zu", i+1, sz);
                break;
            }
        }
        if (i >= 29) {
            LOGE("il2cpp domain timeout (30s apos metadata)");
            hookLogWrite("ERRO: domain timeout 30s");
            return nullptr;
        }
        LOGI("  domain wait... (%d/30)", i+1);
        sleep(1);
    }

    // Agora il2cpp runtime esta pronto — seguro chamar metodos
    LOGI("[3/5] Screen size...");
    if (fn_Screen_get_width && fn_Screen_get_height) {
        sharedData->screenW = fn_Screen_get_width(nullptr);
        sharedData->screenH = fn_Screen_get_height(nullptr);
        LOGI("Tela: %dx%d", sharedData->screenW, sharedData->screenH);
        hookLogWrite("Tela: %dx%d", sharedData->screenW, sharedData->screenH);
    }

    // Achar Assembly-CSharp image
    LOGI("[3/5] Buscando Assembly-CSharp...");
    hookLogWrite("[3/5] Buscando Assembly-CSharp...");
    size_t asmCount = 0;
    void **assemblies = p_domain_get_assemblies(domain, &asmCount);
    void *cs_image = nullptr;
    for (size_t i = 0; i < asmCount; i++) {
        void *img = (void*)p_assembly_get_image(assemblies[i]);
        const char *name = p_image_get_name(img);
        if (name && strcmp(name, "Assembly-CSharp.dll") == 0) {
            cs_image = img;
            break;
        }
    }
    if (!cs_image) {
        LOGE("Assembly-CSharp.dll nao encontrada!");
        hookLogWrite("ERRO: Assembly-CSharp.dll nao encontrada");
        return nullptr;
    }
    LOGI("Assembly-CSharp.dll = %p", cs_image);

    // Achar classe Player
    LOGI("[4/5] Buscando Player class...");
    hookLogWrite("[4/5] Buscando Player class...");
    void *playerClass = p_class_from_name(cs_image, "COW.GamePlay", "Player");
    if (!playerClass) {
        LOGE("Classe Player nao encontrada");
        hookLogWrite("ERRO: classe Player nao encontrada");
        return nullptr;
    }
    LOGI("playerClass = %p", playerClass);
    hookLogWrite("playerClass = %p", playerClass);

    // Pegar MethodInfo* do LateUpdate
    LOGI("  Chamando class_get_method_from_name(LateUpdate, 0)...");
    hookLogWrite("Buscando LateUpdate MethodInfo...");
    void *onUpdateMethodInfo = p_class_get_method(playerClass, "LateUpdate", 0);
    if (!onUpdateMethodInfo) {
        LOGE("LateUpdate nao encontrado em Player");
        hookLogWrite("ERRO: LateUpdate nao encontrado");
        return nullptr;
    }
    LOGI("LateUpdate MethodInfo = %p", onUpdateMethodInfo);
    hookLogWrite("LateUpdate MethodInfo = %p", onUpdateMethodInfo);

    // Verificar que methodPointer bate com nosso offset
    void *currentMethodPtr = *(void**)onUpdateMethodInfo;
    void *expectedPtr = (void*)(il2cpp_base + OFF_LateUpdate);
    LOGI("  methodPointer atual = %p, esperado (base+off) = %p, match=%s",
         currentMethodPtr, expectedPtr,
         (currentMethodPtr == expectedPtr) ? "SIM" : "NAO");

    // Aplicar VMT Hook
    LOGI("[5/5] Aplicando VMT Hook...");
    hookLogWrite("[5/5] Aplicando VMT Hook...");
    if (!VmtHook(onUpdateMethodInfo, (void*)Hook_OnUpdate, (void**)&orig_OnUpdate)) {
        LOGE("VMT Hook falhou");
        hookLogWrite("ERRO: VMT Hook falhou");
        return nullptr;
    }

    hookActive.store(true);
    LOGI("=== HOOK ATIVO === VMT hook em Player::LateUpdate");
    hookLogWrite("=== HOOK ATIVO === VMT em Player::LateUpdate");

    return nullptr;
}

// ============================================================
// ENTRY POINT: CONSTRUCTOR (ptrace + dlopen)
// Quando o injector faz dlopen() no processo do jogo,
// esta funcao roda automaticamente via __attribute__((constructor))
// ============================================================

bool g_hookStarted = false;

// fd do SHM pre-aberto em preAppSpecialize (root context) pelo modulo Zygisk.
// Usado por shm_create_file() para contornar SELinux no contexto untrusted_app.
#ifdef ZYGISK_BUILD
int g_zygisk_shm_fd = -1;
#endif

// lib_main() é o entry point para injeção via ptrace (libgl2.so).
// Em modo Zygisk, o entry point é postAppSpecialize() em zygisk_main.cpp.
#ifndef ZYGISK_BUILD
__attribute__((constructor))
void lib_main() {
    LOGI("lib_main() LOADED [%s] pid=%d uid=%d", HOOK_BUILD_VER, getpid(), getuid());

    // hideFromMaps() DESATIVADO: causa crash por race condition.
    // mmap(MAP_FIXED) sobre r-- zera rodata durante a janela de restauracao.
    // Codigo acessando string literals nesse instante = leitura de zeros = crash.
    // O rename libgl2.so ja e suficiente contra maps scan.
    // hideFromMaps();

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

    // Limpar arquivos de log antigos (evidência forense)
#ifndef STEALTH_DEBUG
    unlink("/data/local/tmp/.hook_log");
    unlink("/sdcard/.hook_log");
    // Limpar SHM antigo (nome mudou)
    unlink("/data/local/tmp/.esp_shm");
    unlink("/sdcard/.esp_shm");
    const char* gameLog = getGameLogPath();
    if (gameLog && gameLog[0]) unlink(gameLog);
    // Limpar game-dir .esp_shm antigo
    char oldShm[512];
    snprintf(oldShm, sizeof(oldShm), "/data/data/%s/.esp_shm", HOOK_GAME_PACKAGE);
    unlink(oldShm);
    char oldLog2[512];
    snprintf(oldLog2, sizeof(oldLog2), "/data/data/%s/.hook_log", HOOK_GAME_PACKAGE);
    unlink(oldLog2);
#endif

    LOGI("loaded (ptrace) [%s] pid=%d", HOOK_BUILD_VER, getpid());
    hookLogWrite("=== HOOK CARREGADO (ptrace) [%s] === pid=%d uid=%d", HOOK_BUILD_VER, getpid(), getuid());
    pthread_t t;
    pthread_create(&t, nullptr, hack_thread, nullptr);
    pthread_detach(t);
}
#endif // ZYGISK_BUILD

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
