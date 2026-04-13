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
import java.io.DataOutputStream;
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

            // 5. Instalar/atualizar modulo Zygisk
            updateStatus("Installing Zygisk module...");
            rootExec("mkdir -p " + moduleDir + "/zygisk");
            rootExec("cp " + hookSrc + " " + moduleDir + "/zygisk/arm64-v8a.so");
            rootExec("chmod 644 " + moduleDir + "/zygisk/arm64-v8a.so");

            // Criar module.prop
            rootExec("echo 'id=jawmods' > " + moduleDir + "/module.prop");
            rootExec("echo 'name=JawMods ESP Hook' >> " + moduleDir + "/module.prop");
            rootExec("echo 'version=v10' >> " + moduleDir + "/module.prop");
            rootExec("echo 'versionCode=10' >> " + moduleDir + "/module.prop");
            rootExec("echo 'author=JawMods' >> " + moduleDir + "/module.prop");
            rootExec("echo 'description=ESP hook via Zygisk for Unity games' >> " + moduleDir + "/module.prop");

            // Verificar que o modulo foi instalado
            String checkModule = rootExec("cat " + moduleDir + "/module.prop 2>/dev/null");
            if (checkModule == null || !checkModule.contains("jawmods")) {
                updateStatus("Failed to install module.\nCheck Magisk and root access.");
                resetButton();
                return;
            }

            // 6. Pre-criar SHM e hook log com permissoes
            rootExec("rm -f /data/local/tmp/.esp_shm /data/local/tmp/.hook_log");
            rootExec("dd if=/dev/zero of=/data/local/tmp/.esp_shm bs=4096 count=1 2>/dev/null");
            rootExec("chmod 666 /data/local/tmp/.esp_shm");
            rootExec("touch /data/local/tmp/.hook_log; chmod 666 /data/local/tmp/.hook_log");

            // Game dir permissoes para overlay ler SHM
            rootExec("mkdir -p " + gameDir);
            rootExec("chmod 711 " + gameDir);
            rootExec("touch " + gameDir + "/.hook_log; chmod 666 " + gameDir + "/.hook_log");

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

            // 8. Aguardar conexao com hook (verificar SHM)
            boolean connected = false;
            for (int i = 0; i < 15; i++) {
                Thread.sleep(1000);

                // Checar hook log
                String hookLog = readAnyHookLog(gameDir);
                if (hookLog != null && (hookLog.contains("HOOK ATIVO") || hookLog.contains("HOOK CARREGADO"))) {
                    connected = true;
                    break;
                }

                // Checar se o jogo esta rodando com hook
                String pid2 = rootExec("pidof " + GAME_PACKAGE);
                if (pid2 != null && !pid2.trim().isEmpty()) {
                    String p = pid2.trim().split("\\s+")[0];
                    String hookMaps = rootExec("grep -c 'jawmods\\|libHook' /proc/" + p + "/maps 2>/dev/null");
                    if (hookMaps != null && !"0".equals(hookMaps.trim())) {
                        connected = true;
                        break;
                    }
                    updateStatus("Waiting for hook... (" + (i+1) + "s)");
                } else {
                    updateStatus("Open Free Fire to connect (" + (i+1) + "s)");
                }
            }

            showLoading(false);
            if (connected) {
                updateStatus("Connected! ESP active.");
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        btnStop.setVisibility(View.VISIBLE);
                    }
                });
            } else {
                // Overlay ja esta rodando — vai conectar quando o jogo abrir
                updateStatus("Overlay active.\nOpen Free Fire — hook loads automatically.\n" +
                    "(If first time, reboot first)");
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
            runOnUi(() -> Toast.makeText(this, err, Toast.LENGTH_LONG).show());
            updateStatus(err);
            resetButton();
        }
    }

    // ── Helpers ──

    private String readAnyHookLog(String gameDir) {
        String log = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null");
        if (log != null && !log.trim().isEmpty()) return log;
        log = rootExec("cat " + gameDir + "/.hook_log 2>/dev/null");
        if (log != null && !log.trim().isEmpty()) return log;
        log = rootExec("cat /sdcard/.hook_log 2>/dev/null");
        return log;
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
        try {
            Process su = Runtime.getRuntime().exec("su");
            DataOutputStream os = new DataOutputStream(su.getOutputStream());
            os.writeBytes(cmd + "\n");
            os.writeBytes("exit\n");
            os.flush();

            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(su.getInputStream()));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line).append("\n");
            }
            int exitCode = su.waitFor();
            reader.close();
            os.close();
            String result = sb.toString().trim();
            if (exitCode != 0) {
                final String err = "rootExec failed: " + cmd + "\nexit=" + exitCode;
                runOnUi(() -> Toast.makeText(this, err, Toast.LENGTH_LONG).show());
                updateStatus(err);
            }
            return result;
        } catch (Exception e) {
            final String err = "rootExec exception: " + e.getClass().getSimpleName() + ": " + e.getMessage() + "\ncmd: " + cmd;
            runOnUi(() -> Toast.makeText(this, err, Toast.LENGTH_LONG).show());
            updateStatus(err);
            return null;
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
