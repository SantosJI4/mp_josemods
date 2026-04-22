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
#include "dobby.h"

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

#define HOOK_BUILD_VER "v59-fix4"

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
#define PLAYER_ANIM_COMPONENT_OFFSET  0x700  // campo HFKJCLHCBGB na classe Player (dump L650012)
#define ANIM_COMPONENT_ANIMATOR_OFFSET 0x20   // campo KFGPIOMOLHI (Animator) na base GCommon.AnimationSystemComponent (dump L644587/644603)
#define HUMAN_BODY_BONE_HEAD           10    // UnityEngine.HumanBodyBones.Head

// Aimbot 2 — GetHeadTF() direto (v59-ab2)
// Player::GetHeadTF() (dump L652224) @ 0x67E689C — retorna UnityEngine.Transform* da cabeça
// IsCrouching() (dump L651872) @ 0x67566AC — detectar agachado (bloquear se deitado)
// get_IsDieing() (dump L651508) @ 0x675B1FC — detectar derrubado/sangrando
#define OFF_GetHeadTF          0x67E689C
#define OFF_IsCrouching        0x67566AC
#define OFF_get_IsDieing       0x675B1FC

// Aimbot 2 — campo legado (removido, mantido por documentação):
// OFF_PLAYER_HEAD_NODE e OFF_TRANSFORMNODE_TRANSFORM eram field offsets supostos, substituídos
// por chamada direta a GetHeadTF() que é mais confiável.

// PlayerColliderChecker — hitbox real da cabeça (igual o servidor usa para dano)
// Collider.get_bounds() retorna Bounds por valor: { m_Center @ +0x10, m_Extents @ +0x1C }
// List<ColliderInfo> layout IL2CPP: _items (array*) @ +0x10, _size @ +0x18
// array de objetos: elemento[i] em _items[i] (ponteiro), cada objeto: m_collider@0x10, m_hitBoxType@0x18
// HitPart.Head = 0
#define OFF_get_HeadCollider               0x676FEB4
// get_bounds() retorna Bounds (24 bytes) via SRET x8 — ABI incorreta para ponteiro direto.
// get_bounds_Injected(out Bounds& ret) recebe o ponteiro em x1 como parâmetro normal → correto.
#define OFF_Collider_get_bounds_Injected  0x9CB794C

// (offsets GetPartByCollider e IsHeadShotCheck removidos em v31 — server-side validates)

// Speed Hack — hook em PlayerAttributes::GetWeaponRunSpeedScale(int) (v41)
// Controla o multiplicador de velocidade ao segurar arma. Mais correto que GetMoveSpeedForFPPMode.
// Dump L714386: public System.Single GetWeaponRunSpeedScale(System.Int32) // 0x725EE1C
#define OFF_GetWeaponRunSpeedScale  0x725EE1C  // PlayerAttributes::GetWeaponRunSpeedScale(int)

// Recoil — hook em PlayerAttributes::GetScatterRate() (v41)
// Controla o spread/scatter das balas (recoil da arma). Retornar 0 = sem recoil.
// Dump L714459: public System.Single GetScatterRate() // 0x7261F4C
#define OFF_GetScatterRate          0x7261F4C  // PlayerAttributes::GetScatterRate()

// Player::get_Attributes() — retorna o PlayerAttributes do player (usado para cache)
// Dump L651529: public COW.GamePlay.PlayerAttributes get_Attributes() // 0x6752B38
// Offset do campo PlayerAttributes dentro de Player (para cache do local player attr):
#define OFF_PlayerAttributes_field  0x708      // Player.JKPFFNEMJIF (protected PlayerAttributes)

// Aimbot — Player::SetAimRotation(Quaternion q, bool forceUpdate) (v40)
// Método oficial do jogo para definir para onde o player mira.
// Dump L651569: public System.Void SetAimRotation(Quaternion, bool) // 0x67718B8
#define OFF_SetAimRotation          0x67718B8
// m_CurrentAimRotation @ 0x1834 — Quaternion atual (para smooth/slerp)
#define OFF_m_CurrentAimRotation    0x1834
// Wall check — AttackableEntity::IsVisible() — game-native visibility query
// Dump L645952: public virtual System.Boolean IsVisible() // 0x68C83F8
// Player herda de AttackableEntity e NAO faz override → chamar direto por offset é correto.
#define OFF_IsVisible               0x68C83F8
// ADS check — Player::get_IsSighting() — true quando o player está mirando (ADS)
// Dump L653775: public System.Boolean get_IsSighting() // 0x676689C
#define OFF_get_IsSighting          0x676689C
// IsFiring — Player::IsFiring() — true quando o player está atirando
// Dump L652303: public System.Boolean IsFiring() // 0x675D420
#define OFF_IsFiring                0x675D420

// Camera Controller — CameraControllerBase::LateUpdate
// Hookeado para aplicar aimbot/anti-recoil DEPOIS que o controlador posiciona a câmera.
// Offset é o RVA do código da função (como OFF_LateUpdate).
#define OFF_CameraController_LateUpdate 0x68FCE30

// ── Player Hacks (v49) ──────────────────────────────────────────────────────
// NickName — Player::get_NickName() — string IL2CPP do nome do player
// Dump L651414: public System.String get_NickName() // 0x676CDF0
#define OFF_get_NickName              0x676CDF0
// get_IsAmmoFree — PlayerAttributes::get_IsAmmoFree() — true = infinito
// Dump L714448: public System.Boolean get_IsAmmoFree() // 0x7261D18
#define OFF_get_IsAmmoFree            0x7261D18
// get_FSModeUseMedikitFasterRate — PlayerAttributes — fator de velocidade do medkit
// Dump L714436: public System.Single get_FSModeUseMedikitFasterRate() // 0x7261740
#define OFF_get_FSModeUseMedikitFasterRate 0x7261740
// get_InSwapWeaponCD — Player — true = em cooldown de troca de arma
// Dump L651567: public System.Boolean get_InSwapWeaponCD() // 0x67717AC
#define OFF_get_InSwapWeaponCD        0x67717AC
// IsMoving — Player — false = player não se move (backup para medkit andando)
// Dump L651981: public System.Boolean IsMoving(); // 0x676650C
#define OFF_IsMoving                  0x676650C
// get_CanMedkitOnMove — Player — true = pode usar medkit andando (função principal)
// Dump L653473: public System.Boolean get_CanMedkitOnMove(); // 0x6766680
#define OFF_get_CanMedkitOnMove       0x6766680
// CancelPreparation — Player — cancela o uso de item em andamento (medkit/item)
// Hook: quando medkitRunEnabled, ignoramos o cancelamento para manter o medkit ativo.
// Dump L652441: public System.Void CancelPreparation(); // 0x6805AC8
#define OFF_CancelPreparation         0x6805AC8
// get_EatSpeedScale — PlayerAttributes — multiplicador de velocidade de uso de itens
// Dump L714414: public System.Single get_EatSpeedScale(); // 0x7261068
#define OFF_get_EatSpeedScale         0x7261068

// ── Anti-recoil adicional (v55) ─────────────────────────────────────────────
// get_SkillScatterRate — PlayerAttributes — scatter adicional de habilidade
// Dump L714455: public float get_SkillScatterRate() // 0x7261DE4
#define OFF_get_SkillScatterRate      0x7261DE4
// get_SkillScatterRateSighting — PlayerAttributes — scatter extra durante ADS de habilidade
// Dump L714457: public float get_SkillScatterRateSighting() // 0x7261E98
#define OFF_get_SkillScatterRateSighting 0x7261E98

// ── Auto Aim (v59-fix4) — SyncStartFire via PlayerNetwork ──────────────────
// PlayerNetwork::SwapWeapon override — chamado SEMPRE (via vtable) ao trocar arma
// Dump L661975: public override System.Void SwapWeapon(int,bool,List) // 0x6A30774
#define OFF_SwapWeapon_PlayerNetwork  0x6A30774
// PlayerNetwork::SyncStartFire(byte) — sincroniza disparo com o servidor
// Dump L661892: public virtual System.Void SyncStartFire(byte) // 0x6A1F670
#define OFF_SyncStartFire             0x6A1F670
// PlayerNetwork::SyncStopFire() — para o disparo
// Dump L661898: public virtual System.Void SyncStopFire() // 0x6A21304
#define OFF_SyncStopFire              0x6A21304

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

// Wall check — AttackableEntity::IsVisible() (v42)
static bool (*fn_IsVisible)(void* self, void* method) = nullptr;
// ADS check — Player::get_IsSighting() (v43)
static bool (*fn_get_IsSighting)(void* self, void* method) = nullptr;
// Aimbot 2 — cabeça direta + filtros
static void* (*fn_GetHeadTF)(void* player, void* method)   = nullptr;  // GetHeadTF() → Transform*
static bool  (*fn_IsCrouching)(void* player, void* method) = nullptr;  // agachado/deitado
static bool  (*fn_get_IsDieing)(void* player, void* method)= nullptr;  // derrubado/sangrando
// IsFiring — Player::IsFiring() (v44)
static bool (*fn_IsFiring)(void* self, void* method) = nullptr;
// Speed hack — PlayerAttributes::GetWeaponRunSpeedScale(int) (v41)
static float (*orig_GetWeaponRunSpeedScale)(void* self, int32_t weaponType, void* method) = nullptr;
// Recoil — PlayerAttributes::GetScatterRate() (v41)
static float (*orig_GetScatterRate)(void* self, void* method) = nullptr;
// Cache do ponteiro PlayerAttributes do player local (para filtrar no hook)
static void* g_localPlayerAttr = nullptr;

// ── Player Hacks (v49) ───────────────────────────────────────────────────────
// NickName — Player::get_NickName() — só chamado, não hookeado
static void* (*fn_get_NickName)(void* self, void* method) = nullptr;
// IsAmmoFree hook
static bool  (*orig_get_IsAmmoFree)(void* self, void* method) = nullptr;
// FSModeUseMedikitFasterRate hook
static float (*orig_get_FSModeUseMedikitFasterRate)(void* self, void* method) = nullptr;
// InSwapWeaponCD hook
static bool  (*orig_get_InSwapWeaponCD)(void* self, void* method) = nullptr;
// IsMoving hook — retornar false = backup para medkit andando
static bool  (*orig_IsMoving)(void* self, void* method) = nullptr;
// get_CanMedkitOnMove hook — true = permite usar medkit enquanto anda (função principal)
static bool  (*orig_get_CanMedkitOnMove)(void* self, void* method) = nullptr;
// CancelPreparation hook — no-op quando medkitRunEnabled para não cancelar o medkit
static void  (*orig_CancelPreparation)(void* self, void* method) = nullptr;
// get_EatSpeedScale hook — valor alto = usa itens mais rápido (medkit fast)
static float (*orig_get_EatSpeedScale)(void* self, void* method) = nullptr;

// ── Anti-recoil adicional (v55) ──────────────────────────────────────────────
static float (*orig_get_SkillScatterRate)(void* self, void* method) = nullptr;
static float (*orig_get_SkillScatterRateSighting)(void* self, void* method) = nullptr;

// ── Auto Aim (v59-fix4) ──────────────────────────────────────────────────────
typedef void (*SyncStartFireFn)(void* self, uint8_t slot, void* method);
static SyncStartFireFn fn_SyncStartFire = nullptr;
typedef void (*SyncStopFireFn)(void* self, void* method);
static SyncStopFireFn fn_SyncStopFire = nullptr;
typedef void (*SwapWeaponFn)(void* self, int32_t slot, bool force, void* list, void* method);
static SwapWeaponFn orig_SwapWeapon = nullptr;

// Aimbot — Player::SetAimRotation
struct Quaternion { float x, y, z, w; };
typedef void (*SetAimRotationFn)(void* self, Quaternion q, bool forceUpdate, void* method);
static SetAimRotationFn fn_SetAimRotation = nullptr;

// LookRotation simples e correta: camPos -> targetPos -> pitch/yaw -> Quaternion
// Sem matriz 3x3, sem risco de degenerado ou flip.
static Quaternion LookQuatFromDir(float dx, float dy, float dz) {
    float hd = sqrtf(dx*dx + dz*dz);
    float pitch = -atan2f(dy, hd);          // rad, negativo = olha pra cima
    float yaw   =  atan2f(dx, dz);          // rad
    // Quaternion de Euler XY: Q = Qyaw(Y) * Qpitch(X)
    float hp = pitch * 0.5f, hy = yaw * 0.5f;
    float sp = sinf(hp), cp = cosf(hp);
    float sy = sinf(hy), cy = cosf(hy);
    return {
        sp * cy,   // x
        cp * sy,   // y
       -sp * sy,   // z
        cp * cy    // w
    };
}

// ============================================================
// Aimbot state (v31: direct aim, camera moves visibly to enemy head)
// ============================================================
static uintptr_t g_il2cpp_base = 0;

// Original LateUpdate
static void (*orig_OnUpdate)(void* self, void* methodInfo) = nullptr;
// Original CameraControllerBase::LateUpdate
static void (*orig_CameraLateUpdate)(void* self, void* methodInfo) = nullptr;

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
// Euler da câmera lido no início de cada frame (em Hook_OnUpdate/newFrame).
// Usado pelo anti-recoil em Hook_CameraLateUpdate sem precisar ler antes do orig.
static Vector3 g_cachedCamEuler{0.0f, 0.0f, 0.0f};
static bool    g_camEulerValid  = false;
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

// ── Aimbot via camera euler angles (v36/v39 approach) ──────────────────────────────
// Aplicado em Hook_CameraLateUpdate DEPOIS do orig → câmera aponta para a cabeça do alvo.
// CalculateViewAngle converte src→dst em pitch/yaw Unity.
struct Angles  { float pitch, yaw; };

static Angles CalculateViewAngle(Vector3 src, Vector3 dst) {
    float dx = dst.x - src.x;
    float dy = dst.y - src.y;
    float dz = dst.z - src.z;
    float hd = sqrtf(dx * dx + dz * dz);
    if (hd < 0.0001f) return {0.0f, 0.0f};
    float yaw   =  atan2f(dx, dz) * (180.0f / (float)M_PI);
    float pitch = -atan2f(dy, hd) * (180.0f / (float)M_PI);
    if (pitch >  89.0f) pitch =  89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
    return {pitch, yaw};
}

static Vector3 g_aimCandTarget{0.0f, 0.0f, 0.0f};  // melhor alvo deste frame
static float   g_aimCandDist  = 1e9f;
static bool    g_aimCandValid = false;
static int     g_pendingAutoFire = 0;   // countdown para disparar após swap
static int     g_pendingStopFire = 0;  // countdown para parar disparo
static void*   g_localPlayer  = nullptr;        // ponteiro do player local (cache por frame)

// ── Aimbot 2 — estado do alvo travado ────────────────────────────────────────
// Sistema com histerese: uma vez travado num alvo, só solta quando ele sai do FOV
// com margem extra (ab2ReleaseMult), morre ou some atrás de parede.
static void*   g_ab2LockedTarget  = nullptr;    // ponteiro do inimigo atualmente travado
static Vector3 g_ab2LockedHeadPos{};            // última posição válida da cabeça do alvo travado
static bool    g_ab2HasLock       = false;      // true = aimbot2 está travado num alvo
static float   g_ab2BestDist      = 1e9f;       // melhor candidato no frame atual (para seleção)

// ── Aimbot 1 lock state (histerese, como aimbot 2) ──────────────────────────
static void*   g_ab1LockedTarget  = nullptr;    // inimigo atualmente travado
static Vector3 g_ab1LockedHeadPos{};            // última posição válida da cabeça do alvo
static bool    g_ab1HasLock       = false;      // true = aimbot1 travado num alvo
static float   g_ab1BestDist      = 1e9f;       // melhor candidato deste frame
// ─────────────────────────────────────────────────────────────────────────────

// ============================================================
// HOOK DE VELOCIDADE — PlayerAttributes::GetWeaponRunSpeedScale(int) (v41)
// Retorna multiplicador de velocidade ao segurar arma.
// Filtramos pelo ponteiro PlayerAttributes do player local (g_localPlayerAttr).
// ============================================================
static float Hook_GetWeaponRunSpeedScale(void* self, int32_t weaponType, void* method) {
    float result = orig_GetWeaponRunSpeedScale
        ? orig_GetWeaponRunSpeedScale(self, weaponType, method)
        : 1.0f;
    if (sharedData && sharedData->speedEnabled && sharedData->speedValue > 0.5f) {
        return sharedData->speedValue;
    }
    return result;
}

// ============================================================
// HOOK DE RECOIL — PlayerAttributes::GetScatterRate() (v41)
// Controla o spread/scatter das balas. Retornar 0 = sem espalhamento.
// ============================================================
static float Hook_GetScatterRate(void* self, void* method) {
    if (sharedData && sharedData->recoilEnabled) {
        return 0.0f;
    }
    return orig_GetScatterRate ? orig_GetScatterRate(self, method) : 1.0f;
}

// ============================================================
// PLAYER HACKS (v49)
// ============================================================

// Helper: lê string IL2CPP (UTF-16LE) e converte para UTF-8 simples (ASCII subset)
// Estrutura do objeto System.String no IL2CPP:
//   +0x10: int32_t length (nº de chars UTF-16)
//   +0x14: uint16_t[] chars
static void il2cppStringToUtf8(void* strObj, char* out, int maxOut) {
    if (!strObj || !out || maxOut <= 0) { if (out) out[0] = '\0'; return; }
    out[0] = '\0';
    int32_t len = *(int32_t*)((uint8_t*)strObj + 0x10);
    if (len <= 0 || len > 128) return;
    uint16_t* chars = (uint16_t*)((uint8_t*)strObj + 0x14);
    int written = 0;
    for (int i = 0; i < len && written < maxOut - 1; i++) {
        uint16_t c = chars[i];
        if (c < 0x80) {
            out[written++] = (char)c;
        } else if (c < 0x800) {
            if (written + 1 >= maxOut - 1) break;
            out[written++] = (char)(0xC0 | (c >> 6));
            out[written++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (written + 2 >= maxOut - 1) break;
            out[written++] = (char)(0xE0 | (c >> 12));
            out[written++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[written++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[written] = '\0';
}

// PlayerAttributes::get_IsAmmoFree() → true = munição infinita
static bool Hook_GetIsAmmoFree(void* self, void* method) {
    if (sharedData && sharedData->ammoEnabled) {
        return true;
    }
    return orig_get_IsAmmoFree ? orig_get_IsAmmoFree(self, method) : false;
}

// PlayerAttributes::get_FSModeUseMedikitFasterRate() — rate ADITIVO do modo FS
// ERRO ANTERIOR: retornar 10.0 causava medkit de 29 segundos (rate é tempo extra, não velocidade)
// Fix: passa pelo original sem interferir. Medkit fast é via get_EatSpeedScale.
static float Hook_GetFSModeUseMedikitFasterRate(void* self, void* method) {
    return orig_get_FSModeUseMedikitFasterRate
        ? orig_get_FSModeUseMedikitFasterRate(self, method)
        : 0.0f;
}

// PlayerAttributes::get_EatSpeedScale() → multiplicador de TEMPO de uso de itens
// IMPORTANTE: valor alto = MAIS tempo (mais lento). Valor baixo = menos tempo (mais rápido).
// 0.01f = quase instantâneo. Default ≈ 1.0
static float Hook_GetEatSpeedScale(void* self, void* method) {
    if (sharedData && sharedData->medkitFastEnabled) {
        return 0.15f;  // ~0.5s (default ≈ 3s × 0.15 = 0.45s)
    }
    return orig_get_EatSpeedScale ? orig_get_EatSpeedScale(self, method) : 1.0f;
}

// Player::get_CanMedkitOnMove() → true = pode usar medkit em movimento
// Esta é a função principal — mais específica que IsMoving()
static bool Hook_GetCanMedkitOnMove(void* self, void* method) {
    if (sharedData && sharedData->medkitRunEnabled) {
        return true;
    }
    return orig_get_CanMedkitOnMove ? orig_get_CanMedkitOnMove(self, method) : false;
}

// Player::CancelPreparation() — jogo chama quando detecta movimento durante uso de item
// No-op quando medkitRunEnabled: impede o jogo de cancelar o medkit ao andar.
static void Hook_CancelPreparation(void* self, void* method) {
    if (sharedData && sharedData->medkitRunEnabled) return;
    if (orig_CancelPreparation) orig_CancelPreparation(self, method);
}

// Player::get_InSwapWeaponCD() → false = sem cooldown de troca de arma
static bool Hook_GetInSwapWeaponCD(void* self, void* method) {
    if (sharedData && sharedData->fastWeaponSwitch) {
        return false;
    }
    return orig_get_InSwapWeaponCD ? orig_get_InSwapWeaponCD(self, method) : false;
}

// Player::IsMoving() → false = jogo pensa que player está parado → medkit não cancela
static bool Hook_IsMoving(void* self, void* method) {
    if (sharedData && sharedData->medkitRunEnabled) {
        return false;
    }
    return orig_IsMoving ? orig_IsMoving(self, method) : false;
}

// Anti-recoil adicional: SkillScatterRate (bônus de dispersão de habilidade)
static float Hook_get_SkillScatterRate(void* self, void* method) {
    if (sharedData && sharedData->recoilEnabled) return 0.0f;
    return orig_get_SkillScatterRate ? orig_get_SkillScatterRate(self, method) : 1.0f;
}

// Anti-recoil adicional: SkillScatterRateSighting (dispersão durante ADS de habilidade)
static float Hook_get_SkillScatterRateSighting(void* self, void* method) {
    if (sharedData && sharedData->recoilEnabled) return 0.0f;
    return orig_get_SkillScatterRateSighting ? orig_get_SkillScatterRateSighting(self, method) : 1.0f;
}

// ============================================================
// AUTO AIM (fix4) — PlayerNetwork::SwapWeapon override
// DobbyHook no endereço direto 0x6A30774 (override correto — nunca é o base 0x67D6194).
// Detecta troca de arma do player local e agenda snap+fire via SyncStartFire.
// ============================================================
static void Hook_SwapWeapon(void* self, int32_t slot, bool force, void* list, void* method) {
    if (orig_SwapWeapon) orig_SwapWeapon(self, slot, force, list, method);
    if (!self || !sharedData || !hookActive.load()) return;
    if (!sharedData->autoAimEnabled) return;
    if (!fn_IsLocalPlayer || !fn_IsLocalPlayer(self, nullptr)) return;
    if (g_aimCandValid) {
        g_pendingAutoFire = 5;  // 5 frames: deixa a troca completar antes de atirar
    }
}

// ============================================================
// HOOK DA CÂMERA — CameraControllerBase::LateUpdate
// Aplicado DEPOIS que o controlador posiciona a câmera.
// Aimbot e anti-recoil correm aqui (v33).
//
// FIX v33-crash: orig é chamado PRIMEIRO (seguro) e os euler angles
// pré-frame vêm de g_cachedCamEuler (setado em Hook_OnUpdate/newFrame),
// eliminando a leitura de g_camTransform antes do orig que causava crash
// quando o ponteiro estava inválido/stale. Guards NaN adicionados.
// ============================================================
static void Hook_CameraLateUpdate(void* self, void* methodInfo) {
    // Chamar original PRIMEIRO — câmera posicionada corretamente
    if (orig_CameraLateUpdate)
        orig_CameraLateUpdate(self, methodInfo);

    if (!hookActive.load() || !sharedData || !g_camTransform ||
        !fn_get_eulerAngles || !fn_set_eulerAngles) return;

    // Anti-recoil SÓ quando disparando (triggerHeld==1).
    // v47: não conflitar com aimbot — quando silentAim ativo e com alvo válido,
    // o aimbot já controla a mira. Anti-recoil rodando junto desfaz o snap frame a frame.
    bool aimbotControlling = sharedData->silentAimEnabled && g_aimCandValid;
    if (sharedData->recoilEnabled && g_camEulerValid &&
        sharedData->triggerHeld == 1 && !aimbotControlling) {
        Vector3 postOrig = fn_get_eulerAngles(g_camTransform, nullptr);
        if (std::isnan(postOrig.y) || std::isnan(postOrig.z)) return;
        fn_set_eulerAngles(g_camTransform,
            Vector3(g_cachedCamEuler.x, postOrig.y, postOrig.z), nullptr);
    }
}

static void Hook_OnUpdate(void* self, void* methodInfo) {
    // ── Player local: SetAimRotation (v40) ───────────────────────────────────
    if (self && sharedData && hookActive.load() &&
        fn_IsLocalPlayer && fn_IsLocalPlayer(self, nullptr)) {
        g_localPlayer = self;
        // Cachear PlayerAttributes do player local para filtro nos hooks de speed/recoil
        void* attrPtr = *(void**)((uint8_t*)self + OFF_PlayerAttributes_field);
        if (attrPtr && (uintptr_t)attrPtr > 0x1000) g_localPlayerAttr = attrPtr;
        // ── Aimbot 2 (v59-ab2v2): snap direto à cabeça — SEM condição IsFiring ──
        // Não precisa estar atirando. Snapa para a cabeça do alvo travado sempre
        // que aimbot2Enabled e g_ab2HasLock (lock selecionado no loop de inimigos).
        // Smooth configurável: 0 = snap instantâneo, 0.05-0.9 = lerp suave.
        if (sharedData->aimbot2Enabled && g_ab2HasLock &&
            fn_SetAimRotation && g_camTransform && fn_get_position) {
            Vector3 camPos = fn_get_position(g_camTransform, nullptr);
            if (!std::isnan(camPos.x) && !std::isnan(camPos.y) && !std::isnan(camPos.z)) {
                float dx = g_ab2LockedHeadPos.x - camPos.x;
                float dy = g_ab2LockedHeadPos.y - camPos.y;
                float dz = g_ab2LockedHeadPos.z - camPos.z;
                float len = sqrtf(dx*dx + dy*dy + dz*dz);
                if (len >= 0.1f) {
                    Quaternion targetQ = LookQuatFromDir(dx, dy, dz);
                    float smooth = sharedData->aimbot2Smooth;
                    if (smooth > 0.0f && smooth < 1.0f) {
                        // Lerp entre rotação atual e target para suavidade
                        Quaternion curQ = *(Quaternion*)((uint8_t*)self + OFF_m_CurrentAimRotation);
                        float dot = curQ.x*targetQ.x + curQ.y*targetQ.y
                                  + curQ.z*targetQ.z + curQ.w*targetQ.w;
                        if (dot < 0.0f) {
                            targetQ.x=-targetQ.x; targetQ.y=-targetQ.y;
                            targetQ.z=-targetQ.z; targetQ.w=-targetQ.w;
                        }
                        float t = 1.0f - smooth;  // quanto mover por frame: 1=snap, ~0.05=muito suave
                        targetQ.x = curQ.x + (targetQ.x - curQ.x) * t;
                        targetQ.y = curQ.y + (targetQ.y - curQ.y) * t;
                        targetQ.z = curQ.z + (targetQ.z - curQ.z) * t;
                        targetQ.w = curQ.w + (targetQ.w - curQ.w) * t;
                        // Normalizar quaternion
                        float qlen = sqrtf(targetQ.x*targetQ.x + targetQ.y*targetQ.y
                                         + targetQ.z*targetQ.z + targetQ.w*targetQ.w);
                        if (qlen > 0.001f) {
                            targetQ.x/=qlen; targetQ.y/=qlen;
                            targetQ.z/=qlen; targetQ.w/=qlen;
                        }
                    }
                    fn_SetAimRotation(self, targetQ, true, nullptr);
                }
            }
        }

        // ── Aimbot 1 (silentAim): SLERP suave + lock à cabeça real ──────────────
        // g_ab1LockedHeadPos = posição GetHeadTF do alvo travado (atualizado no loop de inimigos)
        // Filtros já aplicados no loop: isDying, prone, parede, FOV, histerese
        if (g_ab1HasLock && fn_SetAimRotation && g_camTransform && fn_get_position &&
            sharedData->silentAimEnabled) {

            bool isFiring = fn_IsFiring && fn_IsFiring(self, nullptr);
            if (isFiring) {
                Vector3 camPos = fn_get_position(g_camTransform, nullptr);
                if (!std::isnan(camPos.x) && !std::isnan(camPos.y) && !std::isnan(camPos.z)) {
                    float dx = g_ab1LockedHeadPos.x - camPos.x;
                    float dy = g_ab1LockedHeadPos.y - camPos.y;
                    float dz = g_ab1LockedHeadPos.z - camPos.z;
                    float len = sqrtf(dx*dx + dy*dy + dz*dz);
                    if (len >= 0.1f) {
                        Quaternion targetQ = LookQuatFromDir(dx, dy, dz);
                        float smooth = sharedData->aimbotSmooth;
                        if (smooth > 0.0f && smooth < 1.0f) {
                            // SLERP: interpolação suave frame a frame
                            Quaternion curQ = *(Quaternion*)((uint8_t*)self + OFF_m_CurrentAimRotation);
                            float dot = curQ.x*targetQ.x + curQ.y*targetQ.y
                                      + curQ.z*targetQ.z + curQ.w*targetQ.w;
                            if (dot < 0.0f) {
                                targetQ.x=-targetQ.x; targetQ.y=-targetQ.y;
                                targetQ.z=-targetQ.z; targetQ.w=-targetQ.w;
                            }
                            float t = 1.0f - smooth; // 1=snap, 0.05=muito suave
                            targetQ.x = curQ.x + (targetQ.x - curQ.x) * t;
                            targetQ.y = curQ.y + (targetQ.y - curQ.y) * t;
                            targetQ.z = curQ.z + (targetQ.z - curQ.z) * t;
                            targetQ.w = curQ.w + (targetQ.w - curQ.w) * t;
                            float qlen = sqrtf(targetQ.x*targetQ.x + targetQ.y*targetQ.y
                                             + targetQ.z*targetQ.z + targetQ.w*targetQ.w);
                            if (qlen > 0.001f) {
                                targetQ.x/=qlen; targetQ.y/=qlen;
                                targetQ.z/=qlen; targetQ.w/=qlen;
                            }
                        }
                        fn_SetAimRotation(self, targetQ, true, nullptr);
                    }
                }
            }
        }

        // ── Auto Aim (fix4): snap + SyncStartFire quando countdown chega a zero ──
        if (sharedData->autoAimEnabled) {
            // Parar disparo anterior se estava rodando
            if (g_pendingStopFire > 0) {
                g_pendingStopFire--;
                if (g_pendingStopFire == 0 && fn_SyncStopFire)
                    fn_SyncStopFire(self, nullptr);
            }
            // Disparar quando swap detectado
            if (g_pendingAutoFire > 0) {
                g_pendingAutoFire--;
                if (g_pendingAutoFire == 0 && g_aimCandValid &&
                    fn_SyncStartFire && fn_SetAimRotation &&
                    g_camTransform && fn_get_position) {
                    Vector3 camPos = fn_get_position(g_camTransform, nullptr);
                    if (!std::isnan(camPos.x) && !std::isnan(camPos.y) && !std::isnan(camPos.z)) {
                        float dx = g_aimCandTarget.x - camPos.x;
                        float dy = g_aimCandTarget.y - camPos.y;
                        float dz = g_aimCandTarget.z - camPos.z;
                        float len = sqrtf(dx*dx + dy*dy + dz*dz);
                        if (len >= 0.01f) {
                            float rageOff = sharedData->aimRageOffsetY;
                            if (rageOff != 0.0f) dy += rageOff;
                            Quaternion aimQ = LookQuatFromDir(dx, dy, dz);
                            Quaternion curQ = *(Quaternion*)((uint8_t*)self + OFF_m_CurrentAimRotation);
                            float dot = curQ.x*aimQ.x+curQ.y*aimQ.y+curQ.z*aimQ.z+curQ.w*aimQ.w;
                            if (dot < 0.0f) { aimQ.x=-aimQ.x; aimQ.y=-aimQ.y; aimQ.z=-aimQ.z; aimQ.w=-aimQ.w; }
                            fn_SetAimRotation(self, aimQ, true, nullptr);
                            fn_SyncStartFire(self, 0, nullptr);  // self = PlayerNetwork*
                            g_pendingStopFire = 4;  // parar após 4 frames (~66ms)
                        }
                    }
                }
            }
        }

        if (orig_OnUpdate) orig_OnUpdate(self, methodInfo);

        return;
    }
    // ─────────────────────────────────────────────────────────────────────────

    // Chamar original (jogo processa lógica, física) para não-local players
    if (orig_OnUpdate) {
        orig_OnUpdate(self, methodInfo);
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

        // ── Aim: g_camTransform (necessário para silentAim no próximo frame) ───
        g_camTransform = fn_get_transform ? fn_get_transform(g_cachedCamera, nullptr) : nullptr;

        // Cache euler pré-frame para anti-recoil em Hook_CameraLateUpdate.
        // Lido AQUI (antes do camera controller rodar) para capturar o estado
        // sem recoil. Guard NaN: se inválido, anti-recoil fica desativado neste frame.
        g_camEulerValid = false;
        if (g_camTransform && fn_get_eulerAngles) {
            Vector3 e = fn_get_eulerAngles(g_camTransform, nullptr);
            if (!std::isnan(e.x) && !std::isnan(e.y) && !std::isnan(e.z)) {
                g_cachedCamEuler = e;
                g_camEulerValid  = true;
            }
        }

        // Camera-moving aimbot REMOVIDO (v29).
        // silentAim é o único aimbot: snap pré-orig, restore pós-orig, câmera não mexe.
        // aimAssistHasTarget reflete se silentAim tem alvo para o HUD do overlay.
        // HUD: tinha alvo no frame anterior?
        sharedData->aimAssistHasTarget =
            (g_aimCandValid && sharedData->silentAimEnabled) ? 1 : 0;
        // Auto Aim: atualiza indicador de alvo
        sharedData->autoAimHasTarget = g_aimCandValid ? 1 : 0;
        // Aimbot 2: publica estado do lock
        sharedData->aimbot2HasTarget = g_ab2HasLock ? 1 : 0;

        // Resetar candidatos para este frame (aimbot1)
        g_aimBestScreenDist = 1e9f;
        g_aimBestHp         = 0x7fffffff;
        g_aimBestDepth      = 1e9f;
        g_aimCandValid      = false;
        g_aimCandDist       = 1e9f;
        g_localPlayer       = nullptr;  // reset cache do player local
        // Resetar candidatos aimbot2 (lock é preservado entre frames,
        // mas o "melhor candidato do frame" começa do zero)
        g_ab2BestDist       = 1e9f;
        // Se aimbot2 foi desativado, libera o lock
        if (!sharedData->aimbot2Enabled) {
            g_ab2LockedTarget = nullptr;
            g_ab2HasLock      = false;
        }
        // Aimbot 1: reset candidato do frame; lock preservado entre frames
        g_ab1BestDist = 1e9f;
        if (!sharedData->silentAimEnabled) {
            g_ab1LockedTarget = nullptr;
            g_ab1HasLock      = false;
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

    // ── Filtrar eu mesmo + aliados ──
    // IsLocalTeammate(true) = inclui o proprio jogador + todos os aliados
    // (o player local já foi tratado acima e retornou; aqui só aliados)
    if (fn_IsLocalTeammate && fn_IsLocalTeammate(self, true, nullptr)) return;
    // Fallback: filtra self (caso IsLocalTeammate não disponível)
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
    // P1: get_HeadCollider() → bounds.center  (hitbox exata registrada pelo servidor)
    // P2: Animator.GetBoneTransform(Head=10) → position (bone visual, muito preciso)
    // P3: worldPos.y + 1.75 — estimativa fixa
    Vector3 bottomWorld(worldPos.x, worldPos.y - 0.05f, worldPos.z);
    Vector3 topWorld;
    bool gotHead = false;

    // ── Aimbot 2: GetHeadTF() — posição real da cabeça via método do jogo ────
    // Filtros aplicados ANTES de usar como candidato:
    //   • IsVisible()      — ignora inimigos atrás de paredes
    //   • get_IsDieing()   — ignora derrubados/sangrando
    //   • IsCrouching() + altura — ignora deitados (prone: agachado E posição Y baixa)
    Vector3 ab2HeadPos{};
    bool    ab2HeadValid = false;
    if (sharedData->aimbot2Enabled && fn_GetHeadTF && fn_get_position) {
        // Filtro: derrubado/sangrando
        bool isDying = fn_get_IsDieing && fn_get_IsDieing(self, nullptr);
        if (!isDying) {
            // Filtro: deitado (prone) — agachado E altura abaixo de 0.5m da base
            bool isProneSkip = false;
            if (sharedData->aimbot2IgnoreProne && fn_IsCrouching) {
                bool crouching = fn_IsCrouching(self, nullptr);
                if (crouching) {
                    // Deitado: posição da cabeça muito próxima da base do player
                    // Normal em pé: cabeça ~1.7m acima. Agachado: ~1.2m. Deitado: <0.8m
                    void* htf = fn_GetHeadTF(self, nullptr);
                    if (htf) {
                        Vector3 hp = fn_get_position(htf, nullptr);
                        if (!std::isnan(hp.y) && (hp.y - worldPos.y) < 0.75f)
                            isProneSkip = true;
                    }
                }
            }
            if (!isProneSkip) {
                void* headTF = fn_GetHeadTF(self, nullptr);
                if (headTF) {
                    Vector3 hp = fn_get_position(headTF, nullptr);
                    if (!std::isnan(hp.x) && !std::isnan(hp.y) && !std::isnan(hp.z) &&
                        (hp.x != 0.0f || hp.y != 0.0f || hp.z != 0.0f)) {
                        ab2HeadPos   = hp;
                        ab2HeadValid = true;
                    }
                }
            }
        }
    }

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
    // Guard: validar que animComp parece um ponteiro heap válido (> 0x1000)
    // antes de desreferenciar — offset 0x700 pode estar errado para alguns players.
    if (!gotHead && fn_GetBoneTransform && fn_get_position) {
        void* animComp = *(void**)((uint8_t*)self + PLAYER_ANIM_COMPONENT_OFFSET);
        if (animComp && (uintptr_t)animComp > 0x1000) {
            void* animator = *(void**)((uint8_t*)animComp + ANIM_COMPONENT_ANIMATOR_OFFSET);
            if (animator && (uintptr_t)animator > 0x1000) {
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

    // ── Aimbot 2: seleção de candidato (head direto, lock com histerese) ─────
    // Totalmente separado do aimbot1. Sem condição IsFiring.
    // Seleciona melhor candidato por distância da tela ao centro.
    // Filtros já aplicados acima (isDying, prone, via ab2HeadValid).
    if (sharedData->aimbot2Enabled && ab2HeadValid && chp > 0) {
        // Filtro obrigatório: IsVisible (ignora inimigos atrás de parede)
        bool ab2Vis = fn_IsVisible ? fn_IsVisible(self, nullptr) : true;
        if (ab2Vis && screenTop.z <= 120.0f) {
            // FOV do aimbot2 (separado do FOV do aimbot1)
            float ab2FovDeg = sharedData->aimbot2FovDeg;
            if (ab2FovDeg < 1.0f || ab2FovDeg > 180.0f) ab2FovDeg = 60.0f;
            float ab2RadPx = (float)screenW * (ab2FovDeg / 90.0f) * 0.5f;

            // ── Histerese: se self é o alvo travado, usa raio maior para soltar ──
            float effectiveRadius = ab2RadPx;
            if (self == g_ab2LockedTarget && g_ab2HasLock) {
                effectiveRadius = ab2RadPx * 1.35f;  // margem 35% para não soltar ao mover
            }

            if (screenDist < effectiveRadius) {
                // Candidato melhor = mais próximo ao centro da tela
                if (screenDist < g_ab2BestDist) {
                    g_ab2BestDist      = screenDist;
                    // Travar imediatamente — salva ponteiro e posição da cabeça
                    g_ab2LockedTarget  = self;
                    g_ab2LockedHeadPos = ab2HeadPos;
                    g_ab2HasLock       = true;
                }
            }
        }
    }

    // ── Aimbot 1 (silentAim): seleção completa com lock, GetHeadTF e filtros ────
    // Prioridade única: mais próximo ao centro da tela (sem HP/distância)
    // Filtros: isDying, deitado/prone, atrás de parede, fora do FOV
    if (sharedData->silentAimEnabled && chp > 0) do {
        // Filtro 1: derrubado/sangrando — duas checagens para robustez
        bool isKnocked = *(bool*)((uint8_t*)self + OFF_IsKnockedDownBleed);
        if (isKnocked) break;
        if (fn_get_IsDieing && fn_get_IsDieing(self, nullptr)) break;

        // Filtro 2: deitado (prone) — agachado E cabeça < 0.75m da base
        if (fn_IsCrouching && fn_IsCrouching(self, nullptr) && fn_GetHeadTF) {
            void* htf_prone = fn_GetHeadTF(self, nullptr);
            if (htf_prone) {
                Vector3 hp_prone = fn_get_position(htf_prone, nullptr);
                if (!std::isnan(hp_prone.y) && (hp_prone.y - worldPos.y) < 0.75f) break;
            }
        }

        // Filtro 3: atrás de parede
        if (fn_IsVisible && !fn_IsVisible(self, nullptr)) break;

        // Filtro 4: fora do FOV
        float fovDeg1 = sharedData->aimAssistFovDeg;
        if (fovDeg1 < 1.0f || fovDeg1 > 180.0f) fovDeg1 = 60.0f;
        float fovRadiusPx1 = (float)screenW * (fovDeg1 / 90.0f) * 0.5f;

        // Histerese: alvo travado usa raio 35% maior para não soltar ao mover
        float ab1Radius = (self == g_ab1LockedTarget && g_ab1HasLock)
                          ? fovRadiusPx1 * 1.35f : fovRadiusPx1;
        if (screenDist >= ab1Radius) break;

        // Cabeça via GetHeadTF — segue o bone real, sem lag de collider/estimativa
        Vector3 ab1Head = topWorld;
        if (fn_GetHeadTF) {
            void* htf_a1 = fn_GetHeadTF(self, nullptr);
            if (htf_a1) {
                Vector3 hp_a1 = fn_get_position(htf_a1, nullptr);
                if (!std::isnan(hp_a1.x) && !std::isnan(hp_a1.y) && !std::isnan(hp_a1.z) &&
                    (hp_a1.x != 0.0f || hp_a1.y != 0.0f || hp_a1.z != 0.0f)) {
                    ab1Head = hp_a1;
                }
            }
        }

        // Seleciona apenas o mais próximo ao centro — sem prioridade HP ou distância
        if (screenDist < g_ab1BestDist) {
            g_ab1BestDist      = screenDist;
            g_ab1LockedTarget  = self;
            g_ab1LockedHeadPos = ab1Head;
            g_ab1HasLock       = true;
            // Atualiza legado g_aimCandTarget para autoAim/HUD
            g_aimCandTarget    = ab1Head;
            g_aimCandDist      = screenDist;
            g_aimCandValid     = true;
        }
    } while(0);

    // autoAim sem silentAim: seleção simples com FOV (sem filtros avançados)
    if (sharedData->autoAimEnabled && !sharedData->silentAimEnabled && chp > 0) {
        bool isVis = fn_IsVisible ? fn_IsVisible(self, nullptr) : true;
        if (isVis) {
            float fovDeg2 = sharedData->aimAssistFovDeg;
            if (fovDeg2 < 1.0f || fovDeg2 > 180.0f) fovDeg2 = 60.0f;
            float fovRadiusPx2 = (float)screenW * (fovDeg2 / 90.0f) * 0.5f;
            if (screenDist < fovRadiusPx2 && screenDist < g_aimBestScreenDist) {
                g_aimBestScreenDist = screenDist;
                g_aimCandTarget     = topWorld;
                g_aimCandDist       = screenDist;
                g_aimCandValid      = true;
            }
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

    // Nick name: Player::get_NickName() → string IL2CPP (v49)
    entry.nick[0] = '\0';
    if (fn_get_NickName) {
        void* strObj = fn_get_NickName(self, nullptr);
        if (strObj) il2cppStringToUtf8(strObj, entry.nick, sizeof(entry.nick));
    }

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
    g_il2cpp_base = il2cpp_base;
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
    // Aimbot SetAimRotation
    fn_SetAimRotation = RESOLVE_OFFSET(SetAimRotationFn, OFF_SetAimRotation);
    fn_IsVisible      = RESOLVE_OFFSET(bool(*)(void*, void*),              OFF_IsVisible);
    fn_get_IsSighting = RESOLVE_OFFSET(bool(*)(void*, void*),              OFF_get_IsSighting);
    fn_IsFiring       = RESOLVE_OFFSET(bool(*)(void*, void*),              OFF_IsFiring);
    // Aimbot 2 — GetHeadTF + filtros
    fn_GetHeadTF      = RESOLVE_OFFSET(void*(*)(void*, void*),             OFF_GetHeadTF);
    fn_IsCrouching    = RESOLVE_OFFSET(bool(*)(void*, void*),              OFF_IsCrouching);
    fn_get_IsDieing   = RESOLVE_OFFSET(bool(*)(void*, void*),              OFF_get_IsDieing);
    // Auto Aim (fix4) — SyncStartFire e SyncStopFire da PlayerNetwork
    fn_SyncStartFire  = RESOLVE_OFFSET(SyncStartFireFn, OFF_SyncStartFire);
    fn_SyncStopFire   = RESOLVE_OFFSET(SyncStopFireFn,  OFF_SyncStopFire);
    // Player Hacks (v49) — get_NickName só chamado (não hookeado)
    fn_get_NickName   = RESOLVE_OFFSET(void*(*)(void*, void*),             OFF_get_NickName);
    // Speed Hack — resolvido via VmtHook em UpdateVelocity (ver adiante)

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
    uintptr_t addr_class_get_methods = 0;

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
        addr_class_get_methods = resolveElfSymbol(il2cpp_base, "il2cpp_class_get_methods");

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
    // il2cpp_class_get_methods(class, &iter) — itera metodos; iter=nullptr inicia
    auto p_class_get_methods     = (void*(*)(void*, void**))                 addr_class_get_methods;

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
    // FIX stage-6 crash: metadata mapeado != runtime inicializado.
    // p_domain_get() chamado cedo demais crashava dentro do il2cpp.
    // Aguarda 3s fixo para dar tempo ao runtime completar a init.
    LOGI("  Metadata encontrado. Aguardando runtime init (3s)...");
    hookLogWrite("metadata ok — aguardando runtime 3s...");
    sleep(3);
    LOGI("  Tentando domain_get...");

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

    // CRÍTICO: verificar que methodPointer bate com nosso offset.
    // p_class_get_method pode retornar um LateUpdate errado (há vários no dump).
    // Se não bater, itera todos os métodos para achar o correto.
    // Hooking o método errado = Hook_OnUpdate recebe self inválido = crash.
    void *currentMethodPtr = *(void**)onUpdateMethodInfo;
    void *expectedPtr = (void*)(il2cpp_base + OFF_LateUpdate);
    LOGI("  methodPointer atual = %p, esperado (base+off) = %p, match=%s",
         currentMethodPtr, expectedPtr,
         (currentMethodPtr == expectedPtr) ? "SIM" : "NAO");
    hookLogWrite("LateUpdate ptr=%p expected=%p match=%s",
         currentMethodPtr, expectedPtr,
         (currentMethodPtr == expectedPtr) ? "SIM" : "NAO");

    if (currentMethodPtr != expectedPtr) {
        LOGE("  MISMATCH — iterando metodos do Player para achar OFF_LateUpdate...");
        hookLogWrite("MISMATCH — buscando LateUpdate por endereco...");
        void *correctMethodInfo = nullptr;
        if (p_class_get_methods) {
            void *iter2 = nullptr;
            void *mi2   = nullptr;
            while ((mi2 = p_class_get_methods(playerClass, &iter2)) != nullptr) {
                if ((uintptr_t)*(void**)mi2 == (uintptr_t)expectedPtr) {
                    correctMethodInfo = mi2;
                    break;
                }
            }
        }
        if (!correctMethodInfo) {
            if (sharedData) sharedData->debugLastCall = 99;
            LOGE("  LateUpdate correto nao encontrado! Abortando hook.");
            hookLogWrite("ERRO: LateUpdate correto nao encontrado");
            return nullptr;
        }
        onUpdateMethodInfo = correctMethodInfo;
        LOGI("  LateUpdate correto encontrado via iteracao: %p", onUpdateMethodInfo);
        hookLogWrite("LateUpdate correto: %p", onUpdateMethodInfo);
    }

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

    // ── Hook CameraControllerBase::LateUpdate (aimbot + anti-recoil v33) ──
    // Aplicar APÓS hook do Player::LateUpdate.
    // CameraControllerBase::LateUpdate roda depois de Player::LateUpdate.
    // Setamos euler da câmera DEPOIS que o controlador posiciona a câmera →
    // câmera permanece acoplada ao player e balas seguem camera.forward.
    void* cameraBaseClass = p_class_from_name(cs_image, "COW.GamePlay", "CameraControllerBase");
    if (cameraBaseClass) {
        void* camLateUpdateMethodInfo = p_class_get_method(cameraBaseClass, "LateUpdate", 0);
        if (camLateUpdateMethodInfo) {
            if (VmtHook(camLateUpdateMethodInfo, (void*)Hook_CameraLateUpdate, (void**)&orig_CameraLateUpdate)) {
                LOGI("VMT Hook CameraControllerBase::LateUpdate aplicado: orig=%p", orig_CameraLateUpdate);
                hookLogWrite("VMT CameraLateUpdate: orig=%p", orig_CameraLateUpdate);
            } else {
                LOGE("VMT Hook CameraControllerBase::LateUpdate FALHOU");
                hookLogWrite("ERRO: VMT CameraLateUpdate falhou");
            }
        } else {
            LOGE("LateUpdate nao encontrado em CameraControllerBase");
            hookLogWrite("ERRO: LateUpdate nao encontrado em CameraControllerBase");
        }
    } else {
        LOGE("CameraControllerBase nao encontrada");
        hookLogWrite("ERRO: CameraControllerBase nao encontrada");
    }

    // AUTO AIM (fix4) — DobbyHook em PlayerNetwork::SwapWeapon (override correto)
    // 0x6A30774 é o override da PlayerNetwork — chamado via vtable quando player troca arma.
    // 0x67D6194 (base Player) nunca é chamado diretamente.
    {
        void* addr = (void*)(il2cpp_base + OFF_SwapWeapon_PlayerNetwork);
        int r = DobbyHook(addr, (void*)Hook_SwapWeapon,
                          (void**)&orig_SwapWeapon);
        if (r == 0) {
            LOGI("Dobby SwapWeapon (PlayerNetwork) OK: orig=%p", orig_SwapWeapon);
            hookLogWrite("Dobby SwapWeapon: orig=%p", orig_SwapWeapon);
        } else {
            LOGE("Dobby SwapWeapon FALHOU (r=%d)", r);
            hookLogWrite("ERRO: Dobby SwapWeapon r=%d", r);
        }
    }

    // ── Hook inline (Dobby) para metodos nao-virtuais (v53)
    hookLogWrite("Instalando hooks Dobby (metodos nao-virtuais)...");
    // ── NOTA: GetWeaponRunSpeedScale, GetScatterRate, get_IsAmmoFree,
    // get_FSModeUseMedikitFasterRate, get_InSwapWeaponCD e IsMoving sao
    // metodos NAO-VIRTUAIS no IL2CPP. O VmtHook (patch em methodInfo->methodPointer)
    // NAO funciona para eles porque o codigo AOT os chama diretamente.
    // DobbyHook faz inline hook no endereco real da funcao — intercepta todos os callers.

    // Speed hack: PlayerAttributes::GetWeaponRunSpeedScale(int)
    {
        void* addr = (void*)(il2cpp_base + OFF_GetWeaponRunSpeedScale);
        int r = DobbyHook(addr, (void*)Hook_GetWeaponRunSpeedScale,
                          (void**)&orig_GetWeaponRunSpeedScale);
        if (r == 0) {
            LOGI("Dobby GetWeaponRunSpeedScale OK: orig=%p", orig_GetWeaponRunSpeedScale);
            hookLogWrite("Dobby GetWeaponRunSpeedScale: orig=%p", orig_GetWeaponRunSpeedScale);
        } else {
            LOGE("Dobby GetWeaponRunSpeedScale FALHOU (r=%d)", r);
            hookLogWrite("ERRO: Dobby GetWeaponRunSpeedScale r=%d", r);
        }
    }

    // Recoil: PlayerAttributes::GetScatterRate()
    {
        void* addr = (void*)(il2cpp_base + OFF_GetScatterRate);
        int r = DobbyHook(addr, (void*)Hook_GetScatterRate,
                          (void**)&orig_GetScatterRate);
        if (r == 0) {
            LOGI("Dobby GetScatterRate OK: orig=%p", orig_GetScatterRate);
            hookLogWrite("Dobby GetScatterRate: orig=%p", orig_GetScatterRate);
        } else {
            LOGE("Dobby GetScatterRate FALHOU (r=%d)", r);
            hookLogWrite("ERRO: Dobby GetScatterRate r=%d", r);
        }
    }

    // Munição infinita: PlayerAttributes::get_IsAmmoFree()
    {
        void* addr = (void*)(il2cpp_base + OFF_get_IsAmmoFree);
        int r = DobbyHook(addr, (void*)Hook_GetIsAmmoFree,
                          (void**)&orig_get_IsAmmoFree);
        if (r == 0) {
            LOGI("Dobby get_IsAmmoFree OK: orig=%p", orig_get_IsAmmoFree);
            hookLogWrite("Dobby get_IsAmmoFree: orig=%p", orig_get_IsAmmoFree);
        } else {
            LOGE("Dobby get_IsAmmoFree FALHOU (r=%d)", r);
            hookLogWrite("ERRO: Dobby get_IsAmmoFree r=%d", r);
        }
    }

    // Medkit rapido: PlayerAttributes::get_FSModeUseMedikitFasterRate()
    {
        void* addr = (void*)(il2cpp_base + OFF_get_FSModeUseMedikitFasterRate);
        int r = DobbyHook(addr, (void*)Hook_GetFSModeUseMedikitFasterRate,
                          (void**)&orig_get_FSModeUseMedikitFasterRate);
        if (r == 0) {
            LOGI("Dobby get_FSModeUseMedikitFasterRate OK: orig=%p", orig_get_FSModeUseMedikitFasterRate);
            hookLogWrite("Dobby get_FSModeUseMedikitFasterRate: orig=%p", orig_get_FSModeUseMedikitFasterRate);
        } else {
            LOGE("Dobby get_FSModeUseMedikitFasterRate FALHOU (r=%d)", r);
            hookLogWrite("ERRO: Dobby get_FSModeUseMedikitFasterRate r=%d", r);
        }
    }

    // Troca rapida de arma: Player::get_InSwapWeaponCD()
    {
        void* addr = (void*)(il2cpp_base + OFF_get_InSwapWeaponCD);
        int r = DobbyHook(addr, (void*)Hook_GetInSwapWeaponCD,
                          (void**)&orig_get_InSwapWeaponCD);
        if (r == 0) {
            LOGI("Dobby get_InSwapWeaponCD OK: orig=%p", orig_get_InSwapWeaponCD);
            hookLogWrite("Dobby get_InSwapWeaponCD: orig=%p", orig_get_InSwapWeaponCD);
        } else {
            LOGE("Dobby get_InSwapWeaponCD FALHOU (r=%d)", r);
            hookLogWrite("ERRO: Dobby get_InSwapWeaponCD r=%d", r);
        }
    }

    // Medkit andando (backup): Player::IsMoving()
    {
        void* addr = (void*)(il2cpp_base + OFF_IsMoving);
        int r = DobbyHook(addr, (void*)Hook_IsMoving,
                          (void**)&orig_IsMoving);
        if (r == 0) {
            LOGI("Dobby IsMoving OK: orig=%p", orig_IsMoving);
            hookLogWrite("Dobby IsMoving: orig=%p", orig_IsMoving);
        } else {
            LOGE("Dobby IsMoving FALHOU (r=%d)", r);
            hookLogWrite("ERRO: Dobby IsMoving r=%d", r);
        }
    }

    // Medkit andando (principal): Player::get_CanMedkitOnMove()
    {
        void* addr = (void*)(il2cpp_base + OFF_get_CanMedkitOnMove);
        int r = DobbyHook(addr, (void*)Hook_GetCanMedkitOnMove,
                          (void**)&orig_get_CanMedkitOnMove);
        if (r == 0) {
            LOGI("Dobby get_CanMedkitOnMove OK: orig=%p", orig_get_CanMedkitOnMove);
            hookLogWrite("Dobby get_CanMedkitOnMove: orig=%p", orig_get_CanMedkitOnMove);
        } else {
            LOGE("Dobby get_CanMedkitOnMove FALHOU (r=%d)", r);
            hookLogWrite("ERRO: Dobby get_CanMedkitOnMove r=%d", r);
        }
    }

    // Medkit andando (nuclear): Player::CancelPreparation() — impede cancelamento direto
    {
        void* addr = (void*)(il2cpp_base + OFF_CancelPreparation);
        int r = DobbyHook(addr, (void*)Hook_CancelPreparation,
                          (void**)&orig_CancelPreparation);
        if (r == 0) {
            LOGI("Dobby CancelPreparation OK: orig=%p", orig_CancelPreparation);
            hookLogWrite("Dobby CancelPreparation: orig=%p", orig_CancelPreparation);
        } else {
            LOGE("Dobby CancelPreparation FALHOU (r=%d)", r);
            hookLogWrite("ERRO: Dobby CancelPreparation r=%d", r);
        }
    }

    // Medkit rapido (principal): PlayerAttributes::get_EatSpeedScale()
    {
        void* addr = (void*)(il2cpp_base + OFF_get_EatSpeedScale);
        int r = DobbyHook(addr, (void*)Hook_GetEatSpeedScale,
                          (void**)&orig_get_EatSpeedScale);
        if (r == 0) {
            LOGI("Dobby get_EatSpeedScale OK: orig=%p", orig_get_EatSpeedScale);
            hookLogWrite("Dobby get_EatSpeedScale: orig=%p", orig_get_EatSpeedScale);
        } else {
            LOGE("Dobby get_EatSpeedScale FALHOU (r=%d)", r);
            hookLogWrite("ERRO: Dobby get_EatSpeedScale r=%d", r);
        }
    }

    // Anti-recoil adicional: PlayerAttributes::get_SkillScatterRate()
    {
        void* addr = (void*)(il2cpp_base + OFF_get_SkillScatterRate);
        int r = DobbyHook(addr, (void*)Hook_get_SkillScatterRate,
                          (void**)&orig_get_SkillScatterRate);
        if (r == 0) {
            LOGI("Dobby get_SkillScatterRate OK: orig=%p", orig_get_SkillScatterRate);
            hookLogWrite("Dobby get_SkillScatterRate: orig=%p", orig_get_SkillScatterRate);
        } else {
            LOGE("Dobby get_SkillScatterRate FALHOU (r=%d)", r);
            hookLogWrite("ERRO: Dobby get_SkillScatterRate r=%d", r);
        }
    }

    // Anti-recoil adicional: PlayerAttributes::get_SkillScatterRateSighting()
    {
        void* addr = (void*)(il2cpp_base + OFF_get_SkillScatterRateSighting);
        int r = DobbyHook(addr, (void*)Hook_get_SkillScatterRateSighting,
                          (void**)&orig_get_SkillScatterRateSighting);
        if (r == 0) {
            LOGI("Dobby get_SkillScatterRateSighting OK: orig=%p", orig_get_SkillScatterRateSighting);
            hookLogWrite("Dobby get_SkillScatterRateSighting: orig=%p", orig_get_SkillScatterRateSighting);
        } else {
            LOGE("Dobby get_SkillScatterRateSighting FALHOU (r=%d)", r);
            hookLogWrite("ERRO: Dobby get_SkillScatterRateSighting r=%d", r);
        }
    }

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
