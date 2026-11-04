package com.re5.recompiled;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.util.ArrayList;
import java.util.List;

/**
 * O config.ini do próprio jogo (Documents/CAPCOM/RESIDENT EVIL 5 no drive C do runtime): o
 * launcher troca só as chaves que oferece e preserva o resto (seções, ordem, chaves desconhecidas).
 * O jogo relê o arquivo ao abrir.
 */
public final class GameConfigIni {

    private final File file;
    private final List<String> lines = new ArrayList<>();

    public GameConfigIni(File gameDir) {
        file = new File(gameDir, "user/drive_c/Users/Player/Documents/CAPCOM/RESIDENT EVIL 5/config.ini");
        try (BufferedReader r = new BufferedReader(new FileReader(file))) {
            String l;
            while ((l = r.readLine()) != null) lines.add(l);
        } catch (Exception ignored) {}
    }

    /** O arquivo existe (o jogo cria na primeira vez que abre). */
    public boolean exists() { return file.isFile(); }

    public String get(String section, String key, String def) {
        String cur = "";
        for (String l : lines) {
            String t = l.trim();
            if (t.startsWith("[") && t.endsWith("]")) {
                cur = t.substring(1, t.length() - 1);
                continue;
            }
            int eq = t.indexOf('=');
            if (eq > 0 && cur.equalsIgnoreCase(section) && t.substring(0, eq).trim().equalsIgnoreCase(key))
                return t.substring(eq + 1).trim();
        }
        return def;
    }

    public void set(String section, String key, String value) {
        String cur = "";
        int sectionEnd = -1;
        for (int i = 0; i < lines.size(); i++) {
            String t = lines.get(i).trim();
            if (t.startsWith("[") && t.endsWith("]")) {
                if (cur.equalsIgnoreCase(section)) break;
                cur = t.substring(1, t.length() - 1);
                if (cur.equalsIgnoreCase(section)) sectionEnd = i + 1;
                continue;
            }
            if (cur.equalsIgnoreCase(section)) {
                sectionEnd = i + 1;
                int eq = t.indexOf('=');
                if (eq > 0 && t.substring(0, eq).trim().equalsIgnoreCase(key)) {
                    lines.set(i, key + "=" + value);
                    return;
                }
            }
        }
        if (sectionEnd < 0) {   // seção nova no fim
            lines.add("[" + section + "]");
            lines.add(key + "=" + value);
        } else {
            lines.add(sectionEnd, key + "=" + value);
        }
    }

    public boolean save() {
        File parent = file.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) return false;
        try (FileWriter w = new FileWriter(file)) {
            for (String l : lines) w.write(l + "\r\n");
            return true;
        } catch (Exception e) {
            return false;
        }
    }
}
