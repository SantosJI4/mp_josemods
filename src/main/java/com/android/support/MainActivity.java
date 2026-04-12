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

            // 4. Pre-criar shared memory com permissoes abertas
            // O jogo roda como UID do app, nao como root
            // Entao precisamos criar o arquivo ANTES com chmod 666
            rootExec("rm -f /data/local/tmp/.esp_shm");
            rootExec("rm -f /sdcard/.esp_shm");
            // Criar arquivos com tamanho correto (4096 bytes zerados)
            // Se criar vazio (0 bytes), mmap causa SIGBUS
            rootExec("dd if=/dev/zero of=/data/local/tmp/.esp_shm bs=4096 count=1 2>/dev/null");
            rootExec("chmod 666 /data/local/tmp/.esp_shm");
            rootExec("chcon u:object_r:app_data_file:s0 /data/local/tmp/.esp_shm");
            // Fallback no sdcard
            rootExec("dd if=/dev/zero of=/sdcard/.esp_shm bs=4096 count=1 2>/dev/null");
            rootExec("chmod 666 /sdcard/.esp_shm");

            // 5. Iniciar overlay ANTES da injecao (para estar pronto)
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    startService(new Intent(MainActivity.this, OverlayService.class));
                }
            });

            // 6. INJETAR via setprop wrap (metodo correto para Android)
            // O wrap property diz ao Zygote para configurar LD_PRELOAD quando criar o processo do app
            updateStatus("Root OK\nlibHook.so OK\nConfigurando injecao...");

            // Limpar prop anterior
            rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");
            Thread.sleep(500);

            // Setar wrap property — Zygote vai usar isso ao criar o processo
            rootExec("setprop wrap." + GAME_PACKAGE + " LD_PRELOAD=" + hookDst);
            updateStatus("Root OK\nlibHook.so OK\nwrap property setada\nReiniciando jogo...");

            // Kill e reiniciar o jogo
            rootExec("am force-stop " + GAME_PACKAGE);
            Thread.sleep(1000);

            // Iniciar jogo normalmente — o wrap property injeta automaticamente
            final String launcherAct = getLauncherActivity(GAME_PACKAGE);
            if (launcherAct != null && launcherAct.length() > 1) {
                rootExec("am start -n " + GAME_PACKAGE + "/" + launcherAct);
            } else {
                rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1");
            }

            updateStatus("Root OK\nlibHook.so OK\nJogo iniciado com LD_PRELOAD\nAguardando hook...");

            // 7. Aguardar hook criar shared memory (prova que carregou)
            boolean hookOk = false;
            for (int i = 0; i < 30; i++) {
                Thread.sleep(1000);

                // Verificar se o hook escreveu na shared memory
                // O arquivo foi pre-criado (0 bytes), hook faz ftruncate(4096) + escreve magic
                String check = rootExec("wc -c < /data/local/tmp/.esp_shm 2>/dev/null");
                boolean shmReady = false;
                if (check != null) {
                    try {
                        int sz = Integer.parseInt(check.trim());
                        if (sz >= 4096) shmReady = true;
                    } catch (NumberFormatException ignored) {}
                }
                if (!shmReady) {
                    // Checar fallback sdcard
                    check = rootExec("wc -c < /sdcard/.esp_shm 2>/dev/null");
                    if (check != null) {
                        try {
                            int sz = Integer.parseInt(check.trim());
                            if (sz >= 4096) shmReady = true;
                        } catch (NumberFormatException ignored) {}
                    }
                }
                if (shmReady) {
                    hookOk = true;
                    break;
                }

                // Verificar se libHook.so aparece nos maps do jogo
                String pidResult = rootExec("pidof " + GAME_PACKAGE);
                if (pidResult != null && !pidResult.trim().isEmpty()) {
                    final String gamePid = pidResult.trim().split("\\s+")[0];
                    String maps = rootExec("grep -c libHook.so /proc/" + gamePid + "/maps 2>/dev/null");
                    final String mapsInfo = (maps != null) ? maps.trim() : "0";
                    String il2cpp = rootExec("grep -c libil2cpp.so /proc/" + gamePid + "/maps 2>/dev/null");
                    final String il2cppInfo = (il2cpp != null) ? il2cpp.trim() : "0";
                    final int sec = i + 1;
                    updateStatus("Aguardando hook... (" + sec + "s)\n"
                            + "PID: " + gamePid + "\n"
                            + "Hook nos maps: " + mapsInfo + "\n"
                            + "il2cpp nos maps: " + il2cppInfo);
                } else {
                    final int sec = i + 1;
                    updateStatus("Aguardando jogo iniciar... (" + sec + "s)");
                }
            }

            // 8. Limpar wrap property (so precisa na inicializacao)
            rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");

            // 9. Se wrap nao funcionou, tentar fallback
            if (!hookOk) {
                updateStatus("wrap.PACKAGE nao funcionou\nTentando fallback...");

                // Fallback: Copiar para dir do jogo e usar LD_PRELOAD direto
                String pidResult = rootExec("pidof " + GAME_PACKAGE);
                if (pidResult != null && !pidResult.trim().isEmpty()) {
                    final String gamePid = pidResult.trim().split("\\s+")[0];

                    // Tentar injecao via /proc/PID/mem (escrever em linker globals)
                    // ou via am start com --attach-agent (Android 9+)

                    // Metodo: force stop + LD_PRELOAD via shell script wrapper
                    rootExec("echo '#!/system/bin/sh\nexport LD_PRELOAD=" + hookDst
                            + "\nexec $@' > /data/local/tmp/wrap_hook.sh");
                    rootExec("chmod 755 /data/local/tmp/wrap_hook.sh");
                    rootExec("chcon u:object_r:system_file:s0 /data/local/tmp/wrap_hook.sh");

                    rootExec("setprop wrap." + GAME_PACKAGE + " /data/local/tmp/wrap_hook.sh");

                    rootExec("am force-stop " + GAME_PACKAGE);
                    Thread.sleep(500);
                    // Re-criar shm (game foi morto, pode ter ficado sujo)
                    rootExec("dd if=/dev/zero of=/data/local/tmp/.esp_shm bs=4096 count=1 2>/dev/null");
                    rootExec("chmod 666 /data/local/tmp/.esp_shm");
                    rootExec("dd if=/dev/zero of=/sdcard/.esp_shm bs=4096 count=1 2>/dev/null");
                    rootExec("chmod 666 /sdcard/.esp_shm");
                    Thread.sleep(500);
                    if (launcherAct != null && launcherAct.length() > 1) {
                        rootExec("am start -n " + GAME_PACKAGE + "/" + launcherAct);
                    } else {
                        rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1");
                    }

                    updateStatus("Fallback: wrap script\nAguardando hook...");

                    // Aguardar novamente
                    for (int i = 0; i < 20; i++) {
                        Thread.sleep(1000);
                        String check2 = rootExec("wc -c < /data/local/tmp/.esp_shm 2>/dev/null");
                        boolean ready2 = false;
                        if (check2 != null) {
                            try {
                                if (Integer.parseInt(check2.trim()) >= 4096) ready2 = true;
                            } catch (NumberFormatException ignored) {}
                        }
                        if (!ready2) {
                            check2 = rootExec("wc -c < /sdcard/.esp_shm 2>/dev/null");
                            if (check2 != null) {
                                try {
                                    if (Integer.parseInt(check2.trim()) >= 4096) ready2 = true;
                                } catch (NumberFormatException ignored) {}
                            }
                        }
                        if (ready2) {
                            hookOk = true;
                            break;
                        }
                        String pid2 = rootExec("pidof " + GAME_PACKAGE);
                        if (pid2 != null && !pid2.trim().isEmpty()) {
                            final String gp = pid2.trim().split("\\s+")[0];
                            String m = rootExec("grep -c libHook.so /proc/" + gp + "/maps 2>/dev/null");
                            final int sec = i + 1;
                            updateStatus("Fallback aguardando... (" + sec + "s)\n"
                                    + "PID: " + gp + " | Hook maps: " + (m != null ? m.trim() : "0"));
                        }
                    }

                    // Limpar
                    rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");
                }
            }

            // 10. Status final
            if (hookOk) {
                updateStatus("TUDO ATIVO!\n"
                        + "Hook: VMT (MethodInfo swap)\n"
                        + "IPC: /data/local/tmp/.esp_shm\n"
                        + "ESP: Toggle o botao verde no jogo");
            } else {
                // Mostrar diagnostico
                String pidResult = rootExec("pidof " + GAME_PACKAGE);
                final String diagPid = (pidResult != null) ? pidResult.trim() : "N/A";
                String maps = "";
                if (!diagPid.equals("N/A") && !diagPid.isEmpty()) {
                    maps = rootExec("grep Hook /proc/" + diagPid.split("\\s+")[0] + "/maps 2>/dev/null");
                }
                final String diagMaps = (maps != null && !maps.isEmpty()) ? "SIM" : "NAO";
                String prop = rootExec("getprop wrap." + GAME_PACKAGE);
                final String diagProp = (prop != null) ? prop.trim() : "vazio";
                String logcatHook = rootExec("logcat -d -s GameHook:* | tail -5");
                final String diagLog = (logcatHook != null) ? logcatHook.trim() : "sem logs";

                updateStatus("OVERLAY ATIVO (hook NAO carregou)\n"
                        + "PID jogo: " + diagPid + "\n"
                        + "Hook nos maps: " + diagMaps + "\n"
                        + "wrap prop: " + diagProp + "\n"
                        + "Logcat: " + diagLog + "\n"
                        + "\nTente: instale um injector em\n/data/local/tmp/injector");
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
