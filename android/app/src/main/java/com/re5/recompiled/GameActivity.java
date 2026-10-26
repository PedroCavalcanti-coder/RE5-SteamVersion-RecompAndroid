package com.re5.recompiled;

import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
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
    private long lastTotalCpu = 0, lastProcCpu = 0;

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

        gameDir = getIntent().getStringExtra("GAME_DIR");
        if (gameDir == null) gameDir = GameStorage.gameDir(this).getAbsolutePath();
        provisionCleanExecutable();

        super.onCreate(savedInstanceState);

        SharedPreferences prefs = getSharedPreferences("re5_settings", MODE_PRIVATE);
        boolean touchEnabled = prefs.getBoolean("touch_enabled", true);
        float touchOpacity = prefs.getFloat("touch_opacity", 0.85f);
        float touchScale = prefs.getFloat("touch_scale", 1.0f);
        boolean hudEnabled = prefs.getBoolean("hud_enabled", true);

        TouchControlsView touch = new TouchControlsView(this);
        touch.setControlsEnabled(touchEnabled);
        touch.setControlsOpacity(touchOpacity);
        touch.setControlsScale(touchScale);
        touch.setOnMenuToggleListener(this::toggleOverlay);

        overlay = buildOverlay();

        if (mLayout != null) {
            mLayout.addView(touch, new RelativeLayout.LayoutParams(
                    RelativeLayout.LayoutParams.MATCH_PARENT, RelativeLayout.LayoutParams.MATCH_PARENT));

            if (hudEnabled) {
                hudLayout = buildTelemetryHUD();
                RelativeLayout.LayoutParams hudLp = new RelativeLayout.LayoutParams(
                        ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                hudLp.setMargins(dp(16), dp(10), 0, 0);
                mLayout.addView(hudLayout, hudLp);
                hudHandler.post(hudRunnable);
            }

            mLayout.addView(overlay, new RelativeLayout.LayoutParams(
                    RelativeLayout.LayoutParams.MATCH_PARENT, RelativeLayout.LayoutParams.MATCH_PARENT));
        }

        startLogReader();
    }

    @Override
    protected void onDestroy() {
        readingLog = false;
        hudHandler.removeCallbacks(hudRunnable);
        super.onDestroy();
    }

    /** Constrói a visualização do HUD de telemetria */
    private LinearLayout buildTelemetryHUD() {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(10), dp(4), dp(10), dp(4));

        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0x880B0F19);
        bg.setCornerRadius(dp(12));
        bg.setStroke(dp(1), 0x44FFFFFF);
        card.setBackground(bg);

        hudFps = text("... FPS", 11, 0xFF4ADE80, true);
        hudRam = text("... RAM", 11, 0xFF38BDF8, true);
        hudCpu = text("... CPU", 11, 0xFFFBBF24, true);
        hudGpu = text("Vulkan 1.3", 11, 0xFFE2E8F0, true);

        card.addView(hudFps);
        card.addView(hudDivider());
        card.addView(hudRam);
        card.addView(hudDivider());
        card.addView(hudCpu);
        card.addView(hudDivider());
        card.addView(hudGpu);

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
        // 1. FPS via contador de Presents D3D9 da libre5.so
        int curPresents = 0;
        try { curPresents = SDLActivity.nativeGetPresents(); } catch (Throwable ignored) {}
        long now = SystemClock.uptimeMillis();
        if (lastHudTime > 0 && now > lastHudTime) {
            float dt = (now - lastHudTime) / 1000.0f;
            int dp = curPresents - lastPresents;
            float fps = dt > 0 ? (dp / dt) : 0f;
            if (hudFps != null) {
                hudFps.setText(String.format(Locale.US, "%.1f FPS", fps));
                hudFps.setTextColor(fps >= 45f ? 0xFF4ADE80 : fps >= 25f ? 0xFFFBBF24 : 0xFFF87171);
            }
        }
        lastPresents = curPresents;
        lastHudTime = now;

        // 2. RAM real em uso (VmRSS no /proc/self/status)
        long rssKb = readVmRssKb();
        if (hudRam != null) {
            if (rssKb >= 1024 * 1024) {
                hudRam.setText(String.format(Locale.US, "%.2f GB", rssKb / (1024.0f * 1024.0f)));
            } else {
                hudRam.setText(String.format(Locale.US, "%d MB", rssKb / 1024));
            }
        }

        // 3. CPU % do processo
        float cpu = calculateCpuUsage();
        if (hudCpu != null) {
            hudCpu.setText(String.format(Locale.US, "%.0f%% CPU", cpu));
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

    private float calculateCpuUsage() {
        try (BufferedReader rStat = new BufferedReader(new FileReader("/proc/stat"));
             BufferedReader rProc = new BufferedReader(new FileReader("/proc/self/stat"))) {
            String lStat = rStat.readLine();
            String lProc = rProc.readLine();
            if (lStat != null && lProc != null) {
                String[] sTok = lStat.trim().split("\\s+");
                long total = 0;
                for (int i = 1; i < sTok.length && i < 8; i++) total += Long.parseLong(sTok[i]);

                String[] pTok = lProc.trim().split("\\s+");
                long proc = Long.parseLong(pTok[13]) + Long.parseLong(pTok[14]);

                if (lastTotalCpu > 0 && total > lastTotalCpu) {
                    float usage = (float) (proc - lastProcCpu) / (float) (total - lastTotalCpu) * 100.0f
                            * Runtime.getRuntime().availableProcessors();
                    lastTotalCpu = total;
                    lastProcCpu = proc;
                    return Math.max(0f, Math.min(100f, usage));
                }
                lastTotalCpu = total;
                lastProcCpu = proc;
            }
        } catch (Exception ignored) {}
        return 0f;
    }

    /** O re5dx9.clean.exe (despejo já desempacotado) vai no APK; copia para a pasta do jogo. */
    private void provisionCleanExecutable() {
        File clean = new File(gameDir, GameStorage.CLEAN_EXE_NAME);
        if (clean.exists()) return;
        File tmp = new File(gameDir, GameStorage.CLEAN_EXE_NAME + ".part");
        try (InputStream in = getAssets().open(GameStorage.CLEAN_EXE_NAME);
             FileOutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 16];
            int r;
            while ((r = in.read(buf)) != -1) out.write(buf, 0, r);
        } catch (Exception e) {
            Log.w(TAG, "não foi possível copiar " + GameStorage.CLEAN_EXE_NAME + ": " + e.getMessage());
            tmp.delete();
            return;
        }
        tmp.renameTo(clean);
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
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    private void hideOverlay() {
        if (overlay.getVisibility() != View.VISIBLE) return;
        overlay.animate().alpha(0f).setDuration(350).withEndAction(() -> overlay.setVisibility(View.GONE));
    }

    private void showOverlay() {
        overlay.setAlpha(0f);
        overlay.setVisibility(View.VISIBLE);
        overlay.animate().alpha(1f).setDuration(250);
    }

    private void toggleOverlay() {
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
        statusText = text("Carregando…", 13, TEXT_DIM, false);
        statusText.setMaxLines(3);
        left.addView(statusText);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams bLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(42));
        bLp.setMargins(0, dp(20), dp(10), 0);
        buttons.addView(button("Ocultar", 0xFF22252B, v -> hideOverlay()), bLp);
        buttons.addView(button("Sair", RED, v -> quitGame()), bLp);
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
