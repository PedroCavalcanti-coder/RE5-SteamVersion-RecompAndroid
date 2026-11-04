package com.re5.recompiled;

import android.content.Context;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Locale;
import java.util.Map;

/**
 * Idiomas do app (launcher, configurações, editor, painel e HUD do jogo).
 *
 * A interface é montada em código, então a chave de cada texto é o próprio texto em português:
 * {@code L.t("Jogar")} devolve a tradução de assets/i18n/<idioma>.json, ou o original se faltar.
 * Idioma: o escolhido no app (prefs "app_lang") ou o do sistema; sem tradução para o sistema,
 * inglês. Para corrigir uma tradução basta editar o JSON.
 */
public final class L {

    public static final String[] CODES = {"", "pt", "en", "es", "zh", "ru", "ja"};
    public static final String[] NAMES = {"Idioma do sistema", "Português (Brasil)", "English", "Español", "中文 (简体)",
            "Русский", "日本語"};

    private static Map<String, String> map = Collections.emptyMap();
    private static String lang = "pt";

    private L() {}

    /** Carrega o idioma (chamar no onCreate de cada activity, antes de montar a tela). */
    public static synchronized void init(Context c) {
        String want = c.getSharedPreferences(ControlsOptions.PREFS, Context.MODE_PRIVATE).getString("app_lang", "");
        if (want == null || want.isEmpty()) {
            String sys = Locale.getDefault().getLanguage();
            want = "en";
            for (String code : CODES) if (!code.isEmpty() && code.equals(sys)) want = code;
        }
        if (want.equals(lang) && (want.equals("pt") || !map.isEmpty())) return;
        lang = want;
        if (want.equals("pt")) {
            map = Collections.emptyMap();
            return;
        }
        Map<String, String> m = new HashMap<>();
        try (InputStream in = c.getAssets().open("i18n/" + want + ".json")) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            JSONObject o = new JSONObject(new String(out.toByteArray(), StandardCharsets.UTF_8));
            for (Iterator<String> it = o.keys(); it.hasNext(); ) {
                String k = it.next();
                m.put(k, o.getString(k));
            }
        } catch (Exception ignored) {}
        map = m;
    }

    public static String lang() { return lang; }

    public static String t(String pt) {
        String v = map.get(pt);
        return v != null ? v : pt;
    }

    /** Índice de CODES do idioma salvo ("" = sistema). */
    public static int savedIndex(Context c) {
        String s = c.getSharedPreferences(ControlsOptions.PREFS, Context.MODE_PRIVATE).getString("app_lang", "");
        for (int i = 0; i < CODES.length; i++) if (CODES[i].equals(s)) return i;
        return 0;
    }

    public static void save(Context c, int index) {
        c.getSharedPreferences(ControlsOptions.PREFS, Context.MODE_PRIVATE).edit().putString("app_lang", CODES[index]).apply();
        init(c);
    }
}
