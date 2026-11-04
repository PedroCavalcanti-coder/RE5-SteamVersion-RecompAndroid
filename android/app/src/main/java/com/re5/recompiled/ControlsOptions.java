package com.re5.recompiled;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.util.TypedValue;
import android.view.View;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.Locale;

/**
 * Opções dos controles na tela (prefs "re5_settings"), usadas pelo launcher, pelo editor de
 * layout e pelo painel do jogo. {@link #apply} passa tudo para um {@link TouchControlsView}.
 */
public final class ControlsOptions {

    public static final String PREFS = "re5_settings";

    private ControlsOptions() {}

    public static void apply(Context ctx, TouchControlsView v) {
        SharedPreferences p = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        v.setControlsEnabled(p.getBoolean("touch_enabled", true));
        v.setControlsOpacity(p.getFloat("touch_opacity", 0.85f));
        v.setControlsScale(p.getFloat("touch_scale", 1.0f));
        v.setLookSensitivity(p.getFloat("touch_look_sens", 1.0f));
        v.setInvertY(p.getBoolean("touch_invert_y", false));
        v.setVibration(p.getBoolean("touch_vibration", true));
        v.setMenuButtons(p.getBoolean("touch_menu_buttons", false));
        v.setHideWithPad(p.getBoolean("touch_hide_with_pad", true));
    }

    /** Diálogo com controles deslizantes; onSaved roda depois de gravar. */
    public static void show(Activity a, Runnable onSaved) {
        SharedPreferences p = a.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        ScrollView scroll = new ScrollView(a);
        LinearLayout col = new LinearLayout(a);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(a, 20);
        col.setPadding(pad, dp(a, 12), pad, dp(a, 12));
        scroll.addView(col);

        CheckBox show = check(a, col, "Mostrar os controles na tela", p.getBoolean("touch_enabled", true));
        SeekBar sens = slider(a, col, "Sensibilidade da câmera", 50, 400, Math.round(p.getFloat("touch_look_sens", 1f) * 100), "%");
        CheckBox invert = check(a, col, "Inverter o eixo vertical da câmera", p.getBoolean("touch_invert_y", false));
        SeekBar opacity = slider(a, col, "Opacidade dos botões", 20, 100, Math.round(p.getFloat("touch_opacity", 0.85f) * 100), "%");
        SeekBar scale = slider(a, col, "Tamanho de todos os botões", 60, 160, Math.round(p.getFloat("touch_scale", 1f) * 100), "%");
        CheckBox vib = check(a, col, "Vibrar ao tocar nos botões", p.getBoolean("touch_vibration", true));
        CheckBox hide = check(a, col, "Esconder quando usar um controle físico", p.getBoolean("touch_hide_with_pad", true));

        col.addView(label(a, L.t("Nos menus do jogo")));
        RadioGroup menu = new RadioGroup(a);
        RadioButton byTouch = radio(a, L.t("Toque direto na tela (toque no item)"));
        RadioButton byButtons = radio(a, L.t("Botões (setas ou analógico + OK / Voltar)"));
        menu.addView(byTouch);
        menu.addView(byButtons);
        (p.getBoolean("touch_menu_buttons", false) ? byButtons : byTouch).setChecked(true);
        col.addView(menu);

        new AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(L.t("Opções dos controles"))
                .setView(scroll)
                .setPositiveButton(L.t("Salvar"), (d, w) -> {
                    p.edit()
                            .putBoolean("touch_enabled", show.isChecked())
                            .putFloat("touch_look_sens", (sens.getProgress() + 50) / 100f)
                            .putBoolean("touch_invert_y", invert.isChecked())
                            .putFloat("touch_opacity", (opacity.getProgress() + 20) / 100f)
                            .putFloat("touch_scale", (scale.getProgress() + 60) / 100f)
                            .putBoolean("touch_vibration", vib.isChecked())
                            .putBoolean("touch_hide_with_pad", hide.isChecked())
                            .putBoolean("touch_menu_buttons", byButtons.isChecked())
                            .apply();
                    if (onSaved != null) onSaved.run();
                })
                .setNegativeButton(L.t("Cancelar"), null)
                .show();
    }

    // ------------------------------------------------------------------ peças de interface

    static int dp(Context c, float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics()));
    }

    private static TextView label(Context c, String t) {
        TextView v = new TextView(c);
        v.setText(t);
        v.setTextColor(0xFFE5E7EB);
        v.setTextSize(14);
        v.setPadding(0, dp(c, 14), 0, dp(c, 4));
        return v;
    }

    private static CheckBox check(Context c, LinearLayout col, String t, boolean on) {
        CheckBox b = new CheckBox(c);
        b.setText(t);
        b.setTextColor(0xFFF3F4F6);
        b.setChecked(on);
        col.addView(b);
        return b;
    }

    private static RadioButton radio(Context c, String t) {
        RadioButton r = new RadioButton(c);
        r.setText(t);
        r.setTextColor(0xFFF3F4F6);
        r.setId(View.generateViewId());
        return r;
    }

    /** SeekBar de min a max (o progresso guarda valor - min), com o valor ao lado do título. */
    static SeekBar slider(Context c, LinearLayout col, String title, int min, int max, int value, String unit) {
        TextView l = label(c, "");
        SeekBar s = new SeekBar(c);
        s.setMax(max - min);
        s.setProgress(Math.max(0, Math.min(max - min, value - min)));
        Runnable upd = () -> l.setText(String.format(Locale.US, "%s: %d%s", title, s.getProgress() + min, unit));
        upd.run();
        s.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int pr, boolean u) { upd.run(); }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });
        col.addView(l);
        col.addView(s);
        return s;
    }
}
