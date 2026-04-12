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
            updateStatus("Preparing environment...");

            // 3. Copiar libHook.so
            final String nativeLibDir = getApplicationInfo().nativeLibraryDir;
            final String hookSrc = nativeLibDir + "/libHook.so";
            final String hookDst = "/data/local/tmp/libHook.so";

            if (!new File(hookSrc).exists()) {
                updateStatus("Hook library not found");
                resetButton();
                return;
            }
            rootExec("cp " + hookSrc + " " + hookDst);
            rootExec("chmod 777 " + hookDst);
            rootExec("chcon u:object_r:system_lib_file:s0 " + hookDst);
            updateStatus("Copying libraries...");

            // 4. Pre-criar shared memory + hook log
            // IMPORTANTE: so criar em /data/local/tmp/ (UNICO path confiavel)
            // O hook confirmou que pode escrever la (fd=196)
            // O overlay tambem pode ler de la
            // NÃO criar no game dir — isso causa SHM mismatch
            final String gameDir = "/data/data/" + GAME_PACKAGE;

            // DELETAR qualquer SHM stale no game dir para evitar que o overlay conecte nele
            rootExec("rm -f " + gameDir + "/.esp_shm");
            rootExec("rm -f /data/user/0/" + GAME_PACKAGE + "/.esp_shm 2>/dev/null");

            // Hook log no game dir ainda e util (hook pode escrever por ser owner)
            rootExec("chmod 711 " + gameDir);
            rootExec("chmod 711 /data/user/0/" + GAME_PACKAGE + " 2>/dev/null");
            rootExec("rm -f " + gameDir + "/.hook_log");
            rootExec("touch " + gameDir + "/.hook_log; chmod 666 " + gameDir + "/.hook_log; chcon u:object_r:app_data_file:s0 " + gameDir + "/.hook_log");

            // PRIMARIO: /data/local/tmp/ — UNICO path para SHM
            rootExec("rm -f /data/local/tmp/.esp_shm");
            rootExec("dd if=/dev/zero of=/data/local/tmp/.esp_shm bs=4096 count=1 2>/dev/null");
            rootExec("chmod 666 /data/local/tmp/.esp_shm");
            rootExec("chcon u:object_r:app_data_file:s0 /data/local/tmp/.esp_shm");
            rootExec("rm -f /data/local/tmp/.hook_log");
            rootExec("touch /data/local/tmp/.hook_log; chmod 666 /data/local/tmp/.hook_log; chcon u:object_r:app_data_file:s0 /data/local/tmp/.hook_log");

            // Fallback: /sdcard/
            rootExec("rm -f /sdcard/.esp_shm /sdcard/.hook_log");
            rootExec("dd if=/dev/zero of=/sdcard/.esp_shm bs=4096 count=1 2>/dev/null");
            rootExec("chmod 666 /sdcard/.esp_shm");
            rootExec("touch /sdcard/.hook_log; chmod 666 /sdcard/.hook_log");

            // 5. Iniciar overlay ANTES da injecao (para estar pronto)
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    startService(new Intent(MainActivity.this, OverlayService.class));
                }
            });

            // 6. INJETAR via wrapper script + setprop wrap
            updateStatus("Injecting hook...");

            // Garantir que o diretorio do jogo existe
            rootExec("mkdir -p " + gameDir);
            rootExec("chmod 711 " + gameDir);

            // Copiar .so para diretorio do jogo E /data/local/tmp/
            rootExec("cp " + hookDst + " " + gameDir + "/libHook.so");
            rootExec("chmod 755 " + gameDir + "/libHook.so");
            rootExec("chcon u:object_r:app_data_file:s0 " + gameDir + "/libHook.so");
            rootExec("chmod 755 " + hookDst);
            rootExec("chcon u:object_r:system_data_file:s0 " + hookDst);

            // Criar wrapper script — tenta /data/local/tmp/ primeiro (sempre existe),
            // depois game dir como fallback
            rootExec("echo '#!/system/bin/sh' > /data/local/tmp/wrap_hook.sh");
            rootExec("echo 'export LD_PRELOAD=" + hookDst + "' >> /data/local/tmp/wrap_hook.sh");
            rootExec("echo 'exec \"$@\"' >> /data/local/tmp/wrap_hook.sh");
            rootExec("chmod 755 /data/local/tmp/wrap_hook.sh");
            rootExec("chcon u:object_r:system_file:s0 /data/local/tmp/wrap_hook.sh");

            // Script 2: usa .so do game dir (fallback se namespace bloquear /data/local/tmp/)
            rootExec("echo '#!/system/bin/sh' > /data/local/tmp/wrap_hook2.sh");
            rootExec("echo 'export LD_PRELOAD=" + gameDir + "/libHook.so' >> /data/local/tmp/wrap_hook2.sh");
            rootExec("echo 'exec \"$@\"' >> /data/local/tmp/wrap_hook2.sh");
            rootExec("chmod 755 /data/local/tmp/wrap_hook2.sh");
            rootExec("chcon u:object_r:system_file:s0 /data/local/tmp/wrap_hook2.sh");

            // Habilitar debug mode PERSISTENTE (necessario para wrap em Android 10+)
            // --persistent garante que nao e limpo apos primeiro launch
            rootExec("am set-debug-app --persistent " + GAME_PACKAGE);
            // Se Magisk: resetprop para garantir debuggable
            rootExec("resetprop ro.debuggable 1 2>/dev/null");
            // KernelSU/APatch: ksud como fallback
            rootExec("ksud debug-setprop ro.debuggable 1 2>/dev/null");

            // Limpar wrap anterior
            rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");
            Thread.sleep(500);

            // Setar wrap property — usa /data/local/tmp/libHook.so (sempre existe)
            rootExec("setprop wrap." + GAME_PACKAGE + " /data/local/tmp/wrap_hook.sh");

            // Verificar se property foi setado
            String wrapCheck = rootExec("getprop wrap." + GAME_PACKAGE);
            if (wrapCheck == null || !wrapCheck.contains("wrap_hook")) {
                // Fallback: tentar setprop via resetprop (Magisk)
                rootExec("resetprop wrap." + GAME_PACKAGE + " /data/local/tmp/wrap_hook.sh 2>/dev/null");
            }

            // Limpar logcat para diagnostico limpo
            rootExec("logcat -c 2>/dev/null");

            // Kill e reiniciar o jogo (UMA VEZ so)
            rootExec("am force-stop " + GAME_PACKAGE);
            Thread.sleep(1500);

            final String launcherAct = getLauncherActivity(GAME_PACKAGE);
            if (launcherAct != null && launcherAct.length() > 1) {
                rootExec("am start -n " + GAME_PACKAGE + "/" + launcherAct);
            } else {
                rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1");
            }

            updateStatus("Waiting for hook...");

            // 7. Aguardar hook carregar — checa game dir + fallbacks
            boolean hookOk = false;
            for (int i = 0; i < 30; i++) {
                Thread.sleep(1000);

                // Ler hook log (game dir primeiro, depois fallbacks)
                String hookLog = rootExec("cat " + gameDir + "/.hook_log 2>/dev/null");
                if (hookLog == null || hookLog.trim().isEmpty()) {
                    hookLog = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null");
                }
                if (hookLog == null || hookLog.trim().isEmpty()) {
                    hookLog = rootExec("cat /sdcard/.hook_log 2>/dev/null");
                }

                // HOOK CARREGADO ou HOOK ATIVO = sucesso
                if (hookLog != null && (hookLog.contains("HOOK ATIVO") || hookLog.contains("HOOK CARREGADO"))) {
                    hookOk = true;
                    break;
                }

                // Verificar PID e maps
                String pidResult = rootExec("pidof " + GAME_PACKAGE);
                if (pidResult != null && !pidResult.trim().isEmpty()) {
                    final String gamePid = pidResult.trim().split("\\s+")[0];
                    String maps = rootExec("grep -c libHook.so /proc/" + gamePid + "/maps 2>/dev/null");
                    final String mapsInfo = (maps != null) ? maps.trim() : "0";

                    // Se hook esta nos maps mas nao escreveu log, pode estar carregando
                    if (!"0".equals(mapsInfo) && mapsInfo.length() > 0) {
                        // Hook esta carregado! Esperar mais um pouco
                        if (i > 15) {
                            hookOk = true; // Nos maps = carregou
                            break;
                        }
                    }

                    final int sec = i + 1;
                    String logTail = "";
                    if (hookLog != null && hookLog.length() > 0) {
                        logTail = hookLog.length() > 150 ? hookLog.substring(hookLog.length() - 150) : hookLog;
                    }
                    updateStatus("Injecting... (" + sec + "s)");
                } else {
                    final int sec = i + 1;
                    updateStatus("Starting game... (" + sec + "s)");
                }
            }

            // 8. Limpar wrap property
            rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");

            // 9. Se metodo 1 falhou, tentar wrap_hook2.sh (game dir path) + reiniciar
            if (!hookOk) {
                updateStatus("Retry with alt path...");
                rootExec("am set-debug-app --persistent " + GAME_PACKAGE);
                rootExec("setprop wrap." + GAME_PACKAGE + " /data/local/tmp/wrap_hook2.sh");
                rootExec("am force-stop " + GAME_PACKAGE);
                Thread.sleep(1500);
                if (launcherAct != null && launcherAct.length() > 1) {
                    rootExec("am start -n " + GAME_PACKAGE + "/" + launcherAct);
                } else {
                    rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1");
                }

                for (int r = 0; r < 20; r++) {
                    Thread.sleep(1000);
                    String hookLog2 = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null");
                    if (hookLog2 == null || hookLog2.trim().isEmpty()) {
                        hookLog2 = rootExec("cat " + gameDir + "/.hook_log 2>/dev/null");
                    }
                    if (hookLog2 != null && (hookLog2.contains("HOOK ATIVO") || hookLog2.contains("HOOK CARREGADO"))) {
                        hookOk = true;
                        break;
                    }
                    String pidR = rootExec("pidof " + GAME_PACKAGE);
                    if (pidR != null && !pidR.trim().isEmpty()) {
                        String m = rootExec("grep -c libHook.so /proc/" + pidR.trim().split("\\s+")[0] + "/maps 2>/dev/null");
                        if (m != null && !"0".equals(m.trim()) && m.trim().length() > 0) {
                            hookOk = true;
                            break;
                        }
                    }
                    final int sec2 = r + 1;
                    updateStatus("Retry alt... (" + sec2 + "s)");
                }
                rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");
            }

            // 10. Se ainda falhou, tentar attach-agent no processo rodando
            if (!hookOk) {
                String pidResult = rootExec("pidof " + GAME_PACKAGE);
                if (pidResult != null && !pidResult.trim().isEmpty()) {
                    final String gamePid = pidResult.trim().split("\\s+")[0];
                    updateStatus("Attach-agent injection...");

                    // Limpar hook log
                    rootExec("echo '' > " + gameDir + "/.hook_log 2>/dev/null");
                    rootExec("echo '' > /data/local/tmp/.hook_log 2>/dev/null");

                    // Tentar attach com ambos os paths
                    rootExec("cmd activity attach-agent " + gamePid + " " + hookDst);
                    if ("0".equals(rootExec("grep -c libHook.so /proc/" + gamePid + "/maps 2>/dev/null"))) {
                        rootExec("cmd activity attach-agent " + gamePid + " " + gameDir + "/libHook.so");
                    }

                    for (int j = 0; j < 15; j++) {
                        Thread.sleep(1000);
                        String hookLog3 = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null");
                        if (hookLog3 == null || hookLog3.trim().isEmpty()) {
                            hookLog3 = rootExec("cat " + gameDir + "/.hook_log 2>/dev/null");
                        }
                        if (hookLog3 != null && (hookLog3.contains("HOOK ATIVO") || hookLog3.contains("HOOK CARREGADO"))) {
                            hookOk = true;
                            break;
                        }
                        String m = rootExec("grep -c libHook.so /proc/" + gamePid + "/maps 2>/dev/null");
                        if (m != null && !"0".equals(m.trim()) && m.trim().length() > 0) {
                            hookOk = true;
                            break;
                        }
                        final int sec3 = j + 1;
                        updateStatus("Attach... (" + sec3 + "s)");
                    }
                }
            }

            // 10. Status final
            if (hookOk) {
                showLoading(false);
                updateStatus("Injected successfully");
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        btnStop.setVisibility(View.VISIBLE);
                    }
                });
            } else {
                updateStatus("Injection failed. Retry?");
            }

            resetButton();

        } catch (final Exception e) {
            updateStatus("Error: " + e.getMessage());
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
