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
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    setStatus("Verificando root...");
                }
            });
            final String rootCheck = rootExec("id");
            if (rootCheck == null || !rootCheck.contains("uid=0")) {
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        setStatus("[ERRO] Sem acesso root!\nO app precisa de root.");
                        btnStart.setEnabled(true);
                        injecting = false;
                    }
                });
                return;
            }
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    setStatus("Root OK");
                }
            });

            // 2. SELinux permissive
            rootExec("setenforce 0");

            // 3. Extrair libHook.so do APK
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    setStatus("Root OK\nCopiando libHook.so...");
                }
            });
            final String nativeLibDir = getApplicationInfo().nativeLibraryDir;
            final String hookSrc = nativeLibDir + "/libHook.so";
            final String hookDst = "/data/local/tmp/libHook.so";

            if (!new File(hookSrc).exists()) {
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        setStatus("[ERRO] libHook.so nao encontrada no APK!\nVerifique Android.mk.");
                        btnStart.setEnabled(true);
                        injecting = false;
                    }
                });
                return;
            }
            rootExec("cp " + hookSrc + " " + hookDst);
            rootExec("chmod 755 " + hookDst);

            // 4. Limpar shared memory antiga
            rootExec("rm -f /data/local/tmp/.esp_shm");

            // 5. Verificar se o jogo esta aberto
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    setStatus("Root OK\nlibHook.so OK\nAguardando jogo: " + GAME_PACKAGE + " ...");
                }
            });

            String foundPid = null;
            for (int i = 0; i < 30; i++) {
                String pidResult = rootExec("pidof " + GAME_PACKAGE);
                if (pidResult != null && !pidResult.trim().isEmpty()) {
                    foundPid = pidResult.trim().split("\\s+")[0];
                    break;
                }
                Thread.sleep(1000);
            }

            if (foundPid == null) {
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        setStatus("[ERRO] Jogo nao encontrado!\nAbra " + GAME_PACKAGE + " primeiro.");
                        btnStart.setEnabled(true);
                        injecting = false;
                    }
                });
                return;
            }

            final String pid = foundPid;
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    setStatus("Root OK\nlibHook.so OK\nJogo PID: " + pid + "\nAguardando libil2cpp.so...");
                }
            });

            // 6. Aguardar libil2cpp.so carregar
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
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        setStatus("[ERRO] Timeout: libil2cpp.so nao carregou em 30s");
                        btnStart.setEnabled(true);
                        injecting = false;
                    }
                });
                return;
            }

            runOnUi(new Runnable() {
                @Override
                public void run() {
                    setStatus("Root OK\nlibHook.so OK\nJogo PID: " + pid + "\nlibil2cpp.so OK\nEsperando assemblies...");
                }
            });
            Thread.sleep(3000);

            // 7. Copiar para diretorio do jogo
            final String gameDir = "/data/data/" + GAME_PACKAGE;
            final String hookInGame = gameDir + "/libHook.so";
            rootExec("cp " + hookDst + " " + hookInGame);
            rootExec("chmod 755 " + hookInGame);

            // 8. INJETAR
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    setStatus("Root OK\nlibHook.so OK\nJogo PID: " + pid + "\nlibil2cpp.so OK\nInjetando hook...");
                }
            });

            // METODO 1: LD_PRELOAD (reinicia o jogo)
            rootExec("am force-stop " + GAME_PACKAGE);
            Thread.sleep(1000);
            final String launcherAct = getLauncherActivity(GAME_PACKAGE);
            String launchCmd = "LD_PRELOAD=" + hookInGame
                    + " am start -n " + GAME_PACKAGE + "/" + launcherAct;
            rootExec(launchCmd);

            // METODO 2: Injector ptrace (sem reiniciar)
            // Descomente e comente o METODO 1 se tiver um injector:
            // rootExec("/data/local/tmp/injector " + pid + " " + hookInGame);

            // 9. Aguardar hook ativar
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    setStatus("Root OK\nlibHook.so OK\nJogo reiniciado\nAguardando hook ativar...");
                }
            });

            boolean hookOk = false;
            for (int i = 0; i < 15; i++) {
                String check = rootExec("ls /data/local/tmp/.esp_shm 2>/dev/null");
                if (check != null && check.contains(".esp_shm")) {
                    hookOk = true;
                    break;
                }
                Thread.sleep(1000);
            }

            // 10. Iniciar overlay
            final boolean hookResult = hookOk;
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    rootExec("appops set " + getPackageName() + " SYSTEM_ALERT_WINDOW allow");
                    startService(new Intent(MainActivity.this, OverlayService.class));

                    if (hookResult) {
                        setStatus("TUDO ATIVO\nHook: VMT (MethodInfo swap)\nIPC: /data/local/tmp/.esp_shm\nESP: Ative no menu do overlay");
                    } else {
                        setStatus("OVERLAY ATIVO (hook aguardando...)\nHook: VMT (MethodInfo swap)\nIPC: /data/local/tmp/.esp_shm\nESP: Ative no menu do overlay");
                    }

                    btnStart.setEnabled(true);
                    injecting = false;
                }
            });

        } catch (final Exception e) {
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    setStatus("[ERRO] " + e.getMessage());
                    btnStart.setEnabled(true);
                    injecting = false;
                }
            });
        }
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
