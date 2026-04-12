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
    private static final String GAME_PACKAGE = "com.fungames.sniper3d";

    private TextView tvStatus;
    private TextView tvInfo;
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
        tvInfo = (TextView) findViewById(R.id.tvInfo);
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
            setStatus("Precisamos da permissao de overlay...");
            Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivityForResult(intent, OVERLAY_PERMISSION_CODE);
        } else {
            setStatus("Pronto. Abra o jogo e toque INICIAR.");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == OVERLAY_PERMISSION_CODE) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && Settings.canDrawOverlays(this)) {
                setStatus("Permissao OK. Abra o jogo e toque INICIAR.");
            } else {
                setStatus("[ERRO] Permissao de overlay negada.");
            }
        }
    }

    private void onStartClicked() {
        if (injecting) return;

        // Verificar permissão de overlay
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            Toast.makeText(this, "Permissao de overlay necessaria", Toast.LENGTH_SHORT).show();
            return;
        }

        injecting = true;
        btnStart.setEnabled(false);
        setStatus("Iniciando...");

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
                        setStatus("Overlay parado.");
                    }
                });
                rootExec("rm -f /data/local/tmp/.esp_shm");
                rootExec("rm -f /data/local/tmp/.hook_log /sdcard/.hook_log");
                rootExec("rm -f /data/data/" + GAME_PACKAGE + "/.esp_shm /data/data/" + GAME_PACKAGE + "/.hook_log");
            }
        }).start();
    }

    // ══════════════════════════════════════
    // LOGICA DE INJECAO — roda em background
    // ══════════════════════════════════════
    private void doInjectAndStart() {
        try {
            // 1. Verificar root
            updateStatus("Verificando root...");
            final String rootCheck = rootExec("id");
            if (rootCheck == null || !rootCheck.contains("uid=0")) {
                updateStatus("[ERRO] Sem acesso root!");
                resetButton();
                return;
            }

            // 2. SELinux permissive
            rootExec("setenforce 0");
            updateStatus("Root OK\nSELinux permissive");

            // 3. Copiar libHook.so
            final String nativeLibDir = getApplicationInfo().nativeLibraryDir;
            final String hookSrc = nativeLibDir + "/libHook.so";
            final String hookDst = "/data/local/tmp/libHook.so";

            if (!new File(hookSrc).exists()) {
                updateStatus("[ERRO] libHook.so nao encontrada no APK!\nnativeLibDir: " + nativeLibDir);
                resetButton();
                return;
            }
            rootExec("cp " + hookSrc + " " + hookDst);
            rootExec("chmod 777 " + hookDst);
            // SELinux label para o linker aceitar
            rootExec("chcon u:object_r:system_lib_file:s0 " + hookDst);
            updateStatus("Root OK\nlibHook.so copiada");

            // 4. Pre-criar shared memory + hook log nos 3 locais possiveis
            // PRIMARIO: diretorio de dados do jogo (game pode SEMPRE escrever aqui)
            final String gameDir = "/data/data/" + GAME_PACKAGE;

            // Tornar diretorio do jogo traversavel pelo overlay (o+x)
            // Sem isso, nosso overlay (outro UID) nao entra em /data/data/game/
            rootExec("chmod 711 " + gameDir);
            rootExec("chmod 711 /data/user/0/" + GAME_PACKAGE + " 2>/dev/null");

            rootExec("dd if=/dev/zero of=" + gameDir + "/.esp_shm bs=4096 count=1 2>/dev/null");
            rootExec("chmod 666 " + gameDir + "/.esp_shm");
            rootExec("chcon u:object_r:app_data_file:s0 " + gameDir + "/.esp_shm");
            rootExec("rm -f " + gameDir + "/.hook_log");
            rootExec("touch " + gameDir + "/.hook_log; chmod 666 " + gameDir + "/.hook_log; chcon u:object_r:app_data_file:s0 " + gameDir + "/.hook_log");

            // Fallback: /data/local/tmp/ (pode nao ser acessivel pelo jogo)
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
            updateStatus("Root OK\nlibHook.so OK\nCriando wrapper script...");

            // Copiar .so para diretorio do jogo (garante acessibilidade no namespace)
            rootExec("cp " + hookDst + " " + gameDir + "/libHook.so");
            rootExec("chmod 755 " + gameDir + "/libHook.so");
            rootExec("chcon u:object_r:app_data_file:s0 " + gameDir + "/libHook.so");

            // Criar wrapper scripts (cada echo = uma linha separada)
            // Script 1: usa .so do diretorio do jogo (mais confiavel)
            rootExec("echo '#!/system/bin/sh' > /data/local/tmp/wrap_hook.sh");
            rootExec("echo 'export LD_PRELOAD=" + gameDir + "/libHook.so' >> /data/local/tmp/wrap_hook.sh");
            rootExec("echo 'exec \"$@\"' >> /data/local/tmp/wrap_hook.sh");
            rootExec("chmod 755 /data/local/tmp/wrap_hook.sh");
            rootExec("chcon u:object_r:system_file:s0 /data/local/tmp/wrap_hook.sh");

            // Script 2: usa .so de /data/local/tmp/ (fallback)
            rootExec("echo '#!/system/bin/sh' > /data/local/tmp/wrap_hook2.sh");
            rootExec("echo 'export LD_PRELOAD=" + hookDst + "' >> /data/local/tmp/wrap_hook2.sh");
            rootExec("echo 'exec \"$@\"' >> /data/local/tmp/wrap_hook2.sh");
            rootExec("chmod 755 /data/local/tmp/wrap_hook2.sh");
            rootExec("chcon u:object_r:system_file:s0 /data/local/tmp/wrap_hook2.sh");

            // Habilitar debug mode (necessario para wrap em Android 10+)
            rootExec("am set-debug-app " + GAME_PACKAGE);
            // Se Magisk: resetprop para garantir debuggable
            rootExec("resetprop ro.debuggable 1 2>/dev/null");

            // Limpar wrap anterior
            rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");
            Thread.sleep(300);

            // Setar wrap property para o script (usa .so do game dir)
            rootExec("setprop wrap." + GAME_PACKAGE + " /data/local/tmp/wrap_hook.sh");

            // Limpar logcat para diagnostico limpo
            rootExec("logcat -c 2>/dev/null");

            // Kill e reiniciar o jogo (UMA VEZ so)
            rootExec("am force-stop " + GAME_PACKAGE);
            Thread.sleep(1000);

            final String launcherAct = getLauncherActivity(GAME_PACKAGE);
            if (launcherAct != null && launcherAct.length() > 1) {
                rootExec("am start -n " + GAME_PACKAGE + "/" + launcherAct);
            } else {
                rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1");
            }

            updateStatus("Jogo iniciado com wrap script\nAguardando hook...");

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
                    updateStatus("Aguardando hook... (" + sec + "s)\n"
                            + "PID: " + gamePid + "\n"
                            + "Hook maps: " + mapsInfo + "\n"
                            + "Log: " + logTail);
                } else {
                    final int sec = i + 1;
                    updateStatus("Aguardando jogo iniciar... (" + sec + "s)");
                }
            }

            // 8. Limpar wrap property
            rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");

            // 9. Se metodo 1 falhou, tentar script com path alternativo (sem reiniciar jogo)
            if (!hookOk) {
                // Tentar attach-agent no processo rodando (nao mata o jogo)
                String pidResult = rootExec("pidof " + GAME_PACKAGE);
                if (pidResult != null && !pidResult.trim().isEmpty()) {
                    final String gamePid = pidResult.trim().split("\\s+")[0];
                    updateStatus("Wrap nao funcionou\nTentando attach-agent (PID: " + gamePid + ")...");

                    // Limpar hook log do game dir
                    rootExec("echo '' > " + gameDir + "/.hook_log 2>/dev/null");
                    rootExec("chmod 666 " + gameDir + "/.hook_log 2>/dev/null");

                    // attach-agent injeta via dlopen (Android 9+)
                    rootExec("cmd activity attach-agent " + gamePid + " " + gameDir + "/libHook.so");

                    for (int j = 0; j < 15; j++) {
                        Thread.sleep(1000);
                        String hookLog2 = rootExec("cat " + gameDir + "/.hook_log 2>/dev/null");
                        if (hookLog2 == null || hookLog2.trim().isEmpty()) {
                            hookLog2 = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null");
                        }
                        if (hookLog2 != null && (hookLog2.contains("HOOK ATIVO") || hookLog2.contains("HOOK CARREGADO"))) {
                            hookOk = true;
                            break;
                        }
                        String m = rootExec("grep -c libHook.so /proc/" + gamePid + "/maps 2>/dev/null");
                        if (m != null && !"0".equals(m.trim()) && m.trim().length() > 0) {
                            hookOk = true;
                            break;
                        }
                        final int sec = j + 1;
                        updateStatus("attach-agent (" + sec + "s)\nPID: " + gamePid);
                    }
                }
            }

            // 10. Status final
            if (hookOk) {
                updateStatus("TUDO ATIVO!\n"
                        + "Hook: VMT (MethodInfo swap)\n"
                        + "IPC: " + gameDir + "/.esp_shm\n"
                        + "ESP: Toggle no menu overlay");
            } else {
                // Diagnostico completo
                String pidResult2 = rootExec("pidof " + GAME_PACKAGE);
                final String diagPid = (pidResult2 != null) ? pidResult2.trim() : "N/A";
                String diagMapsStr = "NAO";
                if (!diagPid.equals("N/A") && !diagPid.isEmpty()) {
                    String mResult = rootExec("grep -c libHook.so /proc/" + diagPid.split("\\s+")[0] + "/maps 2>/dev/null");
                    diagMapsStr = (mResult != null && !mResult.trim().isEmpty() && !"0".equals(mResult.trim())) ? "SIM" : "NAO";
                }
                // Hook log de todos os locais
                String hookLogDiag = rootExec("cat " + gameDir + "/.hook_log 2>/dev/null");
                if (hookLogDiag == null || hookLogDiag.trim().isEmpty()) {
                    hookLogDiag = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null || cat /sdcard/.hook_log 2>/dev/null");
                }
                final String diagHookLog = (hookLogDiag != null && !hookLogDiag.trim().isEmpty()) ? hookLogDiag.trim() : "vazio";
                // Logcat (mais linhas)
                String logcatHook = rootExec("logcat -d -s GameHook:* | tail -10");
                final String diagLog = (logcatHook != null && !logcatHook.trim().isEmpty()) ? logcatHook.trim() : "sem logs";

                updateStatus("HOOK NAO CONFIRMADO\n"
                        + "PID: " + diagPid + " | Maps: " + diagMapsStr + "\n"
                        + "HookLog: " + diagHookLog + "\n"
                        + "Logcat:\n" + diagLog);
            }

            resetButton();

        } catch (final Exception e) {
            updateStatus("[ERRO] " + e.getMessage());
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
        runOnUi(new Runnable() {
            @Override
            public void run() {
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
