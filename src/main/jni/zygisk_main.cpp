/*
 * ============================================================
 * zygisk_main.cpp - Entry point do módulo Zygisk para JawMods
 * ============================================================
 *
 * VANTAGENS sobre ptrace injection:
 *   - Carregado pelo Magisk/Zygisk ANTES do anti-cheat inicializar
 *   - .so não precisa estar no game dir (sem arquivo externo detectável)
 *   - TracerPid = 0 (sem ptrace)
 *   - Aparece como mapeamento Magisk no /proc/maps (pode ser ocultado
 *     pelo próprio Magisk com DenyList)
 *
 * FLUXO:
 *   Zygote fork → postAppSpecialize(nice_name=com.dts.freefireth)
 *   → pthread_create(hack_thread) → aguarda libil2cpp.so
 *   → VMT hook → SharedMemory → Overlay lê e desenha
 * ============================================================
 */

#include "zygisk.hpp"
#include <pthread.h>
#include <cstring>
#include <unistd.h>
#include <android/log.h>

// hack_thread é definido em GameHook.cpp (mesmo módulo compilado junto)
extern void* hack_thread(void*);
extern bool  g_hookStarted;

#define ZTAG "JawMods-Zygisk"
// Em release não loga nada — mesmo conceito do STEALTH_DEBUG
#ifdef STEALTH_DEBUG
  #define ZLOGI(...) __android_log_print(ANDROID_LOG_INFO,  ZTAG, __VA_ARGS__)
  #define ZLOGE(...) __android_log_print(ANDROID_LOG_ERROR, ZTAG, __VA_ARGS__)
#else
  #define ZLOGI(...) ((void)0)
  #define ZLOGE(...) ((void)0)
#endif

// ============================================================
// Módulo Zygisk
// ============================================================
class JawModsModule : public zygisk::ModuleBase {

public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (!args || !args->nice_name) return;

        // Pegar o nome do processo que acabou de ser especializado
        const char *process = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!process) return;

        bool isTarget = (strstr(process, "freefireth") != nullptr ||
                         strstr(process, "freefire")   != nullptr);

        env->ReleaseStringUTFChars(args->nice_name, process);

        if (!isTarget) return;

        ZLOGI("postAppSpecialize: target process detectado, iniciando hook...");

        if (g_hookStarted) return;
        g_hookStarted = true;

        // Lançar hack_thread — mesmo que no modo ptrace
        // hack_thread aguarda libil2cpp.so aparecer nos maps e aplica o hook
        pthread_t t;
        pthread_create(&t, nullptr, hack_thread, nullptr);
        pthread_detach(t);
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv      *env = nullptr;
};

// Registrar o módulo (expõe zygisk_module_entry como símbolo público)
REGISTER_ZYGISK_MODULE(JawModsModule)
