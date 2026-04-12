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

            // 4b. Pre-criar hook log files (hook escreve diagnostico aqui)
            rootExec("rm -f /data/local/tmp/.hook_log /sdcard/.hook_log");
            rootExec("touch /data/local/tmp/.hook_log; chmod 666 /data/local/tmp/.hook_log; chcon u:object_r:app_data_file:s0 /data/local/tmp/.hook_log");
            rootExec("touch /sdcard/.hook_log; chmod 666 /sdcard/.hook_log");

            // 5. Iniciar overlay ANTES da injecao (para estar pronto)
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    startService(new Intent(MainActivity.this, OverlayService.class));
                }
            });

            // 6. INJETAR via wrapper script + setprop wrap
            // wrap.<package> diz ao Zygote qual script usar ao criar o processo
            // O script seta LD_PRELOAD e exec o processo original
            updateStatus("Root OK\nlibHook.so OK\nCriando wrapper script...");

            // Copiar .so para diretorio do jogo tambem (garante acessibilidade)
            rootExec("cp " + hookDst + " /data/data/" + GAME_PACKAGE + "/libHook.so");
            rootExec("chmod 755 /data/data/" + GAME_PACKAGE + "/libHook.so");
            rootExec("chcon u:object_r:app_data_file:s0 /data/data/" + GAME_PACKAGE + "/libHook.so");

            // Criar wrapper script CORRETAMENTE (cada echo = uma linha)
            rootExec("echo '#!/system/bin/sh' > /data/local/tmp/wrap_hook.sh");
            rootExec("echo 'export LD_PRELOAD=" + hookDst + "' >> /data/local/tmp/wrap_hook.sh");
            rootExec("echo 'exec \"$@\"' >> /data/local/tmp/wrap_hook.sh");
            rootExec("chmod 755 /data/local/tmp/wrap_hook.sh");
            rootExec("chcon u:object_r:system_file:s0 /data/local/tmp/wrap_hook.sh");

            // Script alternativo com path dentro do dir do jogo
            rootExec("echo '#!/system/bin/sh' > /data/local/tmp/wrap_hook2.sh");
            rootExec("echo 'export LD_PRELOAD=/data/data/" + GAME_PACKAGE + "/libHook.so' >> /data/local/tmp/wrap_hook2.sh");
            rootExec("echo 'exec \"$@\"' >> /data/local/tmp/wrap_hook2.sh");
            rootExec("chmod 755 /data/local/tmp/wrap_hook2.sh");
            rootExec("chcon u:object_r:system_file:s0 /data/local/tmp/wrap_hook2.sh");

            // Habilitar debug mode (necessario para wrap funcionar em Android 10+)
            rootExec("am set-debug-app " + GAME_PACKAGE);

            // Limpar prop anterior
            rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");
            Thread.sleep(500);

            // Setar wrap property para o SCRIPT (nao bare LD_PRELOAD=)
            rootExec("setprop wrap." + GAME_PACKAGE + " /data/local/tmp/wrap_hook.sh");
            updateStatus("Root OK\nlibHook.so OK\nwrap script criado\nReiniciando jogo...");

            // Verificar que script foi criado corretamente
            String scriptContent = rootExec("cat /data/local/tmp/wrap_hook.sh");
            updateStatus("Root OK\nScript: " + (scriptContent != null ? scriptContent.trim().substring(0, Math.min(scriptContent.trim().length(), 100)) : "ERRO"));

            // Kill e reiniciar o jogo
            rootExec("am force-stop " + GAME_PACKAGE);
            Thread.sleep(1000);

            // Iniciar jogo — Zygote usa wrap script automaticamente
            final String launcherAct = getLauncherActivity(GAME_PACKAGE);
            if (launcherAct != null && launcherAct.length() > 1) {
                rootExec("am start -n " + GAME_PACKAGE + "/" + launcherAct);
            } else {
                rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1");
            }

            updateStatus("Root OK\nJogo iniciado com wrap script\nAguardando hook...");

            // 7. Aguardar hook carregar (via hook log, nao file size)
            boolean hookOk = false;
            for (int i = 0; i < 20; i++) {
                Thread.sleep(1000);

                // Ler hook log (escrito pelo hook quando carrega)
                String hookLog = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null");
                if (hookLog == null || hookLog.trim().isEmpty()) {
                    hookLog = rootExec("cat /sdcard/.hook_log 2>/dev/null");
                }

                // Hook escreveu "HOOK ATIVO" = tudo OK
                if (hookLog != null && hookLog.contains("HOOK ATIVO")) {
                    hookOk = true;
                    break;
                }

                // Hook escreveu "HOOK CARREGADO" = carregou mas pode ter crashado
                boolean hookLoaded = (hookLog != null && hookLog.contains("HOOK CARREGADO"));

                // Verificar se libHook.so aparece nos maps do jogo
                String pidResult = rootExec("pidof " + GAME_PACKAGE);
                if (pidResult != null && !pidResult.trim().isEmpty()) {
                    final String gamePid = pidResult.trim().split("\\s+")[0];
                    String maps = rootExec("grep -c libHook.so /proc/" + gamePid + "/maps 2>/dev/null");
                    final String mapsInfo = (maps != null) ? maps.trim() : "0";
                    // Capturar erros do linker
                    String linkerErr = rootExec("logcat -d -s linker:* | grep -i '" + GAME_PACKAGE + "\\|libHook\\|PRELOAD' | tail -3 2>/dev/null");
                    final String linkerInfo = (linkerErr != null && !linkerErr.trim().isEmpty()) ? linkerErr.trim() : "";
                    final int sec = i + 1;
                    String logTail = "";
                    if (hookLog != null && hookLog.length() > 0) {
                        logTail = hookLog.length() > 150 ? hookLog.substring(hookLog.length() - 150) : hookLog;
                    }
                    updateStatus("Metodo 1: wrap script (" + sec + "s)\n"
                            + "PID: " + gamePid + "\n"
                            + "Hook maps: " + mapsInfo
                            + (hookLoaded ? " | Hook CARREGADO!" : "") + "\n"
                            + (linkerInfo.length() > 0 ? "Linker: " + linkerInfo + "\n" : "")
                            + "Log: " + logTail);
                } else {
                    final int sec = i + 1;
                    updateStatus("Aguardando jogo iniciar... (" + sec + "s)");
                }
            }

            // 8. Limpar wrap property (so precisa na inicializacao)
            rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");

            // 9. Se wrap script nao funcionou, tentar alternativas
            if (!hookOk) {
                updateStatus("Metodo 1 falhou\nTentando metodo 2: script com path do jogo...");

                // METODO 2: Script com .so no diretorio do jogo
                rootExec("setprop wrap." + GAME_PACKAGE + " /data/local/tmp/wrap_hook2.sh");
                rootExec("am force-stop " + GAME_PACKAGE);
                Thread.sleep(500);
                // Re-criar arquivos
                rootExec("dd if=/dev/zero of=/data/local/tmp/.esp_shm bs=4096 count=1 2>/dev/null");
                rootExec("chmod 666 /data/local/tmp/.esp_shm");
                rootExec("dd if=/dev/zero of=/sdcard/.esp_shm bs=4096 count=1 2>/dev/null");
                rootExec("chmod 666 /sdcard/.esp_shm");
                rootExec("rm -f /data/local/tmp/.hook_log /sdcard/.hook_log");
                rootExec("touch /data/local/tmp/.hook_log; chmod 666 /data/local/tmp/.hook_log");
                rootExec("touch /sdcard/.hook_log; chmod 666 /sdcard/.hook_log");
                Thread.sleep(500);
                if (launcherAct != null && launcherAct.length() > 1) {
                    rootExec("am start -n " + GAME_PACKAGE + "/" + launcherAct);
                } else {
                    rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1");
                }

                // Aguardar
                for (int i = 0; i < 15; i++) {
                    Thread.sleep(1000);
                    String hookLog2 = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null");
                    if (hookLog2 == null || hookLog2.trim().isEmpty()) {
                        hookLog2 = rootExec("cat /sdcard/.hook_log 2>/dev/null");
                    }
                    if (hookLog2 != null && hookLog2.contains("HOOK ATIVO")) {
                        hookOk = true;
                        break;
                    }
                    String pid2 = rootExec("pidof " + GAME_PACKAGE);
                    if (pid2 != null && !pid2.trim().isEmpty()) {
                        final String gp = pid2.trim().split("\\s+")[0];
                        String m = rootExec("grep -c libHook.so /proc/" + gp + "/maps 2>/dev/null");
                        final int sec = i + 1;
                        updateStatus("Metodo 2: data dir (" + sec + "s)\n"
                                + "PID: " + gp + " | Hook maps: " + (m != null ? m.trim() : "0"));
                    }
                }
                rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");
            }

            // METODO 3: attach-agent (Android 9+, injeta no processo rodando)
            if (!hookOk) {
                updateStatus("Metodo 2 falhou\nTentando metodo 3: attach-agent...");

                String pidResult = rootExec("pidof " + GAME_PACKAGE);
                if (pidResult != null && !pidResult.trim().isEmpty()) {
                    final String gamePid = pidResult.trim().split("\\s+")[0];

                    // Limpar logs
                    rootExec("rm -f /data/local/tmp/.hook_log /sdcard/.hook_log");
                    rootExec("touch /data/local/tmp/.hook_log; chmod 666 /data/local/tmp/.hook_log");
                    rootExec("touch /sdcard/.hook_log; chmod 666 /sdcard/.hook_log");

                    // attach-agent injeta .so no processo rodando (chama dlopen internamente)
                    rootExec("cmd activity attach-agent " + gamePid + " " + hookDst);

                    // Tambem tentar com path do dir do jogo
                    rootExec("cmd activity attach-agent " + gamePid + " /data/data/" + GAME_PACKAGE + "/libHook.so");

                    for (int i = 0; i < 15; i++) {
                        Thread.sleep(1000);
                        String hookLog3 = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null");
                        if (hookLog3 == null || hookLog3.trim().isEmpty()) {
                            hookLog3 = rootExec("cat /sdcard/.hook_log 2>/dev/null");
                        }
                        if (hookLog3 != null && hookLog3.contains("HOOK ATIVO")) {
                            hookOk = true;
                            break;
                        }
                        if (hookLog3 != null && hookLog3.contains("HOOK CARREGADO")) {
                            hookOk = true;
                            break;
                        }
                        String m = rootExec("grep -c libHook.so /proc/" + gamePid + "/maps 2>/dev/null");
                        final int sec = i + 1;
                        String logTail3 = (hookLog3 != null && hookLog3.length() > 0) ? hookLog3.trim() : "vazio";
                        updateStatus("Metodo 3: attach-agent (" + sec + "s)\n"
                                + "PID: " + gamePid + " | Hook maps: " + (m != null ? m.trim() : "0")
                                + "\nLog: " + logTail3);
                    }
                }
            }

            // METODO 4: resetprop (Magisk) + wrap
            if (!hookOk) {
                updateStatus("Metodo 3 falhou\nTentando metodo 4: resetprop + wrap...");

                // Se Magisk esta instalado, resetprop pode mudar ro.debuggable
                rootExec("resetprop ro.debuggable 1 2>/dev/null");
                rootExec("setprop wrap." + GAME_PACKAGE + " /data/local/tmp/wrap_hook.sh");
                rootExec("am force-stop " + GAME_PACKAGE);
                Thread.sleep(500);
                rootExec("rm -f /data/local/tmp/.hook_log /sdcard/.hook_log");
                rootExec("touch /data/local/tmp/.hook_log; chmod 666 /data/local/tmp/.hook_log");
                rootExec("touch /sdcard/.hook_log; chmod 666 /sdcard/.hook_log");
                rootExec("dd if=/dev/zero of=/data/local/tmp/.esp_shm bs=4096 count=1 2>/dev/null");
                rootExec("chmod 666 /data/local/tmp/.esp_shm");
                Thread.sleep(500);
                if (launcherAct != null && launcherAct.length() > 1) {
                    rootExec("am start -n " + GAME_PACKAGE + "/" + launcherAct);
                } else {
                    rootExec("monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1");
                }

                for (int i = 0; i < 15; i++) {
                    Thread.sleep(1000);
                    String hookLog4 = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null");
                    if (hookLog4 == null || hookLog4.trim().isEmpty()) {
                        hookLog4 = rootExec("cat /sdcard/.hook_log 2>/dev/null");
                    }
                    if (hookLog4 != null && (hookLog4.contains("HOOK ATIVO") || hookLog4.contains("HOOK CARREGADO"))) {
                        hookOk = true;
                        break;
                    }
                    final int sec = i + 1;
                    updateStatus("Metodo 4: resetprop (" + sec + "s)");
                }
                rootExec("setprop wrap." + GAME_PACKAGE + " \"\"");
            }

            // 10. Status final
            if (hookOk) {
                updateStatus("TUDO ATIVO!\n"
                        + "Hook: VMT (MethodInfo swap)\n"
                        + "IPC: /data/local/tmp/.esp_shm\n"
                        + "ESP: Toggle no menu overlay");
            } else {
                // Mostrar diagnostico completo
                String pidResult = rootExec("pidof " + GAME_PACKAGE);
                final String diagPid = (pidResult != null) ? pidResult.trim() : "N/A";
                String maps = "";
                String gameArch = "";
                if (!diagPid.equals("N/A") && !diagPid.isEmpty()) {
                    String firstPid = diagPid.split("\\s+")[0];
                    maps = rootExec("grep Hook /proc/" + firstPid + "/maps 2>/dev/null");
                    // Verificar arquitetura do jogo (32 vs 64 bit)
                    gameArch = rootExec("cat /proc/" + firstPid + "/maps | head -1 | awk '{print length($1)}' 2>/dev/null");
                }
                final String diagMaps = (maps != null && !maps.isEmpty()) ? "SIM" : "NAO";
                // Arch: enderecos de 8 chars = 32bit, 12+ chars = 64bit
                String archInfo = "";
                if (gameArch != null && !gameArch.trim().isEmpty()) {
                    try {
                        int addrLen = Integer.parseInt(gameArch.trim());
                        archInfo = (addrLen <= 8) ? "32-bit" : "64-bit";
                    } catch (NumberFormatException ignored) {}
                }
                String prop = rootExec("getprop wrap." + GAME_PACKAGE);
                final String diagProp = (prop != null) ? prop.trim() : "vazio";
                // Script content
                String scriptCheck = rootExec("cat /data/local/tmp/wrap_hook.sh 2>/dev/null");
                final String diagScript = (scriptCheck != null && !scriptCheck.trim().isEmpty()) ? scriptCheck.trim() : "N/A";
                // Logcat do hook
                String logcatHook = rootExec("logcat -d -s GameHook:* | tail -5");
                final String diagLog = (logcatHook != null && !logcatHook.trim().isEmpty()) ? logcatHook.trim() : "sem logs";
                // Logcat do linker
                String logcatLinker = rootExec("logcat -d | grep -i 'linker.*Hook\\|PRELOAD\\|dlopen.*Hook' | tail -5 2>/dev/null");
                final String diagLinker = (logcatLinker != null && !logcatLinker.trim().isEmpty()) ? logcatLinker.trim() : "sem erros linker";
                // Hook log file
                String hookLogDiag = rootExec("cat /data/local/tmp/.hook_log 2>/dev/null || cat /sdcard/.hook_log 2>/dev/null");
                final String diagHookLog = (hookLogDiag != null && !hookLogDiag.trim().isEmpty()) ? hookLogDiag.trim() : "vazio";
                // resetprop check
                String hasMagisk = rootExec("which resetprop 2>/dev/null");
                final String diagMagisk = (hasMagisk != null && !hasMagisk.trim().isEmpty()) ? "SIM" : "NAO";

                updateStatus("HOOK NAO CARREGOU\n"
                        + "Jogo PID: " + diagPid + " (" + archInfo + ")\n"
                        + "Hook maps: " + diagMaps + "\n"
                        + "wrap prop: " + diagProp + "\n"
                        + "Magisk: " + diagMagisk + "\n"
                        + "Script:\n" + diagScript + "\n"
                        + "HookLog: " + diagHookLog + "\n"
                        + "Linker: " + diagLinker + "\n"
                        + "Logcat: " + diagLog);
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
