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
                        setStatus("Stopped");
                        btnStop.setVisibility(View.GONE);
                        btnStart.setVisibility(View.VISIBLE);
                        btnStart.setEnabled(true);
                    }
                });
                rootExec("rm -f /data/local/tmp/.esp_shm");
                rootExec("rm -f /data/local/tmp/.hook_log /sdcard/.hook_log");
                rootExec("rm -f /data/data/" + GAME_PACKAGE + "/.esp_shm /data/data/" + GAME_PACKAGE + "/.hook_log");
                rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");
                rootExec("am clear-debug-app");
            }
        }).start();
    }

    // ══════════════════════════════════════
    // LOGICA DE INJECAO — roda em background
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

            // 2. SELinux permissive
            rootExec("setenforce 0");
            updateStatus("Preparing...");

            // 3. Copiar libHook.so para todos os locais possiveis
            final String nativeLibDir = getApplicationInfo().nativeLibraryDir;
            final String hookSrc = nativeLibDir + "/libHook.so";
            final String hookDst = "/data/local/tmp/libHook.so";
            final String gameDir = "/data/data/" + GAME_PACKAGE;

            if (!new File(hookSrc).exists()) {
                updateStatus("Hook library not found");
                resetButton();
                return;
            }

            rootExec("cp " + hookSrc + " " + hookDst);
            rootExec("chmod 755 " + hookDst);
            rootExec("chcon u:object_r:system_lib_file:s0 " + hookDst);

            rootExec("mkdir -p " + gameDir);
            rootExec("chmod 711 " + gameDir);
            rootExec("cp " + hookDst + " " + gameDir + "/libHook.so");
            rootExec("chmod 755 " + gameDir + "/libHook.so");
            rootExec("chcon u:object_r:app_data_file:s0 " + gameDir + "/libHook.so");

            // 4. Pre-criar SHM + hook log em todos os paths
            rootExec("rm -f /data/local/tmp/.esp_shm /data/local/tmp/.hook_log");
            rootExec("dd if=/dev/zero of=/data/local/tmp/.esp_shm bs=4096 count=1 2>/dev/null");
            rootExec("chmod 666 /data/local/tmp/.esp_shm; chcon u:object_r:app_data_file:s0 /data/local/tmp/.esp_shm");
            rootExec("touch /data/local/tmp/.hook_log; chmod 666 /data/local/tmp/.hook_log; chcon u:object_r:app_data_file:s0 /data/local/tmp/.hook_log");

            rootExec("rm -f " + gameDir + "/.esp_shm " + gameDir + "/.hook_log");
            rootExec("touch " + gameDir + "/.hook_log; chmod 666 " + gameDir + "/.hook_log; chcon u:object_r:app_data_file:s0 " + gameDir + "/.hook_log");

            rootExec("rm -f /sdcard/.esp_shm /sdcard/.hook_log");
            rootExec("dd if=/dev/zero of=/sdcard/.esp_shm bs=4096 count=1 2>/dev/null");
            rootExec("chmod 666 /sdcard/.esp_shm");
            rootExec("touch /sdcard/.hook_log; chmod 666 /sdcard/.hook_log");

            // 5. Garantir debuggable por TODOS os metodos possiveis
            rootExec("resetprop ro.debuggable 1 2>/dev/null");
            rootExec("magiskpolicy --live 'allow untrusted_app untrusted_app process { ptrace }' 2>/dev/null");
            rootExec("magiskpolicy --live 'allow system_server untrusted_app process { ptrace }' 2>/dev/null");
            rootExec("settings put global debug_app " + GAME_PACKAGE + " 2>/dev/null");
            rootExec("am set-debug-app --persistent " + GAME_PACKAGE);

            // 6. Criar wrap scripts
            rootExec("echo '#!/system/bin/sh' > /data/local/tmp/wrap_hook.sh");
            rootExec("echo 'export LD_PRELOAD=" + hookDst + "' >> /data/local/tmp/wrap_hook.sh");
            rootExec("echo 'exec \"$@\"' >> /data/local/tmp/wrap_hook.sh");
            rootExec("chmod 755 /data/local/tmp/wrap_hook.sh; chcon u:object_r:system_file:s0 /data/local/tmp/wrap_hook.sh");

            rootExec("echo '#!/system/bin/sh' > /data/local/tmp/wrap_hook2.sh");
            rootExec("echo 'export LD_PRELOAD=" + gameDir + "/libHook.so' >> /data/local/tmp/wrap_hook2.sh");
            rootExec("echo 'exec \"$@\"' >> /data/local/tmp/wrap_hook2.sh");
            rootExec("chmod 755 /data/local/tmp/wrap_hook2.sh; chcon u:object_r:system_file:s0 /data/local/tmp/wrap_hook2.sh");

            // 7. Iniciar overlay
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    startService(new Intent(MainActivity.this, OverlayService.class));
                }
            });

            // 8. Resolver launcher activity
            final String launcherAct = getLauncherActivity(GAME_PACKAGE);

            // ═══════════════════════════════════════════
            // METODO 1 (PRINCIPAL): ATTACH-AGENT
            // Mais confiavel — funciona mesmo com anti-cheat
            // Requer: jogo rodando + debuggable
            // ═══════════════════════════════════════════
            updateStatus("Starting game...");
            rootExec("logcat -c 2>/dev/null");

            // Iniciar o jogo se nao estiver rodando
            String pid = rootExec("pidof " + GAME_PACKAGE);
            if (pid == null || pid.trim().isEmpty()) {
                rootExec("am force-stop " + GAME_PACKAGE);
                Thread.sleep(500);
                startGame(launcherAct);
                Thread.sleep(3000);
                pid = rootExec("pidof " + GAME_PACKAGE);
            }

            boolean hookOk = false;

            if (pid != null && !pid.trim().isEmpty()) {
                final String gamePid = pid.trim().split("\\s+")[0];

                // Esperar libil2cpp.so carregar (game Unity pronto)
                updateStatus("Waiting for Unity engine...");
                boolean il2cppReady = false;
                for (int w = 0; w < 60; w++) {
                    // PID pode mudar se o jogo reiniciar
                    String curPid = rootExec("pidof " + GAME_PACKAGE);
                    if (curPid == null || curPid.trim().isEmpty()) {
                        Thread.sleep(1000);
                        updateStatus("Game starting... (" + (w+1) + "s)");
                        continue;
                    }
                    String activePid = curPid.trim().split("\\s+")[0];
                    String il2cpp = rootExec("grep -c libil2cpp.so /proc/" + activePid + "/maps 2>/dev/null");
                    if (il2cpp != null && !"0".equals(il2cpp.trim())) {
                        il2cppReady = true;
                        pid = curPid; // Atualizar PID
                        break;
                    }
                    Thread.sleep(1000);
                    updateStatus("Loading engine... (" + (w+1) + "s)");
                }

                if (!il2cppReady) {
                    updateStatus("il2cpp not detected. Is this a Unity game?");
                    resetButton();
                    return;
                }

                // Atualizar PID (pode ter mudado)
                final String finalPid = pid.trim().split("\\s+")[0];

                // Esperar mais 3s para il2cpp inicializar assemblies
                updateStatus("Initializing il2cpp...");
                Thread.sleep(3000);

                // Limpar logs antes de injetar
                rootExec("echo '' > /data/local/tmp/.hook_log 2>/dev/null");
                rootExec("echo '' > " + gameDir + "/.hook_log 2>/dev/null");

                // attach-agent: tenta com /data/local/tmp/ primeiro
                updateStatus("Injecting (attach-agent)...");
                rootExec("cmd activity attach-agent " + finalPid + " " + hookDst + " 2>/dev/null");
                Thread.sleep(2000);

                hookOk = waitForHook(gameDir, 10);

                // Fallback: tenta com path do game dir
                if (!hookOk && !isHookInMaps(finalPid)) {
                    updateStatus("Retry: game dir path...");
                    rootExec("cmd activity attach-agent " + finalPid + " " + gameDir + "/libHook.so 2>/dev/null");
                    Thread.sleep(2000);
                    hookOk = waitForHook(gameDir, 10);
                }
            }

            // ═══════════════════════════════════════════
            // METODO 2 (FALLBACK): WRAP + LD_PRELOAD
            // So tenta se attach-agent falhou
            // ═══════════════════════════════════════════
            if (!hookOk) {
                updateStatus("Fallback: wrap + restart...");

                rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");
                Thread.sleep(300);
                rootExec("setprop wrap." + GAME_PACKAGE + " /data/local/tmp/wrap_hook.sh");

                String propVal = rootExec("getprop wrap." + GAME_PACKAGE);
                if (propVal == null || !propVal.contains("wrap_hook")) {
                    rootExec("resetprop wrap." + GAME_PACKAGE + " /data/local/tmp/wrap_hook.sh 2>/dev/null");
                }

                rootExec("am force-stop " + GAME_PACKAGE);
                Thread.sleep(1500);
                startGame(launcherAct);
                hookOk = waitForHook(gameDir, 25);
                rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");
            }

            // ═══════════════════════════════════════════
            // METODO 3: WRAP com .so do game dir
            // ═══════════════════════════════════════════
            if (!hookOk) {
                updateStatus("Fallback: alt wrap path...");
                rootExec("setprop wrap." + GAME_PACKAGE + " /data/local/tmp/wrap_hook2.sh");
                rootExec("am force-stop " + GAME_PACKAGE);
                Thread.sleep(1500);
                startGame(launcherAct);
                hookOk = waitForHook(gameDir, 20);
                rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");
            }

            // Limpar wrap
            rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");

            // Status final com diagnosticos
            if (hookOk) {
                showLoading(false);
                updateStatus("Injected successfully!");
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        btnStop.setVisibility(View.VISIBLE);
                    }
                });
            } else {
                // Mostrar diagnosticos ao usuario
                String diag = getDiagnostics(gameDir);
                updateStatus("Injection failed.\n" + diag);
            }

            resetButton();

        } catch (final Exception e) {
            updateStatus("Error: " + e.getMessage());
            resetButton();
        }
    }

    // ── Helpers de injeção ──

    private void startGame(String launcherAct) {
        if (launcherAct != null && launcherAct.length() > 1) {
            rootExec("am start -n " + GAME_PACKAGE + "/" + launcherAct);
        } else {
            rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1");
        }
    }

    private boolean isHookInMaps(String pid) {
        String m = rootExec("grep -c libHook /proc/" + pid + "/maps 2>/dev/null");
        return m != null && !"0".equals(m.trim()) && !m.trim().isEmpty();
    }

    private boolean waitForHook(String gameDir, int maxSeconds) {
        for (int i = 0; i < maxSeconds; i++) {
            try { Thread.sleep(1000); } catch (Exception e) { break; }

            // Checar hook log
            String hookLog = readAnyHookLog(gameDir);
            if (hookLog != null && (hookLog.contains("HOOK ATIVO") || hookLog.contains("HOOK CARREGADO"))) {
                return true;
            }

            // Checar logcat
            String lc = rootExec("logcat -d -s HOOK 2>/dev/null | grep -c 'HOOK CARREGADO\\|HOOK ATIVO' 2>/dev/null");
            if (lc != null && !"0".equals(lc.trim())) {
                return true;
            }

            // Checar maps
            String pid = rootExec("pidof " + GAME_PACKAGE);
            if (pid != null && !pid.trim().isEmpty()) {
                if (isHookInMaps(pid.trim().split("\\s+")[0])) {
                    if (i > 5) return true; // No maps ha mais de 5s = ok
                }
                updateStatus("Injecting... (" + (i+1) + "s)");
            } else {
                updateStatus("Starting game... (" + (i+1) + "s)");
            }
        }
        return false;
    }

    private String readAnyHookLog(String gameDir) {
        String log = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null");
        if (log != null && !log.trim().isEmpty()) return log;
        log = rootExec("cat " + gameDir + "/.hook_log 2>/dev/null");
        if (log != null && !log.trim().isEmpty()) return log;
        log = rootExec("cat /sdcard/.hook_log 2>/dev/null");
        return log;
    }

    private String getDiagnostics(String gameDir) {
        StringBuilder sb = new StringBuilder();
        String pid = rootExec("pidof " + GAME_PACKAGE);
        sb.append("PID: ").append(pid != null ? pid.trim() : "not found").append("\n");

        String dbg = rootExec("getprop ro.debuggable");
        sb.append("debuggable: ").append(dbg != null ? dbg.trim() : "?").append("\n");

        String magisk = rootExec("magisk -V 2>/dev/null");
        sb.append("Magisk: ").append(magisk != null ? magisk.trim() : "no").append("\n");

        if (pid != null && !pid.trim().isEmpty()) {
            String p = pid.trim().split("\\s+")[0];
            String maps = rootExec("grep -c libHook /proc/" + p + "/maps 2>/dev/null");
            sb.append("Hook in maps: ").append(maps != null ? maps.trim() : "?").append("\n");
            String il2cpp = rootExec("grep -c libil2cpp /proc/" + p + "/maps 2>/dev/null");
            sb.append("il2cpp loaded: ").append(il2cpp != null ? il2cpp.trim() : "?").append("\n");
        }

        String hookLog = readAnyHookLog(gameDir);
        if (hookLog != null && !hookLog.trim().isEmpty()) {
            String tail = hookLog.length() > 100 ? hookLog.substring(hookLog.length() - 100) : hookLog;
            sb.append("Log: ").append(tail);
        } else {
            sb.append("Hook log: empty");
        }
        return sb.toString();
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
            su.waitFor();
            reader.close();
            os.close();
            return sb.toString().trim();
        } catch (Exception e) {
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
