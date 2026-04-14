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
            if (existingProp != null && existingProp.contains("jawmods")
                && existingSo != null && existingSo.contains("arm64-v8a.so")) {
                moduleAlreadyExisted = true;
            }

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
                    + " ; chmod 644 " + moduleDir + "/zygisk/arm64-v8a.so");

            // Criar module.prop em uma unica chamada su
            rootExec("printf 'id=jawmods\\nname=JawMods ESP Hook\\nversion=v10\\nversionCode=10\\nauthor=JawMods\\ndescription=ESP hook via Zygisk for Unity games\\n'"
                    + " > " + moduleDir + "/module.prop");

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
            if (!moduleAlreadyExisted) {
                // PRIMEIRO INSTALL — precisa reboot para Zygisk ativar
                showLoading(false);
                updateStatus("Module installed!\n\n" +
                    "Now you need to:\n" +
                    "1. Enable Zygisk in Magisk settings\n" +
                    "2. Reboot your phone\n" +
                    "3. Open Free Fire normally\n" +
                    "4. Open JawMods and click Start\n\n" +
                    "Magisk v" + magiskVer.trim());
                resetButton();
                return;
            }

            // Modulo ja existia — Zygisk ja esta ativo, iniciar overlay
            updateStatus("Starting overlay...");
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    startService(new Intent(MainActivity.this, OverlayService.class));
                }
            });

            // 8. Aguardar conexao com hook — loop inicial de 20s com diagnostico
            // Zygisk injeta ANTES do jogo inicializar, pode demorar ate il2cpp carregar
            boolean connected = false;
            for (int i = 0; i < 20; i++) {
                Thread.sleep(1000);

                // Uma chamada su: retorna diagnostico completo
                // grep -cE usa extended regex (|  funciona em TODOS os shells Android)
                // Checar arm64-v8a.so (nome real nos maps do Zygisk) + jawmods + libHook
                String diag = rootExec(
                    "P=$(pidof " + GAME_PACKAGE + " 2>/dev/null | awk '{print $1}');"
                    + " if [ -z \"$P\" ]; then echo 'NOPID';"
                    + " else"
                    + "   M=$(grep -cE 'arm64-v8a\\.so|jawmods|libHook|libMEOW' /proc/$P/maps 2>/dev/null);"
                    + "   L=$(cat /data/local/tmp/.hook_log 2>/dev/null | tail -3);"
                    + "   echo \"PID=$P MAPS=$M LOG=$L\";"
                    + " fi");

                if (diag == null || diag.contains("NOPID") || diag.trim().isEmpty()) {
                    updateStatus("Abra o Free Fire para iniciar o hook...\n(" + (i+1) + "s)");
                    continue;
                }

                // Extrair MAPS count
                int mapsCount = 0;
                try {
                    int mIdx = diag.indexOf("MAPS=");
                    if (mIdx >= 0) {
                        String mStr = diag.substring(mIdx + 5).trim().split("\\s+")[0];
                        mapsCount = Integer.parseInt(mStr);
                    }
                } catch (Exception ignored) {}

                // Extrair LOG
                String logPart = "";
                int lIdx = diag.indexOf("LOG=");
                if (lIdx >= 0) logPart = diag.substring(lIdx + 4).trim();

                boolean hookInMaps  = mapsCount > 0;
                boolean hookInLog   = logPart.contains("HOOK ATIVO") || logPart.contains("HOOK CARREGADO")
                                   || logPart.contains("SHM OK") || logPart.contains("VMT");

                if (hookInMaps || hookInLog) {
                    connected = true;
                    break;
                }

                // Mostrar diagnostico ao usuario para facilitar debug
                updateStatus("Hook carregando...\nJogo rodando, aguardando il2cpp...\n"
                    + "maps=" + mapsCount
                    + (logPart.isEmpty() ? " | sem log" : "\nlog: " + logPart.substring(0, Math.min(logPart.length(), 60)))
                    + "\n(" + (i+1) + "s)");
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
                updateStatus("Overlay ativo. Abra o Free Fire — hook carrega automaticamente.\nAguardando...");
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        btnStop.setVisibility(View.VISIBLE);
                    }
                });
            }

            resetButton();

            // Manter poll em background: atualiza status quando hook conectar
            // (Zygisk pode demorar se il2cpp estava carregando)
            final String finalGameDir = gameDir;
            hookPolling = true;
            hookPollThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    while (hookPolling) {
                        try {
                            Thread.sleep(2000);
                        } catch (InterruptedException e) {
                            break;
                        }
                        if (!hookPolling) break;

                        String diag = rootExec(
                            "P=$(pidof " + GAME_PACKAGE + " 2>/dev/null | awk '{print $1}');"
                            + " if [ -z \"$P\" ]; then echo 'NOPID';"
                            + " else"
                            + "   M=$(grep -cE 'arm64-v8a\\.so|jawmods|libHook|libMEOW' /proc/$P/maps 2>/dev/null);"
                            + "   L=$(cat /data/local/tmp/.hook_log 2>/dev/null | tail -1);"
                            + "   echo \"PID=$P MAPS=$M LOG=$L\";"
                            + " fi");

                        if (diag == null || diag.contains("NOPID")) continue;

                        int mapsCount = 0;
                        try {
                            int mIdx = diag.indexOf("MAPS=");
                            if (mIdx >= 0) {
                                String mStr = diag.substring(mIdx + 5).trim().split("\\s+")[0];
                                mapsCount = Integer.parseInt(mStr);
                            }
                        } catch (Exception ignored) {}

                        String logPart = "";
                        int lIdx = diag.indexOf("LOG=");
                        if (lIdx >= 0) logPart = diag.substring(lIdx + 4).trim();

                        boolean hookInMaps = mapsCount > 0;
                        boolean hookInLog  = logPart.contains("HOOK ATIVO") || logPart.contains("SHM OK")
                                          || logPart.contains("HOOK CARREGADO") || logPart.contains("VMT");

                        if (hookInMaps || hookInLog) {
                            final String lastLog = logPart;
                            updateStatus("Conectado! Hook ativo.\n" + lastLog);
                            hookPolling = false;
                            break;
                        }

                        // Mostrar estado atual continuamente
                        final String statusLine = "Aguardando hook...\nmaps=" + mapsCount
                            + (logPart.isEmpty() ? "" : "\n" + logPart.substring(0, Math.min(logPart.length(), 80)));
                        updateStatus(statusLine);
                    }
                }
            });
            hookPollThread.setDaemon(true);
            hookPollThread.start();

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
