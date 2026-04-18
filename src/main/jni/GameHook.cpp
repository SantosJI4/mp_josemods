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
#define STEALTH_DEBUG  // ← DESCOMENTADO = modo debug com logs

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
// Usa DT_HASH (SysV) ou DT_GNU_HASH com lookup O(1) correto.
// Sem scan linear — não trava em libil2cpp com milhoes de simbolos.
// ============================================================

#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif

// Função de hash ELF padrão (SysV)
static uint32_t elf_sysv_hash(const char *name) {
    uint32_t h = 0, g;
    for (; *name; ++name) {
        h = (h << 4) + (uint8_t)*name;
        if ((g = h & 0xf0000000u)) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

// Função de hash GNU (djb2 modificado)
static uint32_t elf_gnu_hash(const char *name) {
    uint32_t h = 5381;
    for (; *name; ++name) h = h * 33 + (uint8_t)*name;
    return h;
}

static uintptr_t resolveElfSymbol(uintptr_t loadBase, const char *symName) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)loadBase;

    // Validar ELF magic
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        LOGE("resolveElfSymbol: ELF invalido em 0x%lx", (unsigned long)loadBase);
        return 0;
    }

    Elf64_Phdr *phdrs = (Elf64_Phdr *)(loadBase + ehdr->e_phoff);

    // Calcular bias (loadBase - primeiro PT_LOAD vaddr)
    uintptr_t bias = loadBase;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) { bias = loadBase - phdrs[i].p_vaddr; break; }
    }

    // Achar PT_DYNAMIC
    Elf64_Dyn *dyn = nullptr;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) { dyn = (Elf64_Dyn*)(bias + phdrs[i].p_vaddr); break; }
    }
    if (!dyn) { LOGE("resolveElfSymbol: PT_DYNAMIC nao encontrado"); return 0; }

    // Extrair ponteiros da tabela dinâmica
    Elf64_Sym  *symtab  = nullptr;
    const char *strtab  = nullptr;
    uint32_t   *hashTab = nullptr; // DT_HASH  (SysV)
    uint32_t   *gnuHash = nullptr; // DT_GNU_HASH

    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab  = (Elf64_Sym*)(bias + d->d_un.d_ptr);  break;
            case DT_STRTAB:   strtab  = (const char*)(bias + d->d_un.d_ptr); break;
            case DT_HASH:     hashTab = (uint32_t*)(bias + d->d_un.d_ptr);   break;
            case DT_GNU_HASH: gnuHash = (uint32_t*)(bias + d->d_un.d_ptr);   break;
        }
    }

    if (!symtab || !strtab) {
        LOGE("resolveElfSymbol: symtab=%p strtab=%p", (void*)symtab, (void*)strtab);
        return 0;
    }

    // ── Tentativa 1: SysV hash — O(1) lookup via chain ──
    if (hashTab) {
        uint32_t nbuckets = hashTab[0];
        uint32_t nchain   = hashTab[1];
        uint32_t *buckets = hashTab + 2;
        uint32_t *chains  = buckets + nbuckets;
        uint32_t h = elf_sysv_hash(symName) % nbuckets;
        for (uint32_t i = buckets[h]; i != 0 && i < nchain; i = chains[i]) {
            if (symtab[i].st_value &&
                strcmp(strtab + symtab[i].st_name, symName) == 0) {
                LOGI("resolveElfSymbol: '%s' -> 0x%lx (SysV)", symName,
                     (unsigned long)(bias + symtab[i].st_value));
                return bias + symtab[i].st_value;
            }
        }
        LOGE("resolveElfSymbol: '%s' nao encontrado via SysV (nbuckets=%u nchain=%u)",
             symName, nbuckets, nchain);
        // Nao retorna 0 aqui — tenta GNU hash se disponivel
    }

    // ── Tentativa 2: GNU hash — O(1) lookup via bloom + chain ──
    if (gnuHash) {
        uint32_t nbuckets    = gnuHash[0];
        uint32_t symoffset   = gnuHash[1];
        uint32_t bloom_size  = gnuHash[2];
        uint32_t bloom_shift = gnuHash[3];
        // Bloom filter: array de uint64_t para Elf64 (8 bytes por entrada)
        uint64_t *bloom  = (uint64_t*)((uint8_t*)gnuHash + 16);
        uint32_t *bkts   = (uint32_t*)(bloom + bloom_size);
        uint32_t *chains = bkts + nbuckets;

        uint32_t h = elf_gnu_hash(symName);

        // Bloom filter: descarta simbolos definitivamente ausentes
        uint64_t bw   = bloom[(h >> 6) % bloom_size];
        uint32_t bit1 = h & 63;
        uint32_t bit2 = (h >> bloom_shift) & 63;
        if (!((bw >> bit1) & 1u) || !((bw >> bit2) & 1u)) {
            LOGE("resolveElfSymbol: '%s' rejeitado bloom GNU", symName);
            return 0;
        }

        uint32_t symIdx = bkts[h % nbuckets];
        if (symIdx < symoffset) {
            LOGE("resolveElfSymbol: '%s' GNU bucket vazio (symIdx=%u off=%u)",
                 symName, symIdx, symoffset);
            return 0;
        }

        uint32_t *c = chains + (symIdx - symoffset);
        for (uint32_t idx = symIdx; ; idx++, c++) {
            if (((*c ^ h) & ~1u) == 0 &&
                symtab[idx].st_value &&
                strcmp(strtab + symtab[idx].st_name, symName) == 0) {
                LOGI("resolveElfSymbol: '%s' -> 0x%lx (GNU)", symName,
                     (unsigned long)(bias + symtab[idx].st_value));
                return bias + symtab[idx].st_value;
            }
            if (*c & 1u) break; // fim da chain
        }
        LOGE("resolveElfSymbol: '%s' nao encontrado na GNU chain", symName);
        return 0;
    }

    LOGE("resolveElfSymbol: '%s' sem DT_HASH nem DT_GNU_HASH", symName);
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
#define OFF_IsLocalPlayer           0x67558A4
#define OFF_IsLocalTeammate         0x6789BF8
#define OFF_get_CurHP               0x67CDD24
#define OFF_get_MaxHP               0x67CDE1C
#define OFF_IsKnockedDownBleed      0x1150  // offset do campo bool no objeto Player

// Aim Assist — UnityEngine.Transform (mesma classe de get_position)
#define OFF_Transform_get_eulerAngles 0x9C5C2B4
#define OFF_Transform_set_eulerAngles 0x9C5C33C

// Animator.GetBoneTransform(HumanBodyBones) — para pegar o Transform do bone da cabeça
// HumanBodyBones.Head = 10
// Player layout: offset 0x700 = NewPlayerAnimationSystemComponent*
// NewPlayerAnimationSystemComponent base (GCommon.AnimationSystemComponent): offset 0x28 = Animator*
#define OFF_Animator_GetBoneTransform 0x9BEE970
#define PLAYER_ANIM_COMPONENT_OFFSET  0x700  // campo HFKJCLHCBGB na classe Player
#define ANIM_COMPONENT_ANIMATOR_OFFSET 0x28  // campo m_Animator na base GCommon.AnimationSystemComponent
#define HUMAN_BODY_BONE_HEAD           10    // UnityEngine.HumanBodyBones.Head

// PlayerColliderChecker — hitbox real da cabeça (igual o servidor usa para dano)
// Collider.get_bounds() retorna Bounds por valor: { m_Center @ +0x10, m_Extents @ +0x1C }
// List<ColliderInfo> layout IL2CPP: _items (array*) @ +0x10, _size @ +0x18
// array de objetos: elemento[i] em _items[i] (ponteiro), cada objeto: m_collider@0x10, m_hitBoxType@0x18
// HitPart.Head = 0
#define OFF_get_HeadCollider               0x676FEB4
// get_bounds() retorna Bounds (24 bytes) via SRET x8 — ABI incorreta para ponteiro direto.
// get_bounds_Injected(out Bounds& ret) recebe o ponteiro em x1 como parâmetro normal → correto.
#define OFF_Collider_get_bounds_Injected  0x9CB794C

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

// IsLocalPlayer — filtra o proprio jogador do ESP
static bool (*fn_IsLocalPlayer)(void* player, void* method) = nullptr;
// IsLocalTeammate(bool includeLocalPlayer) — true = e aliado (ou o proprio player se bool=true)
// Offset: COW.GamePlay.Player::IsLocalTeammate(bool) = 0x6789BF8
static bool (*fn_IsLocalTeammate)(void* player, bool includeLocal, void* method) = nullptr;
static int  (*fn_get_CurHP)(void* player, void* method) = nullptr;
static int  (*fn_get_MaxHP)(void* player, void* method) = nullptr;

// Aim Assist — UnityEngine.Transform
static Vector3 (*fn_get_eulerAngles)(void* transform, void* method) = nullptr;
static void    (*fn_set_eulerAngles)(void* transform, Vector3 euler, void* method) = nullptr;
// Animator.GetBoneTransform(HumanBodyBones) — retorna Transform do bone da cabeça
static void*   (*fn_GetBoneTransform)(void* animator, int32_t boneId, void* method) = nullptr;
// Collider.get_bounds() — retorna Bounds por valor (passado por ponteiro hidden em x8 na ABI ARM64)
typedef struct { float cx, cy, cz; float ex, ey, ez; } BoundsVal;
static void*   (*fn_get_HeadCollider)(void* player, void* method) = nullptr;
// _Injected: assinatura nativa real — sem SRET, ponteiro de saída em x1
static void    (*fn_Collider_get_bounds_Injected)(void* collider, BoundsVal* outBounds, void* method) = nullptr;

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

// ── Aim Assist — estado por frame ──────────────────────────────────────────
static void*   g_camTransform      = nullptr;      // Transform da câmera (atualizado por frame)
static Vector3 g_aimTargetWorld{0.0f, 0.0f, 0.0f}; // Posição 3D da cabeça do melhor alvo
static float   g_aimBestScreenDist = 1e9f;         // prioridade 0: menor distância ao centro
static int     g_aimBestHp         = 0x7fffffff;   // prioridade 1: menor HP (Lowest HP mode)
static float   g_aimBestDepth      = 1e9f;         // prioridade 2: menor clipW (Nearest Distance)
static bool    g_aimHasTarget      = false;        // alvo válido neste frame
// NOTA: NÃO há estado acumulado de pitch/yaw.
// Lemos os euler angles REAIS da câmera a cada frame e aplicamos apenas um delta.
// Isso garante que o input do jogador nunca seja ignorado.
// ───────────────────────────────────────────────────────────────────────────

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

// ── Silent Aim — estado entre frames ─────────────────────────────────────
// SNAP: alvo travado do frame ANTERIOR — usado no snap PRÉ-orig deste frame.
// CAND: melhor candidato sendo construído no frame ATUAL — vira SNAP no próximo.
//
// Fluxo correto:
//   1. PRÉ-orig: snap câmera → g_silentSnapTarget (frame anterior)
//   2. orig roda → raycast/tiro registra na cabeça → headshot
//   3. PÓS-orig: restore câmera imediatamente → jogador não vê movimento
//   4. newFrame: commit CAND→SNAP, reset CAND para próximo frame
static Vector3 g_silentSnapTarget{0.0f, 0.0f, 0.0f};  // alvo travado (frame anterior)
static bool    g_silentSnapValid  = false;
static Vector3 g_silentCandTarget{0.0f, 0.0f, 0.0f};  // candidato atual (próximo frame)
static float   g_silentCandDist   = 1e9f;
static bool    g_silentCandValid  = false;
// ─────────────────────────────────────────────────────────────────────────

static void Hook_OnUpdate(void* self, void* methodInfo) {
    // ── Silent Aim: Snap PRÉ-orig ─────────────────────────────────────────────
    // Usa g_silentSnapTarget (alvo travado do frame anterior).
    // Snap ANTES de orig → orig processa o tiro/raycast com câmera na cabeça.
    // Restore LOGO APÓS orig → jogador não vê a câmera se mover.
    bool    sawSnap  = false;
    Vector3 sawSaved{0.0f, 0.0f, 0.0f};

    if (sharedData && sharedData->silentAimEnabled &&
        g_silentSnapValid && g_camTransform &&
        fn_get_eulerAngles && fn_set_eulerAngles && fn_get_position) {

        sawSaved = fn_get_eulerAngles(g_camTransform, nullptr);
        Vector3 sCamPos = fn_get_position(g_camTransform, nullptr);
        float sdx = g_silentSnapTarget.x - sCamPos.x;
        float sdy = g_silentSnapTarget.y - sCamPos.y;
        float sdz = g_silentSnapTarget.z - sCamPos.z;
        float shd = sqrtf(sdx * sdx + sdz * sdz);
        if (shd > 0.01f) {
            float sYaw   =  atan2f(sdx, sdz) * (180.0f / (float)M_PI);
            float sPitch = -atan2f(sdy, shd)  * (180.0f / (float)M_PI);
            if (sPitch >  89.0f) sPitch =  89.0f;
            if (sPitch < -89.0f) sPitch = -89.0f;
            float sOut = sPitch < 0.0f ? sPitch + 360.0f : sPitch;
            fn_set_eulerAngles(g_camTransform, Vector3(sOut, sYaw, sawSaved.z), nullptr);
            sawSnap = true;
        }
    }

    // Chamar original (jogo processa lógica, física, raycast do tiro)
    if (orig_OnUpdate) {
        orig_OnUpdate(self, methodInfo);
    }

    // ── Silent Aim: Restore IMEDIATAMENTE após orig ───────────────────────────
    // O tiro já foi registrado (na cabeça). Agora restaura para que o jogador
    // não perceba o snap.
    if (sawSnap && g_camTransform && fn_set_eulerAngles) {
        fn_set_eulerAngles(g_camTransform, sawSaved, nullptr);
    }

    // Se shared memory não está pronto ou hook desativado, retorna
    if (!sharedData || !self || !hookActive.load()) return;

    // Sempre manter magic ativo para overlay detectar
    sharedData->magic = 0xDEADF00D;

    // resetSelf reservado (nao necessario com IsLocalTeammate)

    // espEnabled reservado para futuro; hook sempre processa players

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
        g_frameId++;
        sharedData->debugLastCall = 10;

        // Camera.get_main — UMA VEZ por frame
        g_cachedCamera = fn_Camera_get_main ? fn_Camera_get_main(nullptr) : nullptr;
        sharedData->debugLastCall = 11;

        // CRITICO: só resetar playerCount quando temos câmera válida.
        // Se Camera.get_main retorna null neste frame, mantemos os dados
        // do frame anterior visíveis no overlay em vez de zerar tudo.
        if (g_cachedCamera) {
            sharedData->playerCount = 0;
        } else {
            // sem câmera: retornar cedo, dados do frame anterior permanecem
            return;
        }

        // Resolução interna do jogo — pode ser menor que a surface do overlay
        // (Free Fire reduz res interna em grafico baixo/medio)
        // Atualiza sharedData->screenW/H para o overlay escalar as coordenadas W2S
        if (fn_Screen_get_width && fn_Screen_get_height) {
            int gw = fn_Screen_get_width(nullptr);
            int gh = fn_Screen_get_height(nullptr);
            if (gw > 0 && gh > 0) {
                sharedData->screenW = gw;
                sharedData->screenH = gh;
            }
        }

        // VP Matrix — SEMPRE atualiza (captura FOV atual: scope vs hip-fire)
        g_vpValid = false;
        if (useManualW2S && fn_get_worldToCameraMatrix && fn_get_projectionMatrix) {
            Matrix4x4 viewMat = fn_get_worldToCameraMatrix(g_cachedCamera, nullptr);
            sharedData->debugLastCall = 12;
            Matrix4x4 projMat = fn_get_projectionMatrix(g_cachedCamera, nullptr);
            sharedData->debugLastCall = 13;
            MultiplyMatrix(projMat, viewMat, g_vpMatrix);
            g_vpValid = true;
        }

        // ── Aim Assist: Aplicar correção de ângulo (dados do frame anterior) ──
        // Roda em LateUpdate: o game controller já setou a câmera no Update.
        // Lemos a rotação ATUAL (inclui input do jogador), calculamos um delta
        // pequeno em direção à cabeça do alvo e escrevemos de volta.
        // O jogador mantém controle total — apenas sentimos um "puxão" sutil.
        g_camTransform = fn_get_transform ? fn_get_transform(g_cachedCamera, nullptr) : nullptr;

        if (sharedData->aimAssistEnabled && g_camTransform &&
            g_aimHasTarget && fn_get_eulerAngles && fn_set_eulerAngles) {

            // Trigger check: ativo sempre (triggerKey=0) OU enquanto tecla pressionada
            bool triggerOk = (sharedData->triggerKey == 0) ||
                             (sharedData->triggerHeld == 1);

            if (triggerOk) {
                Vector3 curEuler = fn_get_eulerAngles(g_camTransform, nullptr);
                Vector3 camPos   = fn_get_position(g_camTransform, nullptr);

                // Capa offset: ajusta foco para o topo da malha (Rage mode)
                // Para Legit, aimRageOffsetY pode ser 0 ou pequeno
                float offsetY = sharedData->aimRageOffsetY;

                // Delta 3D entre câmera e alvo + offset vertical
                float dx = g_aimTargetWorld.x - camPos.x;
                float dy = (g_aimTargetWorld.y + offsetY) - camPos.y;
                float dz = g_aimTargetWorld.z - camPos.z;

                // Hipotenusa horizontal (plano XZ)
                float hd = sqrtf(dx * dx + dz * dz);

                if (hd > 0.01f) {
                    // ── Cálculo de ViewAngles via atan2 ────────────────────────
                    // atan2(x, z) lida com todos quadrantes sem divisão por zero.
                    // Yaw   = rotação horizontal (eixo Y Unity)
                    // Pitch = rotação vertical   (eixo X Unity, negado: cima = neg)
                    float targetYaw   =  atan2f(dx, dz) * (180.0f / (float)M_PI);
                    float targetPitch = -atan2f(dy, hd)  * (180.0f / (float)M_PI);
                    if (targetPitch >  89.0f) targetPitch =  89.0f;
                    if (targetPitch < -89.0f) targetPitch = -89.0f;

                    if (sharedData->aimMode == 1) {
                        // ══ RAGE — Snap instantâneo ════════════════════════════
                        // Câmera vai direto ao alvo em 1 frame.
                        // aimRageOffsetY já aplicado no delta dy acima.
                        float outPitch = targetPitch < 0.0f
                            ? targetPitch + 360.0f : targetPitch;
                        fn_set_eulerAngles(g_camTransform,
                            Vector3(outPitch, targetYaw, curEuler.z), nullptr);
                        sharedData->aimAssistHasTarget = 1;

                    } else {
                        // ══ LEGIT — Interpolação suave (lerp) ═══════════════
                        // 1. Normalizar pitch atual de [0,360) para [-180,180)
                        float curPitch = curEuler.x;
                        if (curPitch > 180.0f) curPitch -= 360.0f;
                        float curYaw = curEuler.y;

                        // 2. Delta pelo caminho mais curto
                        float pitchDiff = targetPitch - curPitch;
                        float yawDiff   = targetYaw   - curYaw;
                        while (yawDiff >  180.0f) yawDiff -= 360.0f;
                        while (yawDiff < -180.0f) yawDiff += 360.0f;

                        // 3. Deadzone
                        float dz_ang = sharedData->aimAssistDeadzone;
                        if (dz_ang < 0.0f) dz_ang = 0.0f;
                        bool inDeadzone = (fabsf(pitchDiff) < dz_ang &&
                                           fabsf(yawDiff)   < dz_ang);

                        if (!inDeadzone) {
                            // 4. Lerp suave: fator aimLegitSmooth (0.01–0.5)
                            // Baixo = mais humano/devagar  |  Alto = mais rápido
                            float smooth = sharedData->aimLegitSmooth;
                            if (smooth < 0.01f) smooth = 0.01f;
                            if (smooth > 1.0f)  smooth = 1.0f;

                            float newPitch = curPitch + pitchDiff * smooth;
                            float newYaw   = curYaw   + yawDiff   * smooth;

                            // 5. Sanitizar
                            if (newPitch >  89.0f) newPitch =  89.0f;
                            if (newPitch < -89.0f) newPitch = -89.0f;
                            while (newYaw >= 360.0f) newYaw -= 360.0f;
                            while (newYaw <    0.0f) newYaw += 360.0f;

                            float outPitch = newPitch < 0.0f
                                ? newPitch + 360.0f : newPitch;
                            fn_set_eulerAngles(g_camTransform,
                                Vector3(outPitch, newYaw, curEuler.z), nullptr);
                            sharedData->aimAssistHasTarget = 1;
                        } else {
                            sharedData->aimAssistHasTarget = 1; // dentro da deadzone = travado
                        }
                    }
                }
            } // triggerOk
        } else {
            sharedData->aimAssistHasTarget = 0;
        }

        // Resetar candidatos de aim para o novo frame
        g_aimHasTarget      = false;
        g_aimBestScreenDist = 1e9f;
        g_aimBestHp         = 0x7fffffff;
        g_aimBestDepth      = 1e9f;
        // Silent Aim: commit CAND → SNAP (alvo do frame atual vira alvo do próximo)
        g_silentSnapTarget = g_silentCandTarget;
        g_silentSnapValid  = g_silentCandValid;
        g_silentCandValid  = false;
        g_silentCandDist   = 1e9f;
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

    // ── Filtrar eu mesmo + aliados ──
    // IsLocalTeammate(true) = inclui o proprio jogador + todos os aliados
    // Uma unica chamada, sem cache, sem race condition
    if (fn_IsLocalTeammate && fn_IsLocalTeammate(self, true, nullptr)) return;
    // Fallback: se IsLocalTeammate nao estiver disponivel, pelo menos filtra self
    if (!fn_IsLocalTeammate && fn_IsLocalPlayer && fn_IsLocalPlayer(self, nullptr)) return;

    // ── Pegar transform do player ──
    sharedData->debugLastCall = 20 + idx;
    void* transform = fn_get_transform ? fn_get_transform(self, nullptr) : nullptr;
    if (!transform) return;

    // ── Pegar posição 3D do player ──
    sharedData->debugLastCall = 40 + idx;
    Vector3 worldPos = fn_get_position(transform, nullptr);

    // Sanity check
    if (std::isnan(worldPos.x) || std::isnan(worldPos.y) || std::isnan(worldPos.z)) return;

    // ── Posição real da cabeça: 3 camadas de fallback ────────────────────────
    // Prioridade 1: PlayerColliderChecker — hitbox exata que o servidor usa para headshot
    // Prioridade 2: Animator.GetBoneTransform(Head) — bone visual
    // Prioridade 3: worldPos.y + 1.75 — estimativa fixa
    Vector3 bottomWorld(worldPos.x, worldPos.y - 0.05f, worldPos.z);
    Vector3 topWorld;
    bool gotHead = false;

    // ── Posição da cabeça: 3 camadas de precisão ─────────────────────────────
    // P1: get_HeadCollider() → bounds.center  (hitbox exata registrada pelo servidor)
    // P2: Animator.GetBoneTransform(Head=10) → position (bone visual, muito preciso)
    // P3: worldPos.y + 1.75 (estimativa fixa — fallback de segurança)

    // ── P1: Head collider — hitbox real de colisão/dano ─────────────────────
    // Usa get_bounds_Injected (out Bounds& ret) — ponteiro de saída em x1, sem SRET.
    if (!gotHead && fn_get_HeadCollider && fn_Collider_get_bounds_Injected) {
        void* headCollider = fn_get_HeadCollider(self, nullptr);
        if (headCollider) {
            BoundsVal bounds{};
            fn_Collider_get_bounds_Injected(headCollider, &bounds, nullptr);
            if (!std::isnan(bounds.cx) && !std::isnan(bounds.cy) && !std::isnan(bounds.cz) &&
                (bounds.cx != 0.0f || bounds.cy != 0.0f || bounds.cz != 0.0f)) {
                topWorld = Vector3(bounds.cx, bounds.cy, bounds.cz);
                gotHead = true;
            }
        }
    }

    // ── P2: Head bone via Animator (bone visual, fallback do collider) ───────
    if (!gotHead && fn_GetBoneTransform && fn_get_position) {
        void* animComp = *(void**)((uint8_t*)self + PLAYER_ANIM_COMPONENT_OFFSET);
        if (animComp) {
            void* animator = *(void**)((uint8_t*)animComp + ANIM_COMPONENT_ANIMATOR_OFFSET);
            if (animator) {
                void* headTransform = fn_GetBoneTransform(animator, HUMAN_BODY_BONE_HEAD, nullptr);
                if (headTransform) {
                    Vector3 headPos = fn_get_position(headTransform, nullptr);
                    if (!std::isnan(headPos.x) && !std::isnan(headPos.y) && !std::isnan(headPos.z)) {
                        topWorld = headPos;
                        gotHead = true;
                    }
                }
            }
        }
    }

    if (!gotHead) {
        // ── P3: Estimativa fixa ─────────────────────────────────────────────
        topWorld = Vector3(worldPos.x, worldPos.y + 1.75f, worldPos.z);
    }

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

    // HP check comum (evita alocar alvo morto/derrubado)
    int chp = fn_get_CurHP ? fn_get_CurHP(self, nullptr) : 1;

    float dX = screenTop.x - screenW * 0.5f;
    float dY = screenTop.y - screenH * 0.5f;
    float screenDist = sqrtf(dX * dX + dY * dY);

    // ── Aim Assist: registrar candidato ao alvo ──────────────────────────────
    // Somente inimigos vivos visíveis dentro do cone de FOV configurado
    if (sharedData->aimAssistEnabled && chp > 0) {
        float fovDeg = sharedData->aimAssistFovDeg;
        if (fovDeg < 1.0f || fovDeg > 90.0f) fovDeg = 30.0f;
        float fovRadiusPx = (float)screenW * (fovDeg / 90.0f) * 0.5f;

        if (screenDist < fovRadiusPx) {
            int  priority = sharedData->aimTargetPriority;
            bool isBetter = false;
            if (priority == 1) {
                // Lowest HP
                isBetter = (chp < g_aimBestHp);
            } else if (priority == 2) {
                // Nearest Distance (clipW da cabeça = profundidade de câmera)
                float depth = screenTop.z;
                isBetter = (depth < g_aimBestDepth);
            } else {
                // Nearest Center Screen (padrão)
                isBetter = (screenDist < g_aimBestScreenDist);
            }
            if (isBetter) {
                g_aimBestScreenDist = screenDist;
                g_aimBestHp         = chp;
                g_aimBestDepth      = screenTop.z;
                g_aimTargetWorld    = topWorld;
                g_aimHasTarget      = true;
            }
        }
    }

    // ── Silent Aim: rastrear inimigo mais próximo do centro (TELA INTEIRA) ───
    // Sem restrição de FOV — qualquer inimigo vivo na tela é candidato CAND.
    if (sharedData->silentAimEnabled && chp > 0) {
        if (screenDist < g_silentCandDist) {
            g_silentCandDist   = screenDist;
            g_silentCandTarget = topWorld;
            g_silentCandValid  = true;
        }
    }
    // ─────────────────────────────────────────────────────────────────────────

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

    // HP via chamada direta il2cpp
    int curHp = fn_get_CurHP ? fn_get_CurHP(self, nullptr) : 100;
    int maxHp = fn_get_MaxHP ? fn_get_MaxHP(self, nullptr) : 100;
    if (maxHp <= 0) maxHp = 100;
    entry.curHp  = curHp;
    entry.maxHp  = maxHp;

    // Knocked: campo bool IsKnockedDownBleed a offset 0x1150 do objeto Player
    entry.knocked = *(bool*)((uint8_t*)self + OFF_IsKnockedDownBleed);

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

    // ── Criar SHM IMEDIATAMENTE (Zygisk: fd já disponível como root) ──
    // Garante sharedData != null durante todos os stage markers abaixo.
    if (!sharedData) {
        shmFd = shm_create_file();
        if (shmFd >= 0) {
            sharedData = shm_map(shmFd);
            if (sharedData) {
                memset(sharedData, 0, sizeof(SharedESPData));
                sharedData->magic        = 0xDEADF00D;
                sharedData->debugLastCall = 1; // stage 1: SHM criado, thread rodando
            } else {
                close(shmFd);
            }
        }
    }

    hookLogWrite("Thread iniciada, aguardando libil2cpp.so... shm=%p", sharedData);
    LOGI("Hook thread iniciada, aguardando libil2cpp.so...");

    // ── Aguardar il2cpp carregar ──
    while (!isLibraryLoaded("libil2cpp.so")) {
        if (sharedData) sharedData->debugLastCall = 2; // stage 2: waiting libil2cpp
        sleep(1);
    }
    LOGI("libil2cpp.so detectada");
    hookLogWrite("libil2cpp.so detectada");

    // ── Pegar base address do libil2cpp.so ──
    // findLibrary retorna o PRIMEIRO mapeamento que contém o nome.
    // No Android ARM64, o primeiro mapeamento é r--p offset=0 = load base.
    // Os offsets do il2cppdumper (RVA) são relativos a esse endereço.
    if (sharedData) sharedData->debugLastCall = 3; // stage 3: libil2cpp found
    uintptr_t il2cpp_base = findLibrary("libil2cpp.so");
    if (!il2cpp_base) {
        if (sharedData) sharedData->debugLastCall = 93; // stage 93: base=0 error
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
    fn_IsLocalPlayer           = RESOLVE_OFFSET(bool(*)(void*, void*),             OFF_IsLocalPlayer);
    fn_IsLocalTeammate         = RESOLVE_OFFSET(bool(*)(void*, bool, void*),        OFF_IsLocalTeammate);
    fn_get_CurHP               = RESOLVE_OFFSET(int(*)(void*, void*),               OFF_get_CurHP);
    fn_get_MaxHP               = RESOLVE_OFFSET(int(*)(void*, void*),               OFF_get_MaxHP);
    // Aim Assist
    fn_get_eulerAngles         = RESOLVE_OFFSET(Vector3(*)(void*, void*),           OFF_Transform_get_eulerAngles);
    fn_set_eulerAngles         = RESOLVE_OFFSET(void(*)(void*, Vector3, void*),     OFF_Transform_set_eulerAngles);
    fn_GetBoneTransform        = RESOLVE_OFFSET(void*(*)(void*, int32_t, void*),    OFF_Animator_GetBoneTransform);
    fn_get_HeadCollider              = RESOLVE_OFFSET(void*(*)(void*, void*),             OFF_get_HeadCollider);
    fn_Collider_get_bounds_Injected  = RESOLVE_OFFSET(void(*)(void*, BoundsVal*, void*),  OFF_Collider_get_bounds_Injected);

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

    // Usar WorldToScreenPoint direto (mais preciso — evita erro de VP matrix manual)
    // Estamos na Unity thread (LateUpdate hook), sem risco de deadlock.
    useManualW2S = false;
    LOGI("Usando WorldToScreenPoint direto para W2S (posicionamento preciso)");

    // Verificar se tudo resolveu
    if (!fn_Camera_get_main || !fn_get_transform || !fn_get_position) {
        LOGE("Falha nos offsets: cam=%p trans=%p pos=%p",
             fn_Camera_get_main, fn_get_transform, fn_get_position);
        hookLogWrite("ERRO: offsets invalidos");
        return nullptr;
    }

    hookLogWrite("Game data dir: %s", getGameDataDir());

    // SHM já criado no início do hack_thread.
    // Se por algum motivo falhou lá, tenta novamente agora.
    if (!sharedData) {
        shmFd = shm_create_file();
        if (shmFd < 0) {
            LOGE("Falha ao criar shared memory: errno=%d (%s)", errno, strerror(errno));
            hookLogWrite("FALHA shm_create_file: errno=%d (%s) uid=%d", errno, strerror(errno), getuid());
            return nullptr;
        }
        sharedData = shm_map(shmFd);
        if (!sharedData) {
            close(shmFd);
            LOGE("Falha ao mapear shared memory: errno=%d (%s)", errno, strerror(errno));
            hookLogWrite("FALHA shm_map: errno=%d (%s)", errno, strerror(errno));
            return nullptr;
        }
        memset(sharedData, 0, sizeof(SharedESPData));
        sharedData->magic = 0xDEADF00D;
    }
    LOGI("SHM OK: path=%s mmap=%p", shmActivePath ? shmActivePath : "???", sharedData);
    hookLogWrite("SHM OK: path=%s mmap=%p", shmActivePath ? shmActivePath : "???", sharedData);

    // NOTA: Screen::get_width/height chamado DEPOIS do domain pronto
    // (il2cpp runtime precisa estar inicializado antes de chamar metodos)

    // ── VMT Hook no LateUpdate ──
    // Precisamos do MethodInfo* do LateUpdate pra trocar o methodPointer.
    // dlopen/dlsym NAO funciona (namespace do linker Android 7+).
    // Solucao: resolver simbolos il2cpp lendo o ELF do disco direto.
    LOGI("[1/5] Resolvendo il2cpp API via ELF (memoria)...");
    hookLogWrite("[1/5] Resolvendo il2cpp API via ELF (memoria)...");

    // Resolver as funcoes il2cpp via ELF com retry.
    // libil2cpp.so pode estar parcialmente mapeada logo apos ser detectada —
    // o linker faz mmap lazy dos segmentos. Retentativas com sleep garantem
    // que todas as paginas PT_DYNAMIC estao presentes antes da leitura.
    uintptr_t addr_domain_get = 0, addr_assemblies = 0, addr_get_image = 0;
    uintptr_t addr_image_name = 0, addr_class_from_name2 = 0, addr_class_method = 0;

    for (int elfTry = 0; elfTry < 6; elfTry++) {
        if (elfTry > 0) {
            LOGI("ELF retry %d/5 (aguardando mapeamento completo)...", elfTry);
            hookLogWrite("ELF retry %d/5...", elfTry);
            if (sharedData) sharedData->debugLastCall = 3; // volta a stage 3
            sleep(2);
            // Re-buscar base por seguranca
            uintptr_t newBase = findLibrary("libil2cpp.so");
            if (newBase) il2cpp_base = newBase;
        }
        addr_domain_get      = resolveElfSymbol(il2cpp_base, "il2cpp_domain_get");
        addr_assemblies      = resolveElfSymbol(il2cpp_base, "il2cpp_domain_get_assemblies");
        addr_get_image       = resolveElfSymbol(il2cpp_base, "il2cpp_assembly_get_image");
        addr_image_name      = resolveElfSymbol(il2cpp_base, "il2cpp_image_get_name");
        addr_class_from_name2= resolveElfSymbol(il2cpp_base, "il2cpp_class_from_name");
        addr_class_method    = resolveElfSymbol(il2cpp_base, "il2cpp_class_get_method_from_name");

        LOGI("ELF try %d: domain=%p asm=%p img=%p name=%p class=%p method=%p",
             elfTry + 1,
             (void*)addr_domain_get, (void*)addr_assemblies, (void*)addr_get_image,
             (void*)addr_image_name, (void*)addr_class_from_name2, (void*)addr_class_method);
        hookLogWrite("ELF try %d: domain=%p class=%p method=%p",
             elfTry + 1, (void*)addr_domain_get, (void*)addr_class_from_name2, (void*)addr_class_method);

        if (addr_domain_get && addr_assemblies && addr_get_image &&
            addr_image_name && addr_class_from_name2 && addr_class_method) break;
    }

    auto p_domain_get            = (void*(*)())                              addr_domain_get;
    auto p_domain_get_assemblies = (void**(*)(const void*, size_t*))         addr_assemblies;
    auto p_assembly_get_image    = (const void*(*)(const void*))             addr_get_image;
    auto p_image_get_name        = (const char*(*)(void*))                   addr_image_name;
    auto p_class_from_name       = (void*(*)(const void*, const char*, const char*)) addr_class_from_name2;
    auto p_class_get_method      = (void*(*)(void*, const char*, int))       addr_class_method;

    if (sharedData) sharedData->debugLastCall = 4; // stage 4: ELF resolve done
    if (!p_domain_get || !p_domain_get_assemblies || !p_assembly_get_image ||
        !p_image_get_name || !p_class_from_name || !p_class_get_method) {
        if (sharedData) sharedData->debugLastCall = 94; // stage 94: ELF resolve failed
        LOGE("ELF resolver falhou para uma ou mais funcoes il2cpp");
        hookLogWrite("ERRO: ELF resolver falhou apos 6 tentativas");
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

    if (sharedData) sharedData->debugLastCall = 5; // stage 5: waiting metadata
    // Fase 1: Esperar global-metadata.dat em /proc/self/maps
    // Timeout reduzido para 30s (era 60s) — FF carrega rapidamente
    bool metadataFound = false;
    for (int i = 0; i < 30; i++) {
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
        LOGI("  aguardando global-metadata.dat... (%d/30)", i+1);
        sleep(1);
    }
    if (!metadataFound) {
        if (sharedData) sharedData->debugLastCall = 95; // stage 95: metadata timeout
        LOGE("global-metadata.dat timeout 30s");
        hookLogWrite("ERRO: metadata timeout 30s");
        return nullptr;
    }
    if (sharedData) sharedData->debugLastCall = 6; // stage 6: metadata found

    // Fase 2: Esperar domain ficar pronto.
    // Sem sleep(3) fixo — verifica direto com retries curtos.
    LOGI("  Metadata encontrado. Tentando domain_get...");

    void *domain = nullptr;
    for (int i = 0; i < 15; i++) {
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
        if (sharedData) sharedData->debugLastCall = 7; // stage 7: waiting domain
        if (i >= 14) {
            if (sharedData) sharedData->debugLastCall = 97; // stage 97: domain timeout
            LOGE("il2cpp domain timeout (15s apos metadata)");
            hookLogWrite("ERRO: domain timeout 15s");
            return nullptr;
        }
        LOGI("  domain wait... (%d/15)", i+1);
        sleep(1);
    }

    // Agora il2cpp runtime esta pronto — seguro chamar metodos
    // NOTA: Screen::get_width/height NAO e chamado aqui.
    // O overlay conhece as dimensoes da tela via Android window system
    // e as escreve no SHM a cada frame (onOverlayDraw). Isso e mais
    // confiavel que chamar il2cpp de uma thread nao-Unity.
    LOGI("[3/5] il2cpp runtime pronto (screenW/H via overlay)");
    hookLogWrite("[3/5] il2cpp runtime pronto (screenW/H escrito pelo overlay)");

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
        if (sharedData) sharedData->debugLastCall = 96; // stage 96: assembly not found
        LOGE("Assembly-CSharp.dll nao encontrada!");
        hookLogWrite("ERRO: Assembly-CSharp.dll nao encontrada");
        return nullptr;
    }
    if (sharedData) sharedData->debugLastCall = 8; // stage 8: Assembly-CSharp found
    LOGI("Assembly-CSharp.dll = %p", cs_image);

    // Achar classe Player
    LOGI("[4/5] Buscando Player class...");
    hookLogWrite("[4/5] Buscando Player class...");
    if (sharedData) sharedData->debugLastCall = 81; // stage 81: finding Player class
    void *playerClass = p_class_from_name(cs_image, "COW.GamePlay", "Player");
    if (!playerClass) {
        if (sharedData) sharedData->debugLastCall = 98; // stage 98: Player class not found
        LOGE("Classe Player nao encontrada");
        hookLogWrite("ERRO: classe Player nao encontrada");
        return nullptr;
    }
    if (sharedData) sharedData->debugLastCall = 9; // stage 9: Player class found
    LOGI("playerClass = %p", playerClass);
    hookLogWrite("playerClass = %p", playerClass);

    // Pegar MethodInfo* do LateUpdate
    LOGI("  Chamando class_get_method_from_name(LateUpdate, 0)...");
    hookLogWrite("Buscando LateUpdate MethodInfo...");
    void *onUpdateMethodInfo = p_class_get_method(playerClass, "LateUpdate", 0);
    if (!onUpdateMethodInfo) {
        if (sharedData) sharedData->debugLastCall = 99; // stage 99: LateUpdate not found
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
    sharedData->hookApplied = 0xBEEF1234;  // sinaliza overlay que VMT hook esta ativo
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
