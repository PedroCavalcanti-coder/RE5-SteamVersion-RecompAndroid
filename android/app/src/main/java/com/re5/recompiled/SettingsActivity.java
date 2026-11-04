package com.re5.recompiled;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.util.Locale;

/**
 * Configurações em tela cheia, no estilo dos emuladores: abas no alto (Gráficos, Controles, HUD,
 * Desempenho, Rede, Avançado) e uma lista de opções com título, descrição e controle.
 *
 * Onde cada opção vai:
 * - prefs "re5_settings": lidas pelo GameActivity (variáveis do runtime, toque, HUD);
 * - config.ini do jogo (GameConfigIni): resolução, qualidade, sombras, JobThread;
 * - re5_env.txt na pasta do jogo: variáveis RE5_* livres (Avançado).
 */
public final class SettingsActivity extends Activity {

    public static final String EXTRA_TAB = "tab";
    public static final String RESULT_OPEN_LAN = "open_lan";

    static final int TAB_GRAPHICS = 0, TAB_CONTROLS = 1, TAB_HUD = 2, TAB_PERF = 3, TAB_NET = 4, TAB_ADV = 5;
    private static final String[] TABS = {L.t("Gráficos"), L.t("Controles"), "HUD", L.t("Desempenho"), L.t("Rede"), L.t("Avançado")};

    private static final int BG = 0xFF0A0B0D, SURFACE = 0xFF15171B, SURFACE_2 = 0xFF22252B, STROKE = 0xFF25282E;
    private static final int TEXT = 0xFFF1F1F1, TEXT_DIM = 0xFF9BA1A8, RED = 0xFFC8102E;

    private SharedPreferences prefs;
    private GameConfigIni ini;
    private LinearLayout tabBar, content;
    private int tab;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        L.init(this);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        prefs = getSharedPreferences(ControlsOptions.PREFS, MODE_PRIVATE);
        ini = new GameConfigIni(GameStorage.gameDir(this));
        tab = getIntent().getIntExtra(EXTRA_TAB, TAB_GRAPHICS);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BG);

        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(8), dp(10), dp(16), dp(6));
        TextView back = text("←", 24, TEXT, true);
        back.setPadding(dp(12), dp(4), dp(16), dp(4));
        back.setOnClickListener(v -> finish());
        header.addView(back);
        header.addView(text(L.t("Configurações"), 22, TEXT, true));
        root.addView(header);

        HorizontalScrollView hs = new HorizontalScrollView(this);
        hs.setHorizontalScrollBarEnabled(false);
        tabBar = new LinearLayout(this);
        tabBar.setOrientation(LinearLayout.HORIZONTAL);
        tabBar.setPadding(dp(12), dp(4), dp(12), dp(10));
        hs.addView(tabBar);
        root.addView(hs);

        ScrollView scroll = new ScrollView(this);
        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(16), dp(4), dp(16), dp(32));
        scroll.addView(content);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1f));

        root.setOnApplyWindowInsetsListener((v, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets b = insets.getInsets(WindowInsets.Type.systemBars());
                v.setPadding(0, b.top, 0, b.bottom);
            }
            return insets;
        });
        setContentView(root);
        showTab(tab);
    }

    private void showTab(int t) {
        tab = t;
        tabBar.removeAllViews();
        for (int i = 0; i < TABS.length; i++) {
            final int idx = i;
            TextView chip = text(TABS[i], 14, i == t ? Color.WHITE : TEXT_DIM, true);
            chip.setPadding(dp(16), dp(8), dp(16), dp(8));
            GradientDrawable d = new GradientDrawable();
            d.setCornerRadius(dp(20));
            d.setColor(i == t ? RED : SURFACE);
            d.setStroke(dp(1), i == t ? RED : STROKE);
            chip.setBackground(d);
            chip.setOnClickListener(v -> showTab(idx));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2);
            lp.rightMargin = dp(8);
            tabBar.addView(chip, lp);
        }
        content.removeAllViews();
        switch (t) {
            case TAB_GRAPHICS: buildGraphics(); break;
            case TAB_CONTROLS: buildControls(); break;
            case TAB_HUD: buildHud(); break;
            case TAB_PERF: buildPerf(); break;
            case TAB_NET: buildNet(); break;
            default: buildAdvanced(); break;
        }
    }

    // ------------------------------------------------------------------ abas

    private void buildGraphics() {
        section(L.t("Renderização"));
        choicePref(L.t("Renderizador"), L.t("Quem traduz o Direct3D 9 do jogo para o Vulkan do aparelho."),
                new String[]{L.t("DXVK (padrão, mais testado)"), L.t("Vulkan próprio (experimental)")},
                new String[]{"", "re5vk"}, "gfx_renderer");
        String res = ini.get("DISPLAY", "Resolution", "1280x720");
        String[] resVals = {"640x360", "854x480", "960x540", "1280x720", "1600x900", "1920x1080"};
        choiceIni(L.t("Resolução interna"), L.t("Resolução em que o jogo desenha; a tela estica para caber. Menor = mais FPS."),
                resVals, resVals, "DISPLAY", "Resolution", res);
        choicePref(L.t("Limite de FPS"), L.t("Segura o jogo num ritmo fixo: menos calor e bateria, quadro mais estável."),
                new String[]{L.t("Sem limite"), L.t("30 FPS"), L.t("45 FPS"), L.t("60 FPS")}, new String[]{"", "30", "45", "60"}, "gfx_fps_cap");
        choicePref(L.t("Sincronia vertical (Vulkan próprio)"), L.t("Com vsync a tela não rasga, mas o FPS segue a taxa da tela."),
                new String[]{L.t("Como o jogo pedir"), L.t("Ligada (FIFO)"), L.t("Desligada (mailbox)"), L.t("Desligada (imediato)")},
                new String[]{"", "fifo", "mailbox", "immediate"}, "gfx_present");
        choicePref(L.t("Quadros em voo (Vulkan próprio)"), L.t("Mais quadros = mais FPS e mais atraso de entrada."),
                new String[]{L.t("Padrão (3)"), L.t("1 (menor atraso)"), "2", "3"}, new String[]{"", "1", "2", "3"}, "gfx_latency");
        disabledRow(L.t("FSR 1.0 (AMD FidelityFX)"), L.t("Ampliação com nitidez da resolução interna para a tela. Chega na próxima ")
                + L.t("versão, no Vulkan próprio."));
        disabledRow(L.t("Driver Vulkan: Turnip"), isAdreno()
                ? L.t("Driver Mesa para GPU Adreno (Snapdragon). Chega na próxima versão.")
                : L.t("Turnip só funciona em GPU Adreno (Snapdragon). Este aparelho usa ") + gpuFamily() + L.t(": fica o driver do sistema."));

        section(L.t("Qualidade do jogo (config.ini)"));
        if (!ini.exists()) note(L.t("O config.ini aparece depois que o jogo abrir uma vez; até lá vale o padrão do jogo."));
        String[] q3 = {L.t("Baixa"), L.t("Média"), L.t("Alta")};
        String[] q3v = {"LOW", "MIDDLE", "HIGH"};
        choiceIni(L.t("Sombras"), L.t("Mais caras na GPU."), q3, q3v, "GRAPHICS", "ShadowQuality", ini.get("GRAPHICS", "ShadowQuality", "LOW"));
        choiceIni(L.t("Texturas"), L.t("Mais memória."), q3, q3v, "GRAPHICS", "TextureDetail", ini.get("GRAPHICS", "TextureDetail", "LOW"));
        choiceIni(L.t("Efeitos"), L.t("Fumaça, fogo e partículas."), q3, q3v, "GRAPHICS", "EffectQuality", ini.get("GRAPHICS", "EffectQuality", "LOW"));
        choiceIni(L.t("Iluminação"), "", q3, q3v, "GRAPHICS", "Lighting", ini.get("GRAPHICS", "Lighting", "LOW"));
        choiceIni(L.t("Desfoque de movimento"), "", new String[]{L.t("Desligado"), L.t("Baixo"), L.t("Alto")}, new String[]{"NONE", "LOW", "HIGH"},
                "GRAPHICS", "MotionBlurQuality", ini.get("GRAPHICS", "MotionBlurQuality", "NONE"));
    }

    private void buildControls() {
        section(L.t("Controles na tela"));
        action(L.t("Editar layout"), L.t("Arraste os botões e pince para mudar o tamanho de cada um."),
                () -> startActivity(new Intent(this, ControlsEditorActivity.class)));
        switchPref(L.t("Mostrar os controles na tela"), "", "touch_enabled", true);
        sliderPref(L.t("Sensibilidade da câmera"), L.t("Arrastar no lado direito gira a câmera (e a mira)."), "touch_look_sens", 50, 400, 100f, 1f, "%");
        switchPref(L.t("Inverter o eixo vertical da câmera"), "", "touch_invert_y", false);
        sliderPref(L.t("Opacidade dos botões"), "", "touch_opacity", 20, 100, 100f, 0.85f, "%");
        sliderPref(L.t("Tamanho de todos os botões"), L.t("Cada botão também tem tamanho próprio no editor."), "touch_scale", 60, 160, 100f, 1f, "%");
        switchPref(L.t("Vibrar ao tocar nos botões"), "", "touch_vibration", true);
        switchPref(L.t("Menus por botões"), L.t("Desligado: toque direto no item do menu. Ligado: setas ou analógico + OK/Voltar."),
                "touch_menu_buttons", false);
        switchPref(L.t("Esconder com controle físico"), L.t("Os botões voltam no primeiro toque na tela."), "touch_hide_with_pad", true);
        section(L.t("Como os botões agem"));
        note(L.t("Correr, Mirar e Faca alternam: um toque liga, outro desliga. A arma atira mirando ou dá a facada com a faca ")
                + L.t("pronta; sem nenhum dos dois, segurar a arma mira e atira. Recarregar funciona mirando ou não. O ")
                + L.t("inventário abre com o toque ligado para escolher os itens; toque no ícone de novo para fechar."));
    }

    private void buildHud() {
        section(L.t("Monitor de desempenho"));
        switchPref(L.t("Mostrar o monitor"), L.t("FPS e uso do aparelho por cima do jogo."), "hud_enabled", true);
        int pos = prefs.getInt("hud_pos", HudPrefs.POS_TOP_LEFT);
        row(L.t("Posição"), L.t(HudPrefs.POS_NAMES[pos]), v -> new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(L.t("Posição"))
                .setSingleChoiceItems(trAll(HudPrefs.POS_NAMES), prefs.getInt("hud_pos", 0), (d, w) -> {
                    prefs.edit().putInt("hud_pos", w).apply();
                    d.dismiss();
                    showTab(TAB_HUD);
                }).show());
        section(L.t("O que mostrar"));
        int items = prefs.getInt("hud_items", HudPrefs.DEFAULT_ITEMS);
        hudItem(L.t("Quadros por segundo (FPS)"), HudPrefs.ITEM_FPS, items);
        hudItem(L.t("Tempo de quadro (ms)"), HudPrefs.ITEM_MS, items);
        hudItem(L.t("CPU do jogo (%)"), HudPrefs.ITEM_CPU, items);
        hudItem(L.t("GPU (% de uso e MHz)"), HudPrefs.ITEM_GPU, items);
        hudItem(L.t("Memória (RAM)"), HudPrefs.ITEM_RAM, items);
    }

    private void buildPerf() {
        section(L.t("Processador"));
        switchPref(L.t("Modo paralelo (experimental)"), L.t("As threads do jogo rodam ao mesmo tempo, como no PC: som sem atraso e ")
                + L.t("mais FPS nas cenas pesadas. Se travar ou der erro, desligue."), "parallel_mode", false);
        String jt = ini.get("CPU", "JobThread", "2");
        choiceIni(L.t("Threads de trabalho do jogo"), L.t("Workers do motor do jogo (física, animação, efeitos)."),
                new String[]{"1", "2", "3", "4", "6"}, new String[]{"1", "2", "3", "4", "6"}, "CPU", "JobThread", jt);
    }

    private void buildNet() {
        section(L.t("Multiplayer cooperativo (LAN)"));
        note(L.t("Os dois aparelhos na mesma rede Wi-Fi (ou um no hotspot do outro). No jogo: Multiplayer > criar ou entrar ")
                + L.t("na sessão. Durante o jogo, o app mantém o Wi-Fi em baixa latência e recebendo a busca de sessões."));
        action(L.t("Nome, bandeira e IPs"), L.t("Sua identidade na sessão e IPs para procurar direto."), () -> {
            setResult(RESULT_OK, new Intent().putExtra(RESULT_OPEN_LAN, true));
            finish();
        });
    }

    private void buildAdvanced() {
        section(L.t("Idioma"));
        String[] langs = new String[L.NAMES.length];
        for (int i = 0; i < langs.length; i++) langs[i] = i < 2 ? L.t(L.NAMES[i]) : L.NAMES[i];
        row(L.t("Idioma do app"), langs[L.savedIndex(this)], v -> new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(L.t("Idioma"))
                .setSingleChoiceItems(langs, L.savedIndex(this), (d, w) -> {
                    L.save(this, w);
                    d.dismiss();
                    recreate();
                }).show());
        section(L.t("Variáveis do runtime"));
        note(L.t("Uma por linha, ex.: RE5_FPS_LOG=1 ou RE5_CPUS=4. Vai para re5_env.txt na pasta do jogo. Deixe vazio no uso ")
                + L.t("normal; as opções das outras abas vencem estas."));
        File envFile = new File(GameStorage.gameDir(this), "re5_env.txt");
        EditText edit = new EditText(this);
        edit.setText(readText(envFile));
        edit.setHint("RE5_FPS_LOG=1");
        edit.setTextColor(TEXT);
        edit.setHintTextColor(TEXT_DIM);
        edit.setMinLines(5);
        edit.setGravity(Gravity.TOP);
        edit.setBackground(box(SURFACE_2));
        edit.setPadding(dp(12), dp(10), dp(12), dp(10));
        content.addView(edit, rowLp());
        action(L.t("Salvar variáveis"), "", () -> {
            writeEnv(envFile, edit.getText().toString());
            Toast.makeText(this, L.t("re5_env.txt salvo"), Toast.LENGTH_SHORT).show();
        });
    }

    // ------------------------------------------------------------------ linhas

    private void section(String title) {
        TextView t = text(title.toUpperCase(Locale.ROOT), 12, RED, true);
        t.setLetterSpacing(0.1f);
        t.setPadding(dp(4), dp(18), 0, dp(6));
        content.addView(t);
    }

    private void note(String s) {
        TextView t = text(s, 13, TEXT_DIM, false);
        t.setPadding(dp(4), dp(4), dp(4), dp(8));
        content.addView(t);
    }

    private LinearLayout card(String title, String desc) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.HORIZONTAL);
        c.setGravity(Gravity.CENTER_VERTICAL);
        c.setPadding(dp(16), dp(14), dp(16), dp(14));
        c.setBackground(box(SURFACE));
        LinearLayout txt = new LinearLayout(this);
        txt.setOrientation(LinearLayout.VERTICAL);
        txt.addView(text(title, 15, TEXT, true));
        if (desc != null && !desc.isEmpty()) {
            TextView d = text(desc, 12, TEXT_DIM, false);
            d.setPadding(0, dp(3), 0, 0);
            txt.addView(d);
        }
        c.addView(txt, new LinearLayout.LayoutParams(0, -2, 1f));
        content.addView(c, rowLp());
        return c;
    }

    private void row(String title, String value, View.OnClickListener l) {
        LinearLayout c = card(title, "");
        TextView v = text(value + "  ›", 13, TEXT_DIM, false);
        c.addView(v);
        c.setOnClickListener(l);
    }

    private void action(String title, String desc, Runnable r) {
        LinearLayout c = card(title, desc);
        c.addView(text("›", 22, TEXT_DIM, false));
        c.setOnClickListener(v -> r.run());
    }

    private void disabledRow(String title, String reason) {
        LinearLayout c = card(title, reason);
        c.setAlpha(0.55f);
        c.addView(text("—", 15, TEXT_DIM, false));
    }

    private void switchPref(String title, String desc, String key, boolean def) {
        LinearLayout c = card(title, desc);
        Switch s = new Switch(this);
        s.setChecked(prefs.getBoolean(key, def));
        s.setOnCheckedChangeListener((b, on) -> prefs.edit().putBoolean(key, on).apply());
        c.addView(s);
        c.setOnClickListener(v -> s.toggle());
    }

    private void hudItem(String title, int bit, int items) {
        LinearLayout c = card(title, "");
        Switch s = new Switch(this);
        s.setChecked((items & bit) != 0);
        s.setOnCheckedChangeListener((b, on) -> {
            int cur = prefs.getInt("hud_items", HudPrefs.DEFAULT_ITEMS);
            prefs.edit().putInt("hud_items", on ? cur | bit : cur & ~bit).apply();
        });
        c.addView(s);
        c.setOnClickListener(v -> s.toggle());
    }

    /** Controle deslizante gravado como float (valor / div). */
    private void sliderPref(String title, String desc, String key, int min, int max, float div, float def, String unit) {
        LinearLayout c = card(title, desc);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setGravity(Gravity.START);
        TextView value = text("", 13, TEXT, true);
        SeekBar sb = new SeekBar(this);
        sb.setMax(max - min);
        int cur = Math.round(prefs.getFloat(key, def) * div);
        sb.setProgress(Math.max(0, Math.min(max - min, cur - min)));
        Runnable upd = () -> value.setText(String.format(Locale.US, "%d%s", sb.getProgress() + min, unit));
        upd.run();
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean u) {
                upd.run();
                if (u) prefs.edit().putFloat(key, (p + min) / div).apply();
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        c.addView(value);
        c.addView(sb, new LinearLayout.LayoutParams(-1, -2));
    }

    private void choicePref(String title, String desc, String[] names, String[] values, String key) {
        String cur = prefs.getString(key, "");
        choice(title, desc, names, values, cur, v -> prefs.edit().putString(key, v).apply());
    }

    private void choiceIni(String title, String desc, String[] names, String[] values, String section, String key, String cur) {
        choice(title, desc, names, values, cur, v -> {
            ini.set(section, key, v);
            if (!ini.save()) Toast.makeText(this, L.t("Não consegui gravar o config.ini"), Toast.LENGTH_LONG).show();
        });
    }

    private interface Setter { void set(String v); }

    private void choice(String title, String desc, String[] names, String[] values, String cur, Setter setter) {
        int sel = 0;
        for (int i = 0; i < values.length; i++) if (values[i].equalsIgnoreCase(cur)) sel = i;
        LinearLayout c = card(title, desc);
        TextView v = text(names[sel] + "  ›", 13, TEXT_DIM, false);
        v.setMaxWidth(dp(150));
        v.setGravity(Gravity.END);
        c.addView(v);
        final int selected = sel;
        c.setOnClickListener(x -> new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title)
                .setSingleChoiceItems(names, selected, (d, w) -> {
                    setter.set(values[w]);
                    d.dismiss();
                    showTab(tab);
                }).show());
    }

    // ------------------------------------------------------------------ utilidades

    private boolean isAdreno() {
        String soc = Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER : "";
        return "QTI".equalsIgnoreCase(soc) || Build.HARDWARE.toLowerCase(Locale.ROOT).contains("qcom");
    }

    private String gpuFamily() {
        String hw = (Build.HARDWARE + " " + (Build.VERSION.SDK_INT >= 31 ? Build.SOC_MODEL : "")).toLowerCase(Locale.ROOT);
        if (hw.contains("exynos") || hw.startsWith("s5e") || hw.contains("mt") || hw.contains("tensor")) return "GPU Mali";
        return L.t("outra GPU");
    }

    private static String[] trAll(String[] a) {
        String[] o = new String[a.length];
        for (int i = 0; i < a.length; i++) o[i] = L.t(a[i]);
        return o;
    }

    private static String readText(File f) {
        try (BufferedReader br = new BufferedReader(new FileReader(f))) {
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = br.readLine()) != null) sb.append(line).append('\n');
            return sb.toString().trim();
        } catch (Exception e) {
            return "";
        }
    }

    private void writeEnv(File f, String text) {
        String t = text.trim();
        try {
            if (t.isEmpty()) {
                //noinspection ResultOfMethodCallIgnored
                f.delete();
                return;
            }
            try (FileWriter w = new FileWriter(f)) {
                w.write(t + "\n");
            }
        } catch (Exception e) {
            Toast.makeText(this, L.t("Não consegui gravar re5_env.txt: ") + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private GradientDrawable box(int color) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(14));
        d.setStroke(dp(1), STROKE);
        return d;
    }

    private LinearLayout.LayoutParams rowLp() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        return lp;
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setTypeface(Typeface.create(bold ? "sans-serif-medium" : "sans-serif", bold ? Typeface.BOLD : Typeface.NORMAL));
        return t;
    }

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    static Intent open(Context c, int tab) {
        return new Intent(c, SettingsActivity.class).putExtra(EXTRA_TAB, tab);
    }
}
