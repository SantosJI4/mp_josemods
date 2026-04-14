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
    // LOGICA — PTRACE INJECTION (v15)
    // Usa injector nativo para fazer ptrace+dlopen no processo do jogo
    // Mesmo metodo do GameGuardian e LibTool
    // Nao precisa de Zygisk, attach-agent, ou set-debug-app
    // ══════════════════════════════════════
    private void doInjectAndStart() {
        try {
            // 1. Verificar root
            updateStatus("Checking root...");
            final String rootCheck = rootExec("id");
            if (rootCheck == null || !rootCheck.contains("uid=0")) {
                updateStatus("No root access.\nptrace injection requires root (su).");
                resetButton();
                return;
            }

            // 2. Copiar binarios para /data/local/tmp/
            updateStatus("Preparing files...");
            final String nativeDir = getApplicationInfo().nativeLibraryDir;
            final String hookSrc = nativeDir + "/libHook.so";
            final String tmpHook = "/data/local/tmp/libHook.so";
            final String tmpInjector = "/data/local/tmp/injector";

            if (!new File(hookSrc).exists()) {
                updateStatus("libHook.so not found.\nDid you do a clean build?");
                resetButton();
                return;
            }

            // Injector fica como libinjector.so no APK
            // (Android so extrai lib*.so para nativeLibraryDir)
            String injSrc = nativeDir + "/libinjector.so";
            if (!new File(injSrc).exists()) {
                // Fallback: checar sem prefixo/sufixo
                injSrc = nativeDir + "/injector";
            }
            if (!new File(injSrc).exists()) {
                // Ultimo fallback: libinjector (sem .so)
                injSrc = nativeDir + "/libinjector";
            }
            if (!new File(injSrc).exists()) {
                updateStatus("Injector binary not found.\n"
                    + "nativeDir: " + nativeDir + "\n"
                    + "Tried: libinjector.so, injector, libinjector\n"
                    + "Rebuild the project.");
                resetButton();
                return;
            }

            // Copiar e dar permissoes
            rootExec("cp " + injSrc + " " + tmpInjector
                    + " ; cp " + hookSrc + " " + tmpHook
                    + " ; chmod 755 " + tmpInjector
                    + " ; chmod 755 " + tmpHook);

            // Validar copia
            String checkInj = rootExec("ls -la " + tmpInjector + " 2>/dev/null");
            String checkHook = rootExec("ls -la " + tmpHook + " 2>/dev/null");
            if (checkInj == null || !checkInj.contains("injector")
                || checkHook == null || !checkHook.contains("libHook")) {
                updateStatus("Failed to copy files to /data/local/tmp/.\nCheck root access.");
                resetButton();
                return;
            }

            // 3. Pre-criar SHM e SELinux (runtime — via supolicy se disponivel)
            updateStatus("Setting up SHM...");
            rootExec("rm -f /data/local/tmp/.esp_shm /data/local/tmp/.hook_log"
                    + " ; dd if=/dev/zero of=/data/local/tmp/.esp_shm bs=4096 count=1 2>/dev/null"
                    + " ; chmod 666 /data/local/tmp/.esp_shm"
                    + " ; touch /data/local/tmp/.hook_log ; chmod 666 /data/local/tmp/.hook_log");

            // SELinux permissivo para o /data/local/tmp (runtime — nao precisa reboot)
            rootExec("supolicy --live 'allow untrusted_app shell_data_file dir { search read write open create add_name getattr }'"
                    + " 2>/dev/null;"
                    + " supolicy --live 'allow untrusted_app shell_data_file file { read write open create getattr setattr }'"
                    + " 2>/dev/null;"
                    + " magiskpolicy --live 'allow untrusted_app shell_data_file dir { search read write open create add_name getattr }'"
                    + " 2>/dev/null;"
                    + " magiskpolicy --live 'allow untrusted_app shell_data_file file { read write open create getattr setattr }'"
                    + " 2>/dev/null");
            // Tambem precisa regra pra ptrace
            rootExec("supolicy --live 'allow shell untrusted_app process { ptrace }' 2>/dev/null;"
                    + " magiskpolicy --live 'allow shell untrusted_app process { ptrace }' 2>/dev/null;"
                    + " supolicy --live 'allow init untrusted_app process { ptrace }' 2>/dev/null;"
                    + " magiskpolicy --live 'allow init untrusted_app process { ptrace }' 2>/dev/null");

            // 4. Fechar e reiniciar o jogo (limpo)
            updateStatus("Starting Free Fire...");
            rootExec("am force-stop " + GAME_PACKAGE + " 2>/dev/null");
            Thread.sleep(1500);

            String launcherActivity = getLauncherActivity(GAME_PACKAGE);
            if (launcherActivity != null && launcherActivity.contains("/")) {
                rootExec("am start -n " + GAME_PACKAGE + launcherActivity + " 2>/dev/null");
            } else {
                rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1 2>/dev/null");
            }

            // 5. Esperar PID do jogo (max 25s)
            updateStatus("Waiting for game PID...");
            String gamePid = "";
            for (int i = 0; i < 25; i++) {
                Thread.sleep(1000);
                gamePid = rootExec("pidof " + GAME_PACKAGE + " 2>/dev/null | awk '{print $1}'");
                if (gamePid != null && !gamePid.trim().isEmpty()) {
                    gamePid = gamePid.trim();
                    break;
                }
                updateStatus("Waiting for game PID... (" + (i+1) + "s)");
            }

            if (gamePid == null || gamePid.trim().isEmpty()) {
                updateStatus("Game did not start (no PID after 25s).\nTry opening the game manually.");
                resetButton();
                return;
            }

            // 6. Esperar il2cpp carregar (3-8s depois do PID)
            // ptrace so funciona depois q jogo tem as libs carregadas
            updateStatus("PID=" + gamePid + "\nWaiting for libil2cpp.so...");
            boolean il2cppLoaded = false;
            for (int i = 0; i < 15; i++) {
                Thread.sleep(1000);
                String mc = rootExec("grep -c 'libil2cpp.so' /proc/" + gamePid + "/maps 2>/dev/null");
                int count = 0;
                try { count = Integer.parseInt(mc != null ? mc.trim() : "0"); } catch (Exception e) {}
                if (count > 0) {
                    il2cppLoaded = true;
                    break;
                }
                // Checar se PID mudou
                String np = rootExec("pidof " + GAME_PACKAGE + " 2>/dev/null | awk '{print $1}'");
                if (np != null && !np.trim().isEmpty() && !np.trim().equals(gamePid)) {
                    gamePid = np.trim();
                }
                updateStatus("PID=" + gamePid + "\nWaiting for libil2cpp.so... (" + (i+1) + "s)");
            }

            if (!il2cppLoaded) {
                updateStatus("libil2cpp.so not found in maps after 15s.\n"
                    + "Game may not be Unity/il2cpp or is still loading.\n"
                    + "PID=" + gamePid);
                // Continuar mesmo assim — talvez demore mais
            }

            // 7. INJECAO VIA PTRACE
            updateStatus("INJECTING via ptrace...\nPID=" + gamePid);

            // Iniciar overlay ANTES da injecao (pode demorar)
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    startService(new Intent(MainActivity.this, OverlayService.class));
                }
            });

            // Executar injector
            String injectResult = rootExec(tmpInjector + " " + gamePid + " " + tmpHook + " 2>&1");
            updateStatus("Injector output:\n" + (injectResult != null ? injectResult.trim() : "(null)"));

            boolean injected = injectResult != null && injectResult.contains("SUCCESSFUL");
            boolean dlOpenOk = injectResult != null && injectResult.contains("handle:");

            if (!injected) {
                // Verificar se falhou por SELinux
                boolean selinuxBlock = injectResult != null
                    && (injectResult.contains("Permission denied") || injectResult.contains("Operation not permitted"));

                if (selinuxBlock) {
                    // Tentar setenforce 0 como ultimo recurso
                    updateStatus("SELinux blocking ptrace. Trying setenforce 0...");
                    rootExec("setenforce 0 2>/dev/null");
                    Thread.sleep(500);

                    // Re-pegar PID (pode ter crashado)
                    String np = rootExec("pidof " + GAME_PACKAGE + " 2>/dev/null | awk '{print $1}'");
                    if (np == null || np.trim().isEmpty()) {
                        // Jogo crashou — reiniciar
                        updateStatus("Game crashed. Restarting...");
                        if (launcherActivity != null && launcherActivity.contains("/")) {
                            rootExec("am start -n " + GAME_PACKAGE + launcherActivity + " 2>/dev/null");
                        } else {
                            rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1 2>/dev/null");
                        }
                        for (int i = 0; i < 20; i++) {
                            Thread.sleep(1000);
                            np = rootExec("pidof " + GAME_PACKAGE + " 2>/dev/null | awk '{print $1}'");
                            if (np != null && !np.trim().isEmpty()) break;
                        }
                        if (np != null && !np.trim().isEmpty()) {
                            gamePid = np.trim();
                            // Esperar il2cpp
                            for (int i = 0; i < 10; i++) {
                                Thread.sleep(1000);
                                String mc = rootExec("grep -c 'libil2cpp.so' /proc/" + gamePid + "/maps 2>/dev/null");
                                int c = 0;
                                try { c = Integer.parseInt(mc != null ? mc.trim() : "0"); } catch (Exception e) {}
                                if (c > 0) break;
                            }
                        }
                    } else {
                        gamePid = np.trim();
                    }

                    // Retry injecao
                    injectResult = rootExec(tmpInjector + " " + gamePid + " " + tmpHook + " 2>&1");
                    updateStatus("Retry output:\n" + (injectResult != null ? injectResult.trim() : "(null)"));
                    injected = injectResult != null && injectResult.contains("SUCCESSFUL");

                    // Restaurar SELinux
                    rootExec("setenforce 1 2>/dev/null");
                }
            }

            // 8. Monitorar hook (20s)
            boolean connected = false;
            for (int i = 0; i < 20; i++) {
                Thread.sleep(1000);

                // Checar se PID vivo
                String cp = rootExec("pidof " + GAME_PACKAGE + " 2>/dev/null | awk '{print $1}'");
                if (cp == null || cp.trim().isEmpty()) {
                    updateStatus("Game crashed after injection.\n"
                        + (injectResult != null ? injectResult.trim() : ""));
                    continue;
                }
                gamePid = cp.trim();

                // Verificar hook nos maps
                String mc = rootExec("grep -c 'libHook' /proc/" + gamePid + "/maps 2>/dev/null");
                int mapsCount = 0;
                try { mapsCount = Integer.parseInt(mc != null ? mc.trim() : "0"); } catch (Exception e) {}

                // Verificar hook log (hook ativo = VMT substituido)
                String hookLog = rootExec(
                    "cat /data/local/tmp/.hook_log 2>/dev/null | tail -2;"
                    + " cat /data/data/" + GAME_PACKAGE + "/.hook_log 2>/dev/null | tail -2");
                boolean hookActive = hookLog != null && (hookLog.contains("HOOK ATIVO")
                    || hookLog.contains("SHM OK") || hookLog.contains("VMT"));

                // Checar logcat
                String lc = rootExec("logcat -d -s GameHook 2>/dev/null | grep " + gamePid + " | tail -3");
                boolean lcActive = lc != null && (lc.contains("HOOK ATIVO") || lc.contains("hack_thread")
                    || lc.contains("HOOK CARREGADO"));

                if (hookActive || lcActive) {
                    connected = true;
                    updateStatus("HOOK ATIVO!\n" + (hookLog != null ? hookLog.trim() : ""));
                    break;
                }

                if (mapsCount > 0) {
                    updateStatus("Hook loaded (maps=" + mapsCount + "), waiting il2cpp...\n(" + (i+1) + "s)");
                } else {
                    updateStatus("Waiting for hook... maps=" + mapsCount + " PID=" + gamePid
                        + "\n(" + (i+1) + "/20s)"
                        + (injected ? "\nInjection: OK" : "\nInjection: FAILED"));
                }
            }

            showLoading(false);
            if (connected) {
                updateStatus("Conectado! Hook ativo via ptrace.");
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        btnStop.setVisibility(View.VISIBLE);
                    }
                });
            } else {
                // Diagnostico
                final String fPid = gamePid;
                String finalDiag = rootExec(
                    "echo '=== JAWMODS DIAG v15 (ptrace) ===';"
                    + " echo 'PID=" + fPid + "';"
                    + " echo 'SELinux:'; getenforce 2>/dev/null;"
                    + " echo 'Maps hook:'; grep -iE 'libHook' /proc/" + fPid + "/maps 2>/dev/null | head -5 || echo '(nothing)';"
                    + " echo 'Logcat GameHook:';"
                    + " logcat -d -s GameHook 2>/dev/null | tail -20;"
                    + " echo 'Logcat injector:';"
                    + " logcat -d -s injector 2>/dev/null | tail -10;"
                    + " echo 'HookLog:';"
                    + " cat /data/local/tmp/.hook_log 2>/dev/null;"
                    + " cat /data/data/" + GAME_PACKAGE + "/.hook_log 2>/dev/null;"
                    + " echo 'Injector result: " + (injectResult != null ? injectResult.replace("'", "").replace("\n", " ") : "null") + "'"
                );
                rootExec("echo '" + (finalDiag != null ? finalDiag.replace("'", "'\\''") : "no diag")
                    + "' > /sdcard/jawmods_diag.txt 2>/dev/null");

                updateStatus("Hook did NOT connect.\n"
                    + "Injection: " + (injected ? "OK (dlopen success)" : "FAILED") + "\n"
                    + (injectResult != null ? injectResult.trim().substring(0, Math.min(injectResult.trim().length(), 200)) : "(null)")
                    + "\n\nDiag saved: /sdcard/jawmods_diag.txt");
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
