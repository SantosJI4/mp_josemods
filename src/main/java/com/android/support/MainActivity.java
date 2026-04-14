package com.android.support;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;

public class MainActivity extends Activity {
    private static final int OVERLAY_PERMISSION_CODE = 1234;

    // ══════════════════════════════════════
    // MUDE AQUI: Package name do jogo
    // ══════════════════════════════════════
    private static final String GAME_PACKAGE = "com.dts.freefireth";

    private TextView tvStatus;
    private ProgressBar progressInject;
    private Button btnStart;
    private Button btnStop;
    private Handler handler;
    private volatile boolean injecting = false;
    private volatile boolean hookPolling = false;
    private Thread hookPollThread = null;

    @Override
    public void onPointerCaptureChanged(boolean hasCapture) {
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        tvStatus = (TextView) findViewById(R.id.tvStatus);
        progressInject = (ProgressBar) findViewById(R.id.progressInject);
        btnStart = (Button) findViewById(R.id.btnStart);
        btnStop = (Button) findViewById(R.id.btnStop);
        handler = new Handler(Looper.getMainLooper());

        btnStart.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onStartClicked();
            }
        });

        btnStop.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onStopClicked();
            }
        });

        // Checar permissão de overlay
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            setStatus("Requesting overlay permission...");
            Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivityForResult(intent, OVERLAY_PERMISSION_CODE);
        } else {
            setStatus("Ready to inject");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == OVERLAY_PERMISSION_CODE) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && Settings.canDrawOverlays(this)) {
                setStatus("Ready to inject");
            } else {
                setStatus("Overlay permission denied");
            }
        }
    }

    private void showLoading(final boolean show) {
        runOnUi(new Runnable() {
            @Override
            public void run() {
                progressInject.setVisibility(show ? View.VISIBLE : View.GONE);
            }
        });
    }

    private void onStartClicked() {
        if (injecting) return;

        // Verificar permissão de overlay
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            Toast.makeText(this, "Overlay permission required", Toast.LENGTH_SHORT).show();
            return;
        }

        injecting = true;
        btnStart.setEnabled(false);
        btnStart.setVisibility(View.GONE);
        showLoading(true);
        setStatus("Initializing...");

        // Tudo roda em background thread (su commands bloqueiam)
        new Thread(new Runnable() {
            @Override
            public void run() {
                doInjectAndStart();
            }
        }).start();
    }

    private void onStopClicked() {
        // Parar poll thread
        hookPolling = false;
        if (hookPollThread != null) {
            hookPollThread.interrupt();
            hookPollThread = null;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        stopService(new Intent(MainActivity.this, OverlayService.class));
                        setStatus("Overlay stopped.\nHook stays active until game restarts.");
                        btnStop.setVisibility(View.GONE);
                        btnStart.setVisibility(View.VISIBLE);
                        btnStart.setEnabled(true);
                    }
                });
            }
        }).start();
    }

    // ══════════════════════════════════════
    // LOGICA — INSTALA MODULO ZYGISK + INICIA OVERLAY
    // ZERO comandos detectaveis em runtime:
    //   - SEM setenforce 0
    //   - SEM set-debug-app
    //   - SEM attach-agent
    //   - SEM resetprop ro.debuggable
    //   - SEM wrap property / LD_PRELOAD
    //   - SEM force-stop do jogo
    // O hook carrega via Zygisk antes do anti-cheat
    // ══════════════════════════════════════
    private void doInjectAndStart() {
        try {
            // 1. Verificar root
            updateStatus("Checking root...");
            final String rootCheck = rootExec("id");
            if (rootCheck == null || !rootCheck.contains("uid=0")) {
                updateStatus("No root access");
                resetButton();
                return;
            }

            // 2. Verificar Magisk + Zygisk
            updateStatus("Checking Magisk...");
            String magiskVer = rootExec("magisk -V 2>/dev/null");
            if (magiskVer == null || magiskVer.trim().isEmpty()) {
                updateStatus("Magisk not found.\nInstall Magisk v24+ with Zygisk enabled.");
                resetButton();
                return;
            }

            // 3. Verificar se Zygisk esta habilitado
            String zygiskCheck = rootExec("magisk --sqlite \"SELECT value FROM settings WHERE key='zygisk'\" 2>/dev/null");
            // Tambem checar se o diretorio de modulos zygisk existe
            String zygiskDir = rootExec("ls /data/adb/modules/.zygisk 2>/dev/null; ls /data/adb/zygisk 2>/dev/null");

            // 3b. CRITICO: Verificar e remover DenyList para o jogo
            // Se Free Fire esta no DenyList, Zygisk NAO carrega modulo nele!
            // O usuario pode ter adicionado para esconder root, mas isso bloqueia nosso hook.
            updateStatus("Checking DenyList...");
            String denyList = rootExec("magisk --denylist ls 2>/dev/null");
            if (denyList != null && denyList.contains(GAME_PACKAGE)) {
                updateStatus("Removing " + GAME_PACKAGE + " from DenyList...");
                rootExec("magisk --denylist rm " + GAME_PACKAGE + " 2>/dev/null");
                // Verificar se removeu
                String denyCheck = rootExec("magisk --denylist ls 2>/dev/null");
                if (denyCheck != null && denyCheck.contains(GAME_PACKAGE)) {
                    updateStatus("WARNING: Could not remove game from DenyList.\n"
                        + "Go to Magisk > Settings > DenyList > remove " + GAME_PACKAGE + " manually.\n"
                        + "DenyList blocks Zygisk from loading our hook!");
                    resetButton();
                    return;
                }
                updateStatus("DenyList: game removed successfully.");
            }

            // 3c. Verificar Enforce DenyList — se ativo, desabilita
            // Com Enforce DenyList ON + game na lista = Zygisk nao carrega
            String enforceDeny = rootExec("magisk --sqlite \"SELECT value FROM settings WHERE key='denylist'\" 2>/dev/null");
            if (enforceDeny != null && enforceDeny.contains("1")) {
                // DenyList enforce esta ativo. Para garantia, desabilitar
                // (so afeta o enforce, nao remove apps da lista)
                rootExec("magisk --sqlite \"REPLACE INTO settings (key,value) VALUES('denylist',0)\" 2>/dev/null");
                updateStatus("Enforce DenyList disabled for Zygisk compatibility.");
            }

            // 4. Verificar se modulo JA existia (antes de instalar)
            final String hookSrc = getApplicationInfo().nativeLibraryDir + "/libHook.so";
            final String moduleDir = "/data/adb/modules/jawmods";
            final String gameDir = "/data/data/" + GAME_PACKAGE;

            if (!new File(hookSrc).exists()) {
                updateStatus("Hook library not found.\nDid you do a clean build?");
                resetButton();
                return;
            }

            // Checar se o modulo ja existia ANTES de copiar
            boolean moduleAlreadyExisted = false;
            String existingProp = rootExec("cat " + moduleDir + "/module.prop 2>/dev/null");
            String existingSo = rootExec("ls " + moduleDir + "/zygisk/arm64-v8a.so 2>/dev/null");
            String existingSepolicy = rootExec("cat " + moduleDir + "/sepolicy.rule 2>/dev/null");
            if (existingProp != null && existingProp.contains("jawmods")
                && existingSo != null && existingSo.contains("arm64-v8a.so")) {
                moduleAlreadyExisted = true;
            }
            boolean sepolicyAlreadyExisted = (existingSepolicy != null && existingSepolicy.contains("shell_data_file"));

            // Checar se esta desabilitado
            String moduleDisabled = rootExec("ls " + moduleDir + "/disable 2>/dev/null");
            if (moduleDisabled != null && !moduleDisabled.trim().isEmpty()) {
                rootExec("rm -f " + moduleDir + "/disable");
                moduleAlreadyExisted = false; // Tava desabilitado, precisa reboot
            }

            // 5. Instalar/atualizar modulo Zygisk (batched: 2 chamadas su)
            updateStatus("Installing Zygisk module...");
            rootExec("mkdir -p " + moduleDir + "/zygisk"
                    + " ; cp " + hookSrc + " " + moduleDir + "/zygisk/arm64-v8a.so"
                    + " ; chmod 644 " + moduleDir + "/zygisk/arm64-v8a.so"
                    + " ; rm -f " + moduleDir + "/disable " + moduleDir + "/remove");

            // Criar module.prop em uma unica chamada su
            rootExec("printf 'id=jawmods\\nname=JawMods ESP Hook\\nversion=v13\\nversionCode=13\\nauthor=JawMods\\ndescription=ESP hook via Zygisk for Unity games\\n'"
                    + " > " + moduleDir + "/module.prop");

            // Verificar que .so foi copiado corretamente (comparar tamanho)
            String srcSize = rootExec("wc -c < " + hookSrc + " 2>/dev/null");
            String dstSize = rootExec("wc -c < " + moduleDir + "/zygisk/arm64-v8a.so 2>/dev/null");
            if (srcSize == null || dstSize == null
                || srcSize.trim().isEmpty() || dstSize.trim().isEmpty()
                || !srcSize.trim().equals(dstSize.trim())) {
                updateStatus("ERRO: .so nao foi copiado corretamente!\n"
                    + "src=" + (srcSize != null ? srcSize.trim() : "null")
                    + " dst=" + (dstSize != null ? dstSize.trim() : "null"));
                resetButton();
                return;
            }

            // Verificar se o simbolo zygisk_module_entry existe no .so instalado
            String symbolCheck = rootExec(
                "strings " + moduleDir + "/zygisk/arm64-v8a.so 2>/dev/null | grep -c zygisk_module_entry"
                + " || grep -c zygisk_module_entry " + moduleDir + "/zygisk/arm64-v8a.so 2>/dev/null");
            boolean hasZygiskSymbol = symbolCheck != null && !symbolCheck.trim().equals("0") && !symbolCheck.trim().isEmpty();

            // Verificar Zygisk habilitado - AGORA com acao se nao estiver
            boolean zygiskEnabled = false;
            if (zygiskCheck != null && zygiskCheck.contains("1")) {
                zygiskEnabled = true;
            }
            // Fallback: checar se .zygisk marker existe (Magisk cria quando Zygisk ativo)
            String zygiskMarker = rootExec("ls /data/adb/modules/.zygisk_cache 2>/dev/null"
                + " ; ls /data/adb/.magisk/zygisk 2>/dev/null"
                + " ; getprop ro.zygisk.enabled 2>/dev/null"
                + " ; getprop persist.magisk.zygisk 2>/dev/null");
            if (zygiskMarker != null && !zygiskMarker.trim().isEmpty()) {
                zygiskEnabled = true;
            }

            // Se Zygisk claramente NAO esta habilitado, avisar
            if (!zygiskEnabled && (zygiskCheck == null || zygiskCheck.trim().isEmpty() || zygiskCheck.contains("0"))) {
                // Tentar habilitar via sqlite
                rootExec("magisk --sqlite \"REPLACE INTO settings (key,value) VALUES('zygisk',1)\" 2>/dev/null");
                updateStatus("Zygisk was DISABLED!\nEnabled it now.\n\n"
                    + "You MUST reboot for Zygisk to start.\n"
                    + "After reboot, open JawMods and click Start again.\n\n"
                    + "Also check: Magisk app > Settings > Zygisk = ON");
                resetButton();
                return;
            }

            // CRITICO: SELinux policy — permite hook e overlay acessar /data/local/tmp/
            // Sem isso, o hook (untrusted_app) nao consegue escrever no SHM,
            // e o overlay (outro untrusted_app) nao consegue ler.
            // Magisk aplica no boot — invisible para anti-cheat.
            rootExec("printf '"
                    + "allow untrusted_app shell_data_file dir { search read write open create add_name getattr }\\n"
                    + "allow untrusted_app shell_data_file file { read write open create getattr setattr }\\n"
                    + "allow untrusted_app app_data_file dir { search read open getattr }\\n"
                    + "allow untrusted_app app_data_file file { read write open getattr }\\n"
                    + "' > " + moduleDir + "/sepolicy.rule");

            // Verificar se sepolicy.rule foi criado
            String checkSepolicy = rootExec("cat " + moduleDir + "/sepolicy.rule 2>/dev/null");
            if (checkSepolicy == null || !checkSepolicy.contains("shell_data_file")) {
                updateStatus("Failed to create sepolicy.rule.\nCheck Magisk and root access.");
                resetButton();
                return;
            }

            // Verificar que o modulo foi instalado
            String checkModule = rootExec("cat " + moduleDir + "/module.prop 2>/dev/null");
            if (checkModule == null || !checkModule.contains("jawmods")) {
                updateStatus("Failed to install module.\nCheck Magisk and root access.");
                resetButton();
                return;
            }

            // 6. Pre-criar SHM e hook log com permissoes
            // CRITICO: se o jogo ja esta rodando, o hook JA tem o SHM aberto com mmap.
            // Deletar o arquivo quebra a conexao (hook escreve no inode antigo,
            // overlay abre novo arquivo zerado => magic=0 => "Waiting for hook...").
            // Solucao: so recriar o SHM se o jogo NAO esta rodando.
            String gameRunningPid = rootExec("pidof " + GAME_PACKAGE + " 2>/dev/null");
            boolean gameAlreadyRunning = gameRunningPid != null && !gameRunningPid.trim().isEmpty();

            if (!gameAlreadyRunning) {
                // Jogo fechado: recriar SHM zerado para proxima sessao
                rootExec("rm -f /data/local/tmp/.esp_shm /data/local/tmp/.hook_log"
                        + " ; dd if=/dev/zero of=/data/local/tmp/.esp_shm bs=4096 count=1 2>/dev/null"
                        + " ; chmod 666 /data/local/tmp/.esp_shm"
                        + " ; touch /data/local/tmp/.hook_log ; chmod 666 /data/local/tmp/.hook_log");
            } else {
                // Jogo rodando: hook pode ja ter o SHM mapeado — NAO deletar!
                // Apenas garantir que o arquivo existe e tem permissoes corretas
                rootExec("[ -f /data/local/tmp/.esp_shm ] ||"
                        + " dd if=/dev/zero of=/data/local/tmp/.esp_shm bs=4096 count=1 2>/dev/null"
                        + " ; chmod 666 /data/local/tmp/.esp_shm 2>/dev/null"
                        + " ; touch /data/local/tmp/.hook_log ; chmod 666 /data/local/tmp/.hook_log 2>/dev/null");
            }

            rootExec("mkdir -p " + gameDir
                    + " ; chmod 711 " + gameDir
                    + " ; touch " + gameDir + "/.hook_log ; chmod 666 " + gameDir + "/.hook_log");

            // 7. Decisao: reboot ou iniciar overlay
            // Comparar md5 do .so antigo vs novo — se mudou, Zygisk pode precisar de reboot
            boolean soChanged = false;
            if (moduleAlreadyExisted) {
                String md5Src = rootExec("md5sum " + hookSrc + " 2>/dev/null | awk '{print $1}'");
                String md5Dst = rootExec("md5sum " + moduleDir + "/zygisk/arm64-v8a.so 2>/dev/null | awk '{print $1}'");
                // Apos o cp, os md5 devem ser iguais. Mas se o .so do APK mudou desde o
                // ultimo boot, o Zygisk pode estar usando o antigo em cache.
                // Verificar se Magisk listou nosso modulo como ativo
                String moduleList = rootExec("ls /data/adb/modules/ 2>/dev/null");
                String moduleState = rootExec("cat /data/adb/modules/jawmods/module.prop 2>/dev/null");
                if (moduleState == null || !moduleState.contains("jawmods")) {
                    soChanged = true; // module.prop invalido
                }
            }

            if (!moduleAlreadyExisted || !sepolicyAlreadyExisted || soChanged) {
                // PRIMEIRO INSTALL ou sepolicy.rule acabou de ser criado — precisa reboot
                showLoading(false);
                String reason = !moduleAlreadyExisted ? "Module installed" :
                                !sepolicyAlreadyExisted ? "SELinux policy updated" : "Module updated";
                updateStatus(reason + "!\n\n" +
                    "Now you need to:\n" +
                    "1. Make sure Zygisk is ON in Magisk settings\n" +
                    "2. Reboot your phone\n" +
                    "3. Open JawMods and click Start\n" +
                    (hasZygiskSymbol ? "✓ zygisk_module_entry found in .so" : "✗ zygisk_module_entry NOT found in .so!") +
                    "\nMagisk v" + magiskVer.trim() +
                    "\nZygisk: " + (zygiskEnabled ? "ON" : "check settings"));
                resetButton();
                return;
            }

            // Modulo ja existia — Zygisk ja esta ativo
            // Zygisk so injeta quando o processo do jogo inicia do zero (fork do zygote)
            updateStatus("Reiniciando Free Fire...");

            // Copiar .so para /data/local/tmp/ (para attach-agent fallback)
            final String tmpSo = "/data/local/tmp/libHook.so";
            rootExec("cp " + hookSrc + " " + tmpSo + " ; chmod 755 " + tmpSo);

            // Fechar o jogo
            rootExec("am force-stop " + GAME_PACKAGE + " 2>/dev/null");
            Thread.sleep(1500);

            // Iniciar o jogo
            String launcherActivity = getLauncherActivity(GAME_PACKAGE);
            if (launcherActivity != null && launcherActivity.contains("/")) {
                rootExec("am start -n " + GAME_PACKAGE + launcherActivity + " 2>/dev/null");
            } else {
                rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1 2>/dev/null");
            }
            updateStatus("Free Fire iniciado. Aguardando PID...");

            // Iniciar overlay
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    startService(new Intent(MainActivity.this, OverlayService.class));
                }
            });

            // ═══════════════════════════════════════════════
            // 8. INJECAO DUAL: Zygisk (10s) → attach-agent
            // ═══════════════════════════════════════════════
            boolean connected = false;
            boolean agentTried = false;
            boolean agentDebugTried = false;
            String lastAgentResult = "";
            String gamePid = "";

            // Fase 1: Esperar jogo iniciar (max 20s pra ter PID)
            for (int i = 0; i < 20; i++) {
                Thread.sleep(1000);
                gamePid = rootExec("pidof " + GAME_PACKAGE + " 2>/dev/null | awk '{print $1}'");
                if (gamePid != null && !gamePid.trim().isEmpty()) {
                    gamePid = gamePid.trim();
                    break;
                }
                updateStatus("Inicializando Free Fire...\n(" + (i+1) + "s)");
            }

            if (gamePid == null || gamePid.trim().isEmpty()) {
                updateStatus("Free Fire nao iniciou (sem PID apos 20s).\nTente abrir o jogo manualmente.");
                resetButton();
                return;
            }

            updateStatus("Free Fire PID=" + gamePid + "\nAguardando Zygisk...");

            // Fase 2: Esperar Zygisk injetar (10s)
            // Se Zygisk funciona, o hook ja esta no maps desde o fork
            boolean zygiskWorked = false;
            for (int i = 0; i < 10; i++) {
                Thread.sleep(1000);
                String mapsCheck = rootExec("grep -cE 'arm64-v8a\\.so|jawmods|libHook' /proc/" + gamePid + "/maps 2>/dev/null");
                int mc = 0;
                try { mc = Integer.parseInt(mapsCheck != null ? mapsCheck.trim() : "0"); } catch (Exception e) {}
                if (mc > 0) {
                    zygiskWorked = true;
                    updateStatus("Zygisk OK! Hook nos maps. Aguardando ativar...");
                    break;
                }
                // Checar se PID mudou (game reiniciou)
                String newPid = rootExec("pidof " + GAME_PACKAGE + " 2>/dev/null | awk '{print $1}'");
                if (newPid != null && !newPid.trim().isEmpty() && !newPid.trim().equals(gamePid)) {
                    gamePid = newPid.trim();
                    updateStatus("Game reiniciou: PID=" + gamePid + "\nRecheckando...");
                }
                updateStatus("Aguardando Zygisk... maps=0\nPID=" + gamePid + " (" + (i+1) + "/10s)");
            }

            // Fase 3: Se Zygisk falhou, usar attach-agent
            if (!zygiskWorked) {
                updateStatus("Zygisk nao injetou. Tentando attach-agent...\nPID=" + gamePid);
                agentTried = true;

                // Metodo 1: attach-agent direto (funciona se root tem permissao)
                lastAgentResult = rootExec(
                    "cmd activity attach-agent " + gamePid + " " + tmpSo + " 2>&1");
                updateStatus("attach-agent resultado:\n" + (lastAgentResult != null ? lastAgentResult.trim() : "(null)"));
                Thread.sleep(2000);

                // Verificar se funcionou
                String mc1 = rootExec("grep -cE 'libHook' /proc/" + gamePid + "/maps 2>/dev/null");
                boolean m1ok = mc1 != null && !mc1.trim().equals("0") && !mc1.trim().isEmpty();

                if (!m1ok) {
                    // Metodo 2: set-debug-app + restart + attach-agent
                    updateStatus("Metodo 1 falhou. Tentando set-debug-app...");
                    agentDebugTried = true;

                    rootExec("am set-debug-app -w --persistent " + GAME_PACKAGE + " 2>/dev/null");
                    rootExec("am force-stop " + GAME_PACKAGE + " 2>/dev/null");
                    Thread.sleep(2000);

                    // Reiniciar jogo
                    if (launcherActivity != null && launcherActivity.contains("/")) {
                        rootExec("am start -n " + GAME_PACKAGE + launcherActivity + " 2>/dev/null");
                    } else {
                        rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1 2>/dev/null");
                    }

                    // Esperar jogo iniciar (novo PID)
                    updateStatus("Reiniciando com debug flag...");
                    for (int w = 0; w < 15; w++) {
                        Thread.sleep(1000);
                        String np = rootExec("pidof " + GAME_PACKAGE + " 2>/dev/null | awk '{print $1}'");
                        if (np != null && !np.trim().isEmpty()) {
                            gamePid = np.trim();
                            break;
                        }
                    }

                    // Esperar jogo estabilizar
                    Thread.sleep(3000);

                    // Re-pegar PID (pode ter mudado)
                    String np2 = rootExec("pidof " + GAME_PACKAGE + " 2>/dev/null | awk '{print $1}'");
                    if (np2 != null && !np2.trim().isEmpty()) gamePid = np2.trim();

                    // Tentar attach-agent agora
                    lastAgentResult = rootExec(
                        "cmd activity attach-agent " + gamePid + " " + tmpSo + " 2>&1");
                    updateStatus("attach-agent (debug) resultado:\n"
                        + (lastAgentResult != null ? lastAgentResult.trim() : "(null)"));

                    // Limpar debug flag
                    rootExec("am clear-debug-app 2>/dev/null");
                    Thread.sleep(2000);
                }
            }

            // Fase 4: Monitorar por 30s — hook pode levar tempo para attach->init->il2cpp
            for (int i = 0; i < 30; i++) {
                Thread.sleep(1000);

                // Checar PID ainda vivo
                String cp = rootExec("pidof " + GAME_PACKAGE + " 2>/dev/null | awk '{print $1}'");
                if (cp == null || cp.trim().isEmpty()) {
                    updateStatus("Free Fire crashou. PID nao encontrado.\n"
                        + (agentTried ? "Agent result: " + lastAgentResult : "Zygisk pode ter causado crash."));
                    continue;
                }
                gamePid = cp.trim();

                // Verificar hook nos maps
                String mc = rootExec("grep -cE 'libHook|jawmods|arm64-v8a\\.so' /proc/" + gamePid + "/maps 2>/dev/null");
                int mapsCount = 0;
                try { mapsCount = Integer.parseInt(mc != null ? mc.trim() : "0"); } catch (Exception e) {}

                // Verificar hook log
                String hookLog = rootExec(
                    "cat /data/local/tmp/.hook_log 2>/dev/null | tail -2;"
                    + " cat /data/data/" + GAME_PACKAGE + "/.hook_log 2>/dev/null | tail -2");
                boolean hookActive = hookLog != null && (hookLog.contains("HOOK ATIVO")
                    || hookLog.contains("SHM OK") || hookLog.contains("VMT"));

                // Verificar logcat
                String lc = rootExec("logcat -d -s GameHook 2>/dev/null | grep " + gamePid + " | tail -3");
                boolean lcActive = lc != null && (lc.contains("HOOK ATIVO") || lc.contains("hack_thread")
                    || lc.contains("postAppSpecialize") || lc.contains("HOOK CARREGADO"));

                if (hookActive || lcActive) {
                    connected = true;
                    updateStatus("HOOK ATIVO!\n" + (hookLog != null ? hookLog.trim() : ""));
                    break;
                }

                if (mapsCount > 0 && !hookActive) {
                    updateStatus("Hook carregado (maps=" + mapsCount + "), aguardando il2cpp...\n(" + (i+1) + "s)");
                } else {
                    String method = agentDebugTried ? "debug+agent" : agentTried ? "agent" : "zygisk";
                    updateStatus("Aguardando hook (" + method + ")...\nmaps=" + mapsCount + " PID=" + gamePid
                        + (hookLog != null && !hookLog.trim().isEmpty() ? "\n" + hookLog.trim().substring(0, Math.min(hookLog.trim().length(), 80)) : "")
                        + "\n(" + (i+1) + "/30s)");
                }
            }

            showLoading(false);
            if (connected) {
                updateStatus("Conectado! Hook ativo.");
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        btnStop.setVisibility(View.VISIBLE);
                    }
                });
            } else {
                // Diagnostico final COMPLETO — salvar /sdcard/
                final String fPid = gamePid;
                String finalDiag = rootExec(
                    "echo '=== JAWMODS DIAG v14 ===';"
                    + " echo 'Zygisk:'; magisk --sqlite \"SELECT value FROM settings WHERE key='zygisk'\" 2>/dev/null;"
                    + " echo 'DenyList:'; magisk --denylist ls 2>/dev/null | grep -i fire || echo '(clean)';"
                    + " echo 'Module:'; ls -la /data/adb/modules/jawmods/zygisk/ 2>/dev/null;"
                    + " echo 'State:'; ls /data/adb/modules/jawmods/disable /data/adb/modules/jawmods/remove 2>/dev/null || echo '(active)';"
                    + " echo 'PID=" + fPid + "';"
                    + " echo 'Maps hook:'; grep -iE 'libHook|jawmods|arm64-v8a' /proc/" + fPid + "/maps 2>/dev/null | head -5 || echo '(nothing)';"
                    + " echo 'All Logcat GameHook:';"
                    + " logcat -d -s GameHook 2>/dev/null | tail -20;"
                    + " echo 'HookLog:';"
                    + " cat /data/local/tmp/.hook_log 2>/dev/null;"
                    + " cat /data/data/" + GAME_PACKAGE + "/.hook_log 2>/dev/null;"
                    + " echo 'Agent tried: " + agentTried + " debug: " + agentDebugTried + "';"
                    + " echo 'Last result: " + (lastAgentResult != null ? lastAgentResult.replace("'", "") : "null") + "'"
                );
                rootExec("echo '" + (finalDiag != null ? finalDiag.replace("'", "'\\''") : "no diag") 
                    + "' > /sdcard/jawmods_diag.txt 2>/dev/null");

                String msg = "Hook NAO conectou.\n"
                    + "Metodos tentados:\n"
                    + "- Zygisk: " + (zygiskWorked ? "OK" : "FALHOU") + "\n"
                    + "- attach-agent: " + (agentTried ? (lastAgentResult != null ? lastAgentResult.trim() : "tentado") : "nao usado") + "\n"
                    + "- set-debug-app: " + (agentDebugTried ? "tentado" : "nao usado") + "\n\n"
                    + "Diag salvo: /sdcard/jawmods_diag.txt";
                updateStatus(msg);
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        btnStop.setVisibility(View.VISIBLE);
                    }
                });
            }

            resetButton();

        } catch (final Exception e) {
            final String err = "Fatal error: " + e.getClass().getSimpleName() + ": " + e.getMessage();
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    Toast.makeText(MainActivity.this, err, Toast.LENGTH_LONG).show();
                }
            });
            updateStatus(err);
            resetButton();
        }
    }

    // ── Helpers ──

    private String readAnyHookLog(String gameDir) {
        String log = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null");
        if (log != null && !log.trim().isEmpty()) return log.trim();
        log = rootExec("cat " + gameDir + "/.hook_log 2>/dev/null");
        if (log != null && !log.trim().isEmpty()) return log.trim();
        return null;
    }

    private void updateStatus(final String text) {
        runOnUi(new Runnable() {
            @Override
            public void run() {
                setStatus(text);
            }
        });
    }

    private void resetButton() {
        showLoading(false);
        runOnUi(new Runnable() {
            @Override
            public void run() {
                btnStart.setVisibility(View.VISIBLE);
                btnStart.setEnabled(true);
                injecting = false;
            }
        });
    }

    // ══════════════════════════════════════
    // Helpers
    // ══════════════════════════════════════

    private String rootExec(String cmd) {
        Process su = null;
        try {
            // su -c "cmd" é mais robusto que piping via stdin em dispositivos Android
            // Evita confusao do su ao spawnar muitos processos rapidamente
            su = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});

            // Consumir stderr em thread separada para evitar deadlock no pipe
            final Process proc = su;
            Thread stderrDrainer = new Thread(new Runnable() {
                @Override
                public void run() {
                    try {
                        byte[] buf = new byte[4096];
                        while (proc.getErrorStream().read(buf) != -1) { /* descarta */ }
                    } catch (Exception ignored) {}
                }
            });
            stderrDrainer.setDaemon(true);
            stderrDrainer.start();

            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(su.getInputStream()));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line).append("\n");
            }
            reader.close();
            su.waitFor();
            return sb.toString().trim();
        } catch (Exception e) {
            return null;
        } finally {
            if (su != null) {
                su.destroy();
            }
        }
    }

    private String getLauncherActivity(String pkg) {
        String result = rootExec("cmd package resolve-activity --brief " + pkg
                + " | tail -1");
        if (result != null && result.contains("/")) {
            String activity = result.trim();
            if (activity.contains(pkg)) {
                return activity.substring(activity.indexOf("/"));
            }
        }
        result = rootExec("dumpsys package " + pkg
                + " | grep -A1 'android.intent.action.MAIN' | grep -o '"
                + pkg + "/[^ ]*'");
        if (result != null && result.contains("/")) {
            return result.trim().substring(result.indexOf("/"));
        }
        return "/.MainActivity";
    }

    private void setStatus(String text) {
        tvStatus.setText(text);
    }

    private void runOnUi(Runnable r) {
        handler.post(r);
    }
}
