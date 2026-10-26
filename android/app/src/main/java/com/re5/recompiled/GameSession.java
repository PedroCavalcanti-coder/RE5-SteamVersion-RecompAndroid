package com.re5.recompiled;

import android.content.Context;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.Deque;
import java.util.List;
import java.util.Locale;

/**
 * Mostra no launcher por que o jogo fechou.
 *
 * O jogo roda no processo ":game"; um abort() do runtime (re5_fatal) ou um sinal
 * derruba esse processo inteiro sem tela de erro, e um erro de inicialização faz
 * o SDL_main retornar e a tela simplesmente fechar. O launcher grava a hora ao
 * abrir o jogo; ao voltar, lê o logcat do próprio app desde essa hora (o Android
 * deixa um app ler os logs do seu UID) e mostra a mensagem de erro de verdade.
 */
public final class GameSession {
    private static final String MARKER = "game_running";

    private GameSession() {}

    /** Grava o marcador com a hora de início, no formato do "logcat -T". */
    public static void markStarted(Context ctx) {
        String now = new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US).format(new Date());
        try (FileWriter w = new FileWriter(new File(ctx.getFilesDir(), MARKER))) {
            w.write(now);
        } catch (IOException ignored) {
        }
    }

    /**
     * Devolve (uma única vez) a hora em que o jogo foi aberto pelo launcher, ou null
     * se não houve partida desde a última consulta.
     */
    public static String consumeLaunch(Context ctx) {
        File f = new File(ctx.getFilesDir(), MARKER);
        if (!f.exists()) return null;
        String start = "";
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String line = r.readLine();
            if (line != null) start = line.trim();
        } catch (IOException ignored) {
        }
        f.delete();
        return start;
    }

    /** Últimas linhas do log do runtime (RE5Native) e do SDL desde {@code since}. */
    public static String readRecentLog(String since, int maxLines) {
        Deque<String> lines = new ArrayDeque<>();
        Process p = null;
        try {
            List<String> cmd = new ArrayList<>(Arrays.asList("logcat", "-d", "-v", "time"));
            if (since != null && !since.isEmpty()) {
                cmd.add("-T");
                cmd.add(since);
            }
            cmd.addAll(Arrays.asList("-s", "RE5Native:V", "SDL:V", "RE5GameActivity:V", "AndroidRuntime:E", "libc:F"));
            p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                String line;
                while ((line = r.readLine()) != null) {
                    if (line.startsWith("---------")) continue;
                    lines.addLast(line);
                    if (lines.size() > maxLines) lines.removeFirst();
                }
            }
        } catch (IOException e) {
            return "";
        } finally {
            if (p != null) p.destroy();
        }
        return String.join("\n", lines);
    }

    /**
     * A causa mais provável: a última linha de erro do runtime ("[ERROR] canal msg"),
     * ou de abort/sinal. Null se o log não tiver erro (p.ex. o usuário fechou o app
     * pela lista de recentes).
     */
    public static String findError(String log) {
        String found = null;
        for (String line : log.split("\n")) {
            int i = line.indexOf("[ERROR]");
            if (i >= 0) {
                // re5_fatal imprime a causa e depois a trilha de dispatch (canal "dispatch").
                String msg = line.substring(i + 7).trim();
                if (!msg.startsWith("dispatch")) found = msg;
            } else if (found == null && (line.contains("Fatal signal") || line.contains("FATAL EXCEPTION")
                    || line.contains("abort"))) {
                int c = line.indexOf("): ");
                found = c >= 0 ? line.substring(c + 3).trim() : line.trim();
            }
        }
        return found;
    }
}
