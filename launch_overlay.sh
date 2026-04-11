#!/system/bin/sh
# ============================================================
# launch.sh - Script para injetar o hook + iniciar overlay
# ============================================================
#
# Uso:
#   adb push launch.sh /data/local/tmp/
#   adb push libHook.so /data/local/tmp/
#   adb shell su -c "sh /data/local/tmp/launch.sh"
#
# Fluxo:
#   1. Configura SELinux permissive
#   2. Aguarda o jogo abrir e libil2cpp.so carregar
#   3. Injeta libHook.so no processo do jogo (VMT hook)
#   4. Inicia o overlay APK (lê SharedMemory, desenha ImGui)
#
# Arquitetura:
#   [Jogo] ←── libHook.so (VMT hook → SharedMemory)
#   [Overlay APK] ←── SharedMemory → ImGui draw
#
# Pré-requisitos:
#   - APK instalado
#   - libHook.so em /data/local/tmp/
#   - Root access
# ============================================================

PACKAGE="com.android.support"
GAME_PACKAGE="com.fungames.sniper3d"
HOOK_LIB="/data/local/tmp/libHook.so"

echo "========================================="
echo " HYBRID MOD - VMT Hook + External Overlay"
echo "========================================="
echo "[*] Jogo:    $GAME_PACKAGE"
echo "[*] Overlay: $PACKAGE"
echo ""

# ── Verifica root ──
if [ "$(id -u)" != "0" ]; then
    echo "[!] Este script precisa ser executado como root"
    echo "[!] Use: su -c 'sh $0'"
    exit 1
fi

# ── SELinux permissive ──
SELINUX_MODE=$(getenforce 2>/dev/null)
if [ "$SELINUX_MODE" = "Enforcing" ]; then
    echo "[*] SELinux: Enforcing -> Permissive"
    setenforce 0
fi

# ── Verifica libHook.so ──
if [ ! -f "$HOOK_LIB" ]; then
    echo "[!] libHook.so não encontrada em $HOOK_LIB"
    echo "[!] Faça: adb push libs/arm64-v8a/libHook.so /data/local/tmp/"
    exit 1
fi
chmod 755 "$HOOK_LIB"

# ── Limpar shared memory anterior ──
rm -f /data/local/tmp/.esp_shm 2>/dev/null

# ── Aguardar jogo abrir ──
echo "[*] Aguardando $GAME_PACKAGE iniciar..."
GAME_PID=0
while [ "$GAME_PID" = "0" ] || [ -z "$GAME_PID" ]; do
    GAME_PID=$(pidof "$GAME_PACKAGE" 2>/dev/null)
    if [ -z "$GAME_PID" ]; then
        GAME_PID=0
    fi
    sleep 1
done
echo "[+] Jogo detectado: PID $GAME_PID"

# ── Aguardar libil2cpp.so carregar ──
echo "[*] Aguardando libil2cpp.so carregar..."
IL2CPP_LOADED=0
for i in $(seq 1 30); do
    if grep -q "libil2cpp.so" /proc/$GAME_PID/maps 2>/dev/null; then
        IL2CPP_LOADED=1
        break
    fi
    sleep 1
done

if [ "$IL2CPP_LOADED" = "0" ]; then
    echo "[!] Timeout: libil2cpp.so não carregou em 30s"
    exit 1
fi
echo "[+] libil2cpp.so carregada"
sleep 2  # Esperar assemblies carregarem

# ── Copiar libHook.so para o diretório do jogo ──
# Necessário para o dlopen funcionar dentro do processo do jogo
GAME_DIR="/data/data/$GAME_PACKAGE"
HOOK_DEST="$GAME_DIR/libHook.so"
cp "$HOOK_LIB" "$HOOK_DEST"
chmod 755 "$HOOK_DEST"
chown $(stat -c '%U:%G' "$GAME_DIR") "$HOOK_DEST" 2>/dev/null

# ── Injetar libHook.so via am broadcast + runtime exec ──
# Método: usar /proc/<pid>/mem ou linker injection
# Aqui usamos o método mais simples: inject via ptrace-free dlopen

echo "[*] Injetando libHook.so no PID $GAME_PID..."

# Método de injeção: usar /proc/<pid>/cmdline trick + LD_PRELOAD
# Se você tem um injector customizado, substitua aqui:
#   ./injector $GAME_PID $HOOK_DEST

# Método alternativo usando linker namespace (Android 7+):
# Cria um script que o zygote vai executar
# Opção mais confiável: use um injector como libinject ou cmdline-inject

# ── MÉTODO SIMPLES: Matar e reiniciar jogo com LD_PRELOAD ──
# (Funciona mas reinicia o jogo)
# Descomente SE seu injector customizado não funcionar:

# echo "[*] Reiniciando jogo com hook..."
# am force-stop "$GAME_PACKAGE"
# sleep 1
# LD_PRELOAD="$HOOK_DEST" am start -n "$GAME_PACKAGE/.MainActivity" 2>/dev/null

# ── MÉTODO PTRACE (sem reiniciar, precisa de injector) ──
# Se você tem um injector (ptrace + dlopen), use aqui:
# /data/local/tmp/injector $GAME_PID "$HOOK_DEST"

# ── MÉTODO MEMFD (sem tocar disco dentro do jogo) ──
# Alternativa avançada que carrega a lib via memfd_create
# Requer injector com suporte a memfd

echo ""
echo "[!] IMPORTANTE: Escolha UM método de injeção acima e descomente."
echo "[!] O mais simples é LD_PRELOAD (reinicia jogo)."
echo "[!] O mais stealth é ptrace + dlopen (sem reiniciar)."
echo ""
echo "[*] Se o hook estiver funcionando, /data/local/tmp/.esp_shm aparece."
echo ""

# ── Garantir permissão overlay ──
appops set "$PACKAGE" SYSTEM_ALERT_WINDOW allow 2>/dev/null

# ── Verificar APK instalado ──
pm path "$PACKAGE" > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "[!] APK overlay não encontrado: $PACKAGE"
    echo "[!] Instale o APK primeiro"
    exit 1
fi

# ── Iniciar overlay ──
echo "[*] Iniciando overlay..."
am start -n "$PACKAGE/.MainActivity" 2>/dev/null

echo ""
echo "========================================="
echo " STATUS"
echo "========================================="
echo "[+] Jogo:    PID $GAME_PID"
echo "[+] Overlay: Iniciado"
echo "[+] Hook:    libHook.so (VMT)"
echo "[+] IPC:     /data/local/tmp/.esp_shm"
echo "[+] Overlay: FLAG_SECURE (captura bloqueada)"
echo ""
echo "[*] Para verificar se o hook funciona:"
echo "    ls -la /data/local/tmp/.esp_shm"
echo ""
echo "[*] Para parar tudo:"
echo "    am force-stop $PACKAGE"
echo "    rm /data/local/tmp/.esp_shm"
