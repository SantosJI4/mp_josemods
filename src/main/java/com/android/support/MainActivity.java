package com.android.support;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
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
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        tvStatus = findViewById(R.id.tvStatus);
        tvInfo = findViewById(R.id.tvInfo);
        btnStart = findViewById(R.id.btnStart);
        btnStop = findViewById(R.id.btnStop);
        handler = new Handler(Looper.getMainLooper());

        btnStart.setOnClickListener(v -> onStartClicked());
        btnStop.setOnClickListener(v -> onStopClicked());

        // Checar permissão de overlay
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            setStatus("Precisamos da permissão de overlay...");
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
                setStatus("Permissão OK. Abra o jogo e toque INICIAR.");
            } else {
                setStatus("[ERRO] Permissão de overlay negada.");
            }
        }
    }

    private void onStartClicked() {
        if (injecting) return;

        // Verificar permissão de overlay
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            Toast.makeText(this, "Permissão de overlay necessária", Toast.LENGTH_SHORT).show();
            return;
        }

        injecting = true;
        btnStart.setEnabled(false);
        setStatus("Iniciando...");

        // Tudo roda em background thread (su commands bloqueiam)
        new Thread(this::doInjectAndStart).start();
    }

    private void onStopClicked() {
        new Thread(() -> {
            // Parar overlay
            runOnUi(() -> {
                stopService(new Intent(this, OverlayService.class));
                setStatus("Overlay parado.");
            });

            // Limpar shared memory
            rootExec("rm -f /data/local/tmp/.esp_shm");
        }).start();
    }

    // ══════════════════════════════════════
    // LÓGICA DE INJEÇÃO — roda em background
    // ══════════════════════════════════════
    private void doInjectAndStart() {
        try {
            // ── 1. Verificar root ──
            runOnUi(() -> setStatus("Verificando root..."));
            String rootCheck = rootExec("id");
            if (rootCheck == null || !rootCheck.contains("uid=0")) {
                runOnUi(() -> {
                    setStatus("[ERRO] Sem acesso root!\nO app precisa de root para injetar o hook.");
                    btnStart.setEnabled(true);
                    injecting = false;
                });
                return;
            }
            runOnUi(() -> setStatus("Root OK ✓"));

            // ── 2. SELinux permissive ──
            rootExec("setenforce 0");

            // ── 3. Extrair libHook.so do APK para /data/local/tmp/ ──
            runOnUi(() -> setStatus("Root OK ✓\nCopying libHook.so..."));
            String nativeLibDir = getApplicationInfo().nativeLibraryDir;
            String hookSrc = nativeLibDir + "/libHook.so";
            String hookDst = "/data/local/tmp/libHook.so";

            // O Android.mk compila libHook.so junto com libMEOW.so no APK
            if (!new File(hookSrc).exists()) {
                runOnUi(() -> {
                    setStatus("[ERRO] libHook.so não encontrada no APK!\n" +
                              "Verifique se Android.mk está compilando o módulo Hook.");
                    btnStart.setEnabled(true);
                    injecting = false;
                });
                return;
            }
            rootExec("cp " + hookSrc + " " + hookDst);
            rootExec("chmod 755 " + hookDst);

            // ── 4. Limpar shared memory antiga ──
            rootExec("rm -f /data/local/tmp/.esp_shm");

            // ── 5. Verificar se o jogo está aberto ──
            runOnUi(() -> setStatus("Root OK ✓\nlibHook.so OK ✓\nAguardando jogo: " + GAME_PACKAGE + " ..."));

            String gamePid = null;
            for (int i = 0; i < 30; i++) {
                gamePid = rootExec("pidof " + GAME_PACKAGE);
                if (gamePid != null && !gamePid.trim().isEmpty()) {
                    gamePid = gamePid.trim().split("\\s+")[0]; // Primeiro PID
                    break;
                }
                gamePid = null;
                Thread.sleep(1000);
            }

            if (gamePid == null) {
                runOnUi(() -> {
                    setStatus("[ERRO] Jogo não encontrado!\nAbra " + GAME_PACKAGE + " primeiro.");
                    btnStart.setEnabled(true);
                    injecting = false;
                });
                return;
            }

            final String pid = gamePid;
            runOnUi(() -> setStatus("Root OK ✓\nlibHook.so OK ✓\nJogo PID: " + pid + " ✓\nAguardando libil2cpp.so..."));

            // ── 6. Aguardar libil2cpp.so carregar ──
            boolean il2cppLoaded = false;
            for (int i = 0; i < 30; i++) {
                String maps = rootExec("grep libil2cpp.so /proc/" + pid + "/maps");
                if (maps != null && maps.contains("libil2cpp.so")) {
                    il2cppLoaded = true;
                    break;
                }
                Thread.sleep(1000);
            }

            if (!il2cppLoaded) {
                runOnUi(() -> {
                    setStatus("[ERRO] Timeout: libil2cpp.so não carregou em 30s");
                    btnStart.setEnabled(true);
                    injecting = false;
                });
                return;
            }

            runOnUi(() -> setStatus("Root OK ✓\nlibHook.so OK ✓\nJogo PID: " + pid + " ✓\nlibil2cpp.so ✓\nEsperando assemblies..."));
            Thread.sleep(3000); // Aguardar assemblies carregarem

            // ── 7. Copiar para diretório do jogo ──
            String gameDir = "/data/data/" + GAME_PACKAGE;
            String hookInGame = gameDir + "/libHook.so";
            rootExec("cp " + hookDst + " " + hookInGame);
            rootExec("chmod 755 " + hookInGame);

            // ── 8. INJETAR ──
            runOnUi(() -> setStatus("Root OK ✓\nlibHook.so OK ✓\nJogo PID: " + pid + " ✓\nlibil2cpp.so ✓\nInjetando hook..."));

            // ────────────────────────────────────────────
            // MÉTODO DE INJEÇÃO — Escolha UM:
            // ────────────────────────────────────────────

            // MÉTODO 1: LD_PRELOAD (reinicia o jogo)
            // Simples e funcional. O jogo reinicia com o hook carregado.
            rootExec("am force-stop " + GAME_PACKAGE);
            Thread.sleep(1000);
            // Pega a activity principal do jogo
            String launchCmd = "LD_PRELOAD=" + hookInGame
                    + " am start -n " + GAME_PACKAGE + "/"
                    + getLauncherActivity(GAME_PACKAGE);
            rootExec(launchCmd);

            // MÉTODO 2: Injector ptrace (sem reiniciar)
            // Descomente e comente o MÉTODO 1 se tiver um injector:
            // rootExec("/data/local/tmp/injector " + pid + " " + hookInGame);

            // ── 9. Aguardar hook ativar (shared memory aparece) ──
            runOnUi(() -> setStatus("Root OK ✓\nlibHook.so OK ✓\nJogo reiniciado ✓\nAguardando hook ativar..."));

            boolean hookOk = false;
            for (int i = 0; i < 15; i++) {
                String check = rootExec("ls /data/local/tmp/.esp_shm 2>/dev/null");
                if (check != null && check.contains(".esp_shm")) {
                    hookOk = true;
                    break;
                }
                Thread.sleep(1000);
            }

            // ── 10. Iniciar overlay ──
            final boolean hookResult = hookOk;
            runOnUi(() -> {
                if (hookResult) {
                    setStatus("Root OK ✓\nlibHook.so OK ✓\nJogo OK ✓\nHook ATIVO ✓\nOverlay iniciando...");
                } else {
                    setStatus("Root OK ✓\nlibHook.so OK ✓\nJogo OK ✓\nHook: aguardando... (overlay vai conectar quando disponível)");
                }

                // Permissão overlay
                rootExec("appops set " + getPackageName() + " SYSTEM_ALERT_WINDOW allow");

                // Iniciar overlay service
                startService(new Intent(this, OverlayService.class));

                setStatus((hookResult
                        ? "TUDO ATIVO ✓\n"
                        : "OVERLAY ATIVO (hook aguardando...)\n")
                        + "Hook: VMT (MethodInfo swap)\n"
                        + "IPC: /data/local/tmp/.esp_shm\n"
                        + "ESP: Ative no menu do overlay");

                btnStart.setEnabled(true);
                injecting = false;
            });

        } catch (Exception e) {
            runOnUi(() -> {
                setStatus("[ERRO] " + e.getMessage());
                btnStart.setEnabled(true);
                injecting = false;
            });
        }
    }

    // ══════════════════════════════════════
    // Helpers
    // ══════════════════════════════════════

    /** Executa comando como root e retorna stdout */
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

    /** Descobre a activity principal (launcher) do jogo */
    private String getLauncherActivity(String pkg) {
        String result = rootExec("cmd package resolve-activity --brief " + pkg
                + " | tail -1");
        if (result != null && result.contains("/")) {
            // Retorna algo como "com.game/.MainActivity"
            String activity = result.trim();
            if (activity.contains(pkg)) {
                return activity.substring(activity.indexOf("/"));
            }
        }
        // Fallback: tentar dumpsys
        result = rootExec("dumpsys package " + pkg
                + " | grep -A1 'android.intent.action.MAIN' | grep -o '"
                + pkg + "/[^ ]*'");
        if (result != null && result.contains("/")) {
            return result.trim().substring(result.indexOf("/"));
        }
        // Último fallback
        return "/.MainActivity";
    }

    private void setStatus(String text) {
        tvStatus.setText(text);
    }

    private void runOnUi(Runnable r) {
        handler.post(r);
    }
}
