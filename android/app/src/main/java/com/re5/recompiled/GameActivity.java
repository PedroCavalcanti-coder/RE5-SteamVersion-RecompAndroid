package com.re5.recompiled;

import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.net.wifi.WifiManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RelativeLayout;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Deque;
import java.util.Locale;

/**
 * O jogo: SDLActivity (libre5.so, SDL_main) deitado, com os controles de toque,
 * monitor de telemetria em tempo real (FPS, RAM, CPU, GPU) e tela de carregamento.
 */
public class GameActivity extends SDLActivity {
    private static final String TAG = "RE5GameActivity";
    private static final String FIRST_FRAME_MARK = "primeiro Present";
    private static final int LOG_LINES = 14;

    private static final int BG = 0xFF0A0B0D;
    private static final int RED = 0xFFC8102E;
    private static final int TEXT = 0xFFF1F1F1;
    private static final int TEXT_DIM = 0xFF9BA1A8;

    private final Handler main = new Handler(Looper.getMainLooper());
    private final Deque<String> recent = new ArrayDeque<>();

    private View overlay;
    private TextView statusText, logText;
    private String gameDir;
    private volatile boolean readingLog = true;

    // Telemetria (HUD)
    private LinearLayout hudLayout;
    private TextView hudFps, hudRam, hudCpu, hudGpu;
    private final Handler hudHandler = new Handler(Looper.getMainLooper());
    private int lastPresents = 0;
    private long lastHudTime = 0;
    private long lastProcCpuTicks = -1, lastCpuWallMs = 0;
    private final java.util.Map<Integer, Long> lastThreadTicks = new java.util.HashMap<>();
    private TextView hudMs;
    private int hudItems;
    // Multiplayer LAN: sem o MulticastLock o Wi-Fi do Android descarta broadcast UDP (a busca de
    // sessão); sem o WifiLock de baixa latência a economia do rádio dá picos de ping.
    private WifiManager.MulticastLock multicastLock;
    private WifiManager.WifiLock wifiLock;

    private final Runnable hudRunnable = new Runnable() {
        @Override
        public void run() {
            updateTelemetry();
            hudHandler.postDelayed(this, 500);
        }
    };

    @Override
    protected String[] getLibraries() {
        return new String[] { "re5" };
    }

    @Override
    protected String getMainFunction() {
        return "SDL_main";
    }

    @Override
    protected String[] getArguments() {
        File dir = new File(gameDir);
        File exe = new File(dir, GameStorage.CLEAN_EXE_NAME);
        if (!exe.exists()) exe = new File(dir, GameStorage.EXE_NAME);
        return new String[] { "--game-dir", dir.getAbsolutePath(), "--exe", exe.getAbsolutePath() };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        L.init(this);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 27) {
            setShowWhenLocked(true);
            setTurnScreenOn(true);
        } else {
            getWindow().addFlags(
                    WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED |
                    WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON |
                    WindowManager.LayoutParams.FLAG_DISMISS_KEYGUARD);
        }
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        SDLActivity.sForceImmersive = true;
        if (Build.VERSION.SDK_INT >= 30)
            getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        else
            getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;

        gameDir = getIntent().getStringExtra("GAME_DIR");
        if (gameDir == null) gameDir = GameStorage.gameDir(this).getAbsolutePath();
        GameStorage.provisionCleanFromAssets(this, new File(gameDir));

        super.onCreate(savedInstanceState);
        if (isFinishing()) return;   // recriada pelo sistema: shouldStartGame recusou

        try {
            nativeSetenv("SDL_ACCELEROMETER_AS_JOYSTICK", "0");
        } catch (Throwable ignored) {}

        SharedPreferences prefs = getSharedPreferences("re5_settings", MODE_PRIVATE);
        // Modo paralelo: as threads do jogo rodam em paralelo, com lock apenas no HLE.
        // Garante uso pleno dos núcleos da CPU (Cortex-A78) e alimenta a GPU com alta taxa de quadros.
        if (prefs.getBoolean("parallel_mode", true)) {
            try {
                nativeSetenv("RE5_GUEST_LOCK", "hle");
            } catch (Throwable ignored) {}
        }
        // Workers (JobThread do config.ini, aba Desempenho): o próprio jogo lê. No modo paralelo o
        // GetSystemInfo já anuncia todos os núcleos do aparelho (kernel32_env.c), então JobThread=8
        // vale direto; repassá-lo como RE5_CPUS só reduziria os núcleos vistos (o padrão é 2). Sem o
        // modo paralelo o jogo vê 2 de propósito: as threads se revezam num lock só.
        // Vídeo (launcher, aba Gráficos): renderizador, limite de FPS, apresentação e quadros em voo.
        // Renderizador: a DXVK 1.9.2b (libdxvk_d3d9.so) é o único; RE5_GFX=gl só com tradutor externo (re5_env.txt).
        /* Limitador padrao de 60 FPS quando o usuario nunca escolheu (bateria/calor; o menu rodava a ~80
         * sem necessidade). "Sem limite" nas configuracoes grava "" e continua sem limite. */
        if (!prefs.contains("gfx_fps_cap")) {
            try { nativeSetenv("RE5_FPS_CAP", "60"); } catch (Throwable ignored) {}
        } else {
            setenvPref(prefs, "gfx_fps_cap", "RE5_FPS_CAP");
        }
        setenvPref(prefs, "gfx_renderer", "RE5_GFX");               // "" = DXVK (padrão); "vk2" = Vulkan 1.1 nativo
        setenvPref(prefs, "gfx_precompile", "RE5VK2_PRECOMPILE");   // "" = segundo plano; "wait" = antes de entrar; "0" = desligada
        applyCacheDirs();
        applyVulkanDriverEnv(prefs);
        java.util.Set<String> flags = envFlags();
        boolean lean = flags.contains("RE5_LEAN=1");
        boolean noTouch = lean || flags.contains("RE5_NO_TOUCH=1");     // controles na tela
        boolean noHud = lean || flags.contains("RE5_NO_HUD=1");         // monitor de desempenho
        boolean noOverlay = lean || flags.contains("RE5_NO_OVERLAY=1"); // tela de carregamento + leitor de log
        boolean noMisc = lean || flags.contains("RE5_NO_MISC=1");       // vigia de controles + locks de Wi-Fi
        if (lean) {
            /* RE5_LEAN=1 no re5_env.txt: só o jogo. Sem controles na tela, monitor, tela de carregamento,
             * leitor de log, vigia de controles nem locks de Wi-Fi: nenhum custo de Java durante o jogo
             * (teste do 23/11: o Java do app parece custar ~3 fps no A54). Entrada só por controle/teclado. */
            Log.i(TAG, "modo RE5_LEAN: só o jogo (sem controles na tela, monitor nem tela de carregamento)");
            return;
        }
        boolean hudEnabled = prefs.getBoolean("hud_enabled", true) && !noHud;
        hudItems = prefs.getInt("hud_items", HudPrefs.DEFAULT_ITEMS);

        TouchControlsView touch = null;
        if (!noTouch) {
            touch = new TouchControlsView(this);
            touchView = touch;
            ControlsOptions.apply(this, touch);
            touch.setOnMenuToggleListener(this::toggleOverlay);
        }
        if (!noOverlay) overlay = buildOverlay();

        if (mLayout != null) {
            if (touch != null) {
                mLayout.addView(touch, new RelativeLayout.LayoutParams(
                        RelativeLayout.LayoutParams.MATCH_PARENT, RelativeLayout.LayoutParams.MATCH_PARENT));
            }

            if (hudEnabled) {
                hudLayout = buildTelemetryHUD();
                RelativeLayout.LayoutParams hudLp = new RelativeLayout.LayoutParams(
                        ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                HudPrefs.place(hudLp, prefs.getInt("hud_pos", HudPrefs.POS_TOP_LEFT), dp(16), dp(10));
                mLayout.addView(hudLayout, hudLp);
                hudHandler.post(hudRunnable);
            }

            if (overlay != null) {
                mLayout.addView(overlay, new RelativeLayout.LayoutParams(
                        RelativeLayout.LayoutParams.MATCH_PARENT, RelativeLayout.LayoutParams.MATCH_PARENT));
            }
        }

        if (!noOverlay) startLogReader();
        if (!noMisc) {
            watchInputDevices();
            acquireNetworkLocks();
        }
    }

    /** Caches de shader, pipeline e textura no cache do app (Configurações > Apps > Limpar cache
     *  apaga só eles, sem tocar no jogo nem nos saves). Antes ficavam em files/; o que sobrou lá
     *  é apagado uma vez. O re5_env.txt ainda pode apontar outro lugar (lido depois). */
    private void applyCacheDirs() {
        File cache = getCacheDir();
        File dxvk = new File(cache, "dxvk");
        //noinspection ResultOfMethodCallIgnored
        dxvk.mkdirs();
        try {
            nativeSetenv("DXVK_STATE_CACHE_PATH", dxvk.getAbsolutePath());
            nativeSetenv("MESA_SHADER_CACHE_DIR", new File(cache, "mesa").getAbsolutePath());   // drivers Turnip
            // vk2: VkPipelineCache, manifesto de shaders (pré-compilação) e cache de texturas ETC2
            File vk2 = new File(cache, "vk2");
            //noinspection ResultOfMethodCallIgnored
            new File(vk2, "tex").mkdirs();
            nativeSetenv("RE5VK2_CACHE_DIR", vk2.getAbsolutePath());
            nativeSetenv("RE5VK2_TEX_CACHE_DIR", new File(vk2, "tex").getAbsolutePath());
        } catch (Throwable t) {
            Log.w(TAG, "caches: " + t);
        }
        File files = getFilesDir();
        File[] old = {new File(files, "texcache"), new File(files, "dxvk"), new File(files, "re5vk.pcache"),
                new File(files, "re5vk.pcache.tmp")};
        new Thread(() -> {
            for (File f : old) if (f.exists()) GameStorage.deleteTree(f);
        }, "re5-cache-migrate").start();
    }

    /** Linhas "CHAVE=VALOR" do re5_env.txt da pasta do jogo. Chaves do Java (testes de custo do app):
     *  RE5_LEAN=1 (só o jogo, tudo abaixo), RE5_NO_TOUCH=1, RE5_NO_HUD=1, RE5_NO_OVERLAY=1, RE5_NO_MISC=1. */
    private java.util.Set<String> envFlags() {
        java.util.Set<String> out = new java.util.HashSet<>();
        try (BufferedReader br = new BufferedReader(new FileReader(new File(gameDir, "re5_env.txt")))) {
            String l;
            while ((l = br.readLine()) != null) out.add(l.trim());
        } catch (Exception ignored) {
        }
        return out;
    }

    /** Esconde a barra de status e a de navegação (voltam com um deslize e somem sozinhas). */
    private void hideSystemBars() {
        android.view.Window w = getWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(false);
            android.view.WindowInsetsController c = w.getInsetsController();
            if (c != null) {
                c.hide(android.view.WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
    }

    private void setenvPref(SharedPreferences prefs, String key, String env) {
        String v = prefs.getString(key, "");
        if (v == null || v.isEmpty()) return;
        try {
            nativeSetenv(env, v);
        } catch (Throwable ignored) {}
    }

    private void applyVulkanDriverEnv(SharedPreferences prefs) {
        try {
            String nativeLibDir = getApplicationInfo().nativeLibraryDir;
            if (nativeLibDir != null && !nativeLibDir.isEmpty()) {
                nativeSetenv("RE5VK_HOOK_DIR", nativeLibDir);
            }
            GpuDriverManager.DriverInfo drv = GpuDriverManager.getSelected(this, prefs.getString("gfx_driver_id", ""));
            if (drv != null) {
                File tmpDir = new File(getCacheDir(), "adrenotools_tmp");
                //noinspection ResultOfMethodCallIgnored
                tmpDir.mkdirs();
                nativeSetenv("RE5VK_DRIVER_PATH", drv.driverPathWithSlash());
                nativeSetenv("RE5VK_DRIVER_LIB", drv.libraryName);
                nativeSetenv("RE5VK_DRIVER_TMP", tmpDir.getAbsolutePath());
            }
            if (prefs.getBoolean("gfx_adreno_turbo", false)) {
                nativeSetenv("RE5VK_ADRENO_TURBO", "1");
            }
        } catch (Throwable t) {
            Log.w(TAG, "driver env: " + t);
        }
    }

    private void acquireNetworkLocks() {
        try {
            WifiManager wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
            if (wm == null) return;
            multicastLock = wm.createMulticastLock("re5-lan");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
            int mode = Build.VERSION.SDK_INT >= 29 ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                                                   : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
            wifiLock = wm.createWifiLock(mode, "re5-lan");
            wifiLock.setReferenceCounted(false);
            wifiLock.acquire();
        } catch (Throwable t) {
            Log.w(TAG, "locks de Wi-Fi: " + t);
        }
    }

    private void releaseNetworkLocks() {
        try {
            if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
            if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
        } catch (Throwable ignored) {}
    }

    // ------------------------------------------------------------------ ciclo de vida
    // O jogo não sobrevive à morte do processo ":game": recriar a activity seria começar
    // do zero (e, com um roteiro no re5_env.txt, rodar o benchmark de novo, como se fossem
    // "duas instâncias"). Então a activity recriada pelo sistema volta ao launcher, e a
    // saída do jogo (ExitProcess, botão Sair) fecha a activity antes de matar o processo.

    @Override
    protected boolean shouldStartGame(Bundle savedInstanceState) {
        if (GameSession.claimLaunch(this, getIntent().getStringExtra("LAUNCH_ID"))) return true;
        Log.i(TAG, "activity recriada pelo sistema depois que o processo do jogo morreu: volta ao launcher");
        android.widget.Toast.makeText(this, L.t("O jogo foi fechado pelo Android em segundo plano. Toque em JOGAR ")
                + L.t("para abrir de novo."), android.widget.Toast.LENGTH_LONG).show();
        Intent i = new Intent(this, LauncherActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(i);
        return false;
    }

    /** Chamado pelo runtime (ExitProcess, kernel32.c) na thread do jogo, logo antes do exit(). */
    @SuppressWarnings("unused")
    public void onNativeExit() {
        java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(1);
        runOnUiThread(() -> {
            finish();
            done.countDown();
        });
        try {
            done.await(2, java.util.concurrent.TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
        }
    }

    @Override
    protected void onDestroy() {
        readingLog = false;
        hudHandler.removeCallbacks(hudRunnable);
        releaseNetworkLocks();
        InputManager im = (InputManager) getSystemService(INPUT_SERVICE);
        if (im != null) im.unregisterInputDeviceListener(deviceListener);
        super.onDestroy();
    }

    // ------------------------------------------------------------------ dispositivos de entrada
    // O toque é o padrão no celular. Ligar um controle ou um teclado físico avisa na tela
    // (como o jogo avisa o controle no PC); o runtime troca o dispositivo ativo quando ele
    // é usado (re5/touch.h) e os botões da tela somem até o próximo toque.

    private TouchControlsView touchView;
    private final android.util.SparseArray<String> knownDevices = new android.util.SparseArray<>();

    /** "controle" ou "teclado" para um aparelho físico; null para o resto (sensores, toque, botões do corpo). */
    private static String deviceKind(InputDevice d) {
        if (d == null || d.isVirtual()) return null;
        int s = d.getSources();
        if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) return "controle";
        if ((s & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD
                && d.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC) return "teclado";
        return null;
    }

    private final InputManager.InputDeviceListener deviceListener = new InputManager.InputDeviceListener() {
        @Override
        public void onInputDeviceAdded(int id) {
            InputDevice d = InputDevice.getDevice(id);
            String kind = deviceKind(d);
            if (kind == null) return;
            knownDevices.put(id, kind);
            notice(("controle".equals(kind) ? "🎮 Controle conectado: " : "⌨ Teclado conectado: ") + d.getName());
        }

        @Override
        public void onInputDeviceRemoved(int id) {
            String kind = knownDevices.get(id);
            if (kind == null) return;
            knownDevices.remove(id);
            notice(("controle".equals(kind) ? "🎮 Controle" : "⌨ Teclado") + " desconectado · voltando ao toque");
        }

        @Override
        public void onInputDeviceChanged(int id) {}
    };

    private void watchInputDevices() {
        InputManager im = (InputManager) getSystemService(INPUT_SERVICE);
        if (im == null) return;
        im.registerInputDeviceListener(deviceListener, main);
        StringBuilder already = new StringBuilder();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            String kind = deviceKind(d);
            if (kind == null) continue;
            knownDevices.put(id, kind);
            if (already.length() > 0) already.append(", ");
            already.append(kind).append(' ').append(d.getName());
        }
        // Mostrado quando a tela de carregamento sai (embaixo dela ninguém veria).
        if (already.length() > 0) startupNotice = L.t("Conectado: ") + already;
    }

    private void notice(String text) {
        Log.i(TAG, text);
        if (touchView != null) touchView.showNotice(text);
    }

    /** HUD de desempenho: FPS, ms por quadro, CPU do processo, GPU (uso e MHz) e RAM, conforme
     *  os itens e a posição escolhidos no launcher (aba HUD). */
    private LinearLayout buildTelemetryHUD() {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(10), dp(4), dp(10), dp(4));

        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0x990B0F19);
        bg.setCornerRadius(dp(12));
        bg.setStroke(dp(1), 0x44FFFFFF);
        card.setBackground(bg);

        hudFps = text(L.t("… FPS"), 11, 0xFF4ADE80, true);
        hudMs = text(L.t("… ms"), 11, 0xFFE2E8F0, true);
        hudCpu = text(L.t("… CPU"), 11, 0xFFFBBF24, true);
        hudGpu = text(L.t("… GPU"), 11, 0xFFC084FC, true);
        hudRam = text(L.t("… RAM"), 11, 0xFF38BDF8, true);
        TextView[] all = {hudFps, hudMs, hudCpu, hudGpu, hudRam};
        int[] bits = {HudPrefs.ITEM_FPS, HudPrefs.ITEM_MS, HudPrefs.ITEM_CPU, HudPrefs.ITEM_GPU, HudPrefs.ITEM_RAM};
        boolean first = true;
        for (int i = 0; i < all.length; i++) {
            if ((hudItems & bits[i]) == 0) continue;
            if (!first) card.addView(hudDivider());
            card.addView(all[i]);
            first = false;
        }
        return card;
    }

    private View hudDivider() {
        View v = new View(this);
        v.setBackgroundColor(0x33FFFFFF);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(1), dp(12));
        lp.setMargins(dp(8), 0, dp(8), 0);
        v.setLayoutParams(lp);
        return v;
    }

    private void updateTelemetry() {
        // FPS e ms pelo contador de Presents D3D9 da libre5.so (média da janela de 500 ms)
        int curPresents = 0;
        try { curPresents = SDLActivity.nativeGetPresents(); } catch (Throwable ignored) {}
        long now = SystemClock.uptimeMillis();
        if (lastHudTime > 0 && now > lastHudTime) {
            float dt = (now - lastHudTime) / 1000.0f;
            int n = curPresents - lastPresents;
            float fps = dt > 0 ? (n / dt) : 0f;
            hudFps.setText(String.format(Locale.US, L.t("%.1f FPS"), fps));
            hudFps.setTextColor(fps >= 45f ? 0xFF4ADE80 : fps >= 25f ? 0xFFFBBF24 : 0xFFF87171);
            hudMs.setText(n > 0 ? String.format(Locale.US, L.t("%.1f ms"), 1000f * dt / n) : L.t("— ms"));
        }
        lastPresents = curPresents;
        lastHudTime = now;

        long rssKb = readVmRssKb();
        hudRam.setText(rssKb >= 1024 * 1024 ? String.format(Locale.US, L.t("%.2f GB"), rssKb / (1024.0f * 1024.0f))
                                            : String.format(Locale.US, L.t("%d MB"), rssKb / 1024));

        // CPU: tempo de CPU do processo / tempo real, em % de todos os núcleos (100% = todos cheios),
        // e a thread mais ocupada em % de um núcleo (perto de 100 = essa thread é o limite do fps).
        long ticks = HudPrefs.processCpuTicks();
        long wall = SystemClock.elapsedRealtime();
        HudPrefs.BusiestThread top = HudPrefs.busiestThread(lastThreadTicks, wall - lastCpuWallMs);
        if (ticks >= 0 && lastProcCpuTicks >= 0 && wall > lastCpuWallMs) {
            float cpuMs = (ticks - lastProcCpuTicks) * 10f;   // 1 tick = 10 ms (USER_HZ 100)
            int cores = Runtime.getRuntime().availableProcessors();
            float pct = 100f * cpuMs / ((wall - lastCpuWallMs) * (float) cores);
            String s = String.format(Locale.US, L.t("%.0f%% CPU"), Math.max(0f, Math.min(100f, pct)));
            if (top.pct >= 0f)
                s += String.format(Locale.US, L.t(" · máx %.0f%% %s"), top.pct, top.name);
            hudCpu.setText(s);
        } else if (ticks < 0) {
            hudCpu.setText(L.t("— CPU"));
        }
        lastProcCpuTicks = ticks;
        lastCpuWallMs = wall;

        int busy = HudPrefs.gpuBusy(), mhz = HudPrefs.gpuMhz();
        if (busy >= 0) {
            hudGpu.setText(mhz > 0 ? String.format(Locale.US, L.t("GPU %d%% · %d MHz"), busy, mhz)
                                   : String.format(Locale.US, L.t("GPU %d%%"), busy));
        } else {
            hudGpu.setText(L.t("GPU —"));
        }
    }

    private long readVmRssKb() {
        try (BufferedReader br = new BufferedReader(new FileReader("/proc/self/status"))) {
            String line;
            while ((line = br.readLine()) != null) {
                if (line.startsWith("VmRSS:")) {
                    String[] parts = line.split("\\s+");
                    if (parts.length >= 2) return Long.parseLong(parts[1]);
                }
            }
        } catch (Exception ignored) {}
        return Runtime.getRuntime().totalMemory() / 1024;
    }

    /** Lê o logcat deste processo a partir de agora e mostra as últimas linhas do runtime. */
    private void startLogReader() {
        String since = new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US).format(new Date());
        Thread t = new Thread(() -> {
            Process p = null;
            try {
                p = new ProcessBuilder("logcat", "-v", "brief", "-T", since, "-s", "RE5Native:V", "SDL:V")
                        .redirectErrorStream(true).start();
                try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                    String line;
                    while (readingLog && (line = r.readLine()) != null) {
                        if (line.startsWith("---------")) continue;
                        int sep = line.indexOf(": ");   // "-v brief": "I/RE5Native( 123): msg"
                        String msg = sep >= 0 ? line.substring(sep + 2) : line;
                        boolean firstFrame = msg.contains(FIRST_FRAME_MARK);
                        main.post(() -> onLogLine(msg, firstFrame));
                        // Depois do primeiro quadro o painel de log some: seguir lendo deixava um
                        // processo logcat acordado e um setText na thread de UI a cada linha do
                        // runtime (o RE5_FPS_LOG escreve várias por segundo) o jogo inteiro.
                        if (firstFrame) break;
                    }
                }
            } catch (Exception e) {
                Log.w(TAG, "leitor de log: " + e.getMessage());
            } finally {
                if (p != null) p.destroy();
            }
        }, "re5-log");
        t.setDaemon(true);
        t.start();
    }

    private void onLogLine(String msg, boolean firstFrame) {
        recent.addLast(msg);
        if (recent.size() > LOG_LINES) recent.removeFirst();
        if (logText != null) logText.setText(String.join("\n", recent));
        if (statusText != null && msg.startsWith("[")) {
            int close = msg.indexOf(']');
            String rest = close > 0 ? msg.substring(close + 1).trim() : msg;
            statusText.setText(rest);
            if (msg.startsWith("[ERROR")) statusText.setTextColor(0xFFFF8A8A);
        }
        if (firstFrame) hideOverlay();
    }

    private void quitGame() {
        finish();   // sem isso o sistema recria a activity num processo novo
        main.postDelayed(() -> android.os.Process.killProcess(android.os.Process.myPid()), 300);
    }

    private String startupNotice;

    private void hideOverlay() {
        if (startupNotice != null) {
            notice(startupNotice);
            startupNotice = null;
        }
        if (overlay == null || overlay.getVisibility() != View.VISIBLE) return;
        overlay.animate().alpha(0f).setDuration(350).withEndAction(() -> overlay.setVisibility(View.GONE));
    }

    private void showOverlay() {
        if (overlay == null) return;
        overlay.setAlpha(0f);
        overlay.setVisibility(View.VISIBLE);
        overlay.animate().alpha(1f).setDuration(250);
    }

    /** Editor do layout dos controles por cima do jogo (o jogo não recebe toque enquanto isso). */
    private void editControls() {
        hideOverlay();
        if (touchView == null) return;
        touchView.setEditorListener(new TouchControlsView.EditorListener() {
            @Override
            public void onEditorOptions() {
                ControlsOptions.show(GameActivity.this, () -> ControlsOptions.apply(GameActivity.this, touchView));
            }

            @Override
            public void onEditorClose(boolean saved) {
                touchView.setEditMode(false);
                if (saved) notice(L.t("Layout dos controles salvo"));
            }
        });
        touchView.setEditMode(true);
    }

    private void toggleOverlay() {
        if (overlay == null) return;
        if (overlay.getVisibility() == View.VISIBLE) hideOverlay();
        else showOverlay();
    }

    private View buildOverlay() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(BG);
        root.setClickable(true);

        ImageView bg = new ImageView(this);
        bg.setImageResource(R.drawable.re5_hero);
        bg.setScaleType(ImageView.ScaleType.CENTER_CROP);
        bg.setAlpha(0.35f);
        root.addView(bg, match());
        View shade = new View(this);
        shade.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[]{0xF00A0B0D, 0xB30A0B0D, 0xF00A0B0D}));
        root.addView(shade, match());

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(dp(32), dp(24), dp(32), dp(24));

        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        left.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = text("RESIDENT EVIL 5", 30, TEXT, true);
        title.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        left.addView(title);
        ProgressBar spin = new ProgressBar(this);
        spin.setIndeterminateTintList(android.content.res.ColorStateList.valueOf(RED));
        LinearLayout.LayoutParams sLp = new LinearLayout.LayoutParams(dp(36), dp(36));
        sLp.setMargins(0, dp(18), 0, dp(12));
        left.addView(spin, sLp);
        statusText = text(L.t("Carregando…"), 13, TEXT_DIM, false);
        statusText.setMaxLines(3);
        left.addView(statusText);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams bLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(42));
        bLp.setMargins(0, dp(20), dp(10), 0);
        buttons.addView(button(L.t("Ocultar"), 0xFF22252B, v -> hideOverlay()), bLp);
        buttons.addView(button(L.t("Controles"), 0xFF22252B, v -> editControls()), bLp);
        buttons.addView(button(L.t("Sair"), RED, v -> quitGame()), bLp);
        left.addView(buttons);
        row.addView(left, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));

        logText = text("", 10, 0xFF8FA3B8, false);
        logText.setTypeface(Typeface.MONOSPACE);
        logText.setGravity(Gravity.BOTTOM);
        logText.setPadding(dp(14), dp(12), dp(14), dp(12));
        GradientDrawable logBg = new GradientDrawable();
        logBg.setColor(0xCC0F1114);
        logBg.setCornerRadius(dp(14));
        logBg.setStroke(dp(1), 0xFF25282E);
        logText.setBackground(logBg);
        LinearLayout.LayoutParams lLp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.2f);
        lLp.leftMargin = dp(24);
        row.addView(logText, lLp);

        root.addView(row, match());
        return root;
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private TextView button(String s, int bg, View.OnClickListener l) {
        TextView b = text(s, 14, Color.WHITE, true);
        b.setGravity(Gravity.CENTER);
        b.setPadding(dp(22), 0, dp(22), 0);
        GradientDrawable d = new GradientDrawable();
        d.setColor(bg);
        d.setCornerRadius(dp(12));
        b.setBackground(d);
        b.setOnClickListener(l);
        return b;
    }

    private static FrameLayout.LayoutParams match() {
        return new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }
}
