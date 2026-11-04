package com.re5.recompiled;

import android.widget.RelativeLayout;

import java.io.BufferedReader;
import java.io.FileReader;

/** Monitor de desempenho (HUD): itens, posição e leituras do sistema. */
public final class HudPrefs {

    public static final int ITEM_FPS = 1, ITEM_MS = 2, ITEM_CPU = 4, ITEM_GPU = 8, ITEM_RAM = 16;
    public static final int DEFAULT_ITEMS = ITEM_FPS | ITEM_MS | ITEM_CPU | ITEM_GPU | ITEM_RAM;
    public static final int POS_TOP_LEFT = 0, POS_TOP_RIGHT = 1, POS_BOTTOM_LEFT = 2, POS_BOTTOM_RIGHT = 3;
    public static final String[] POS_NAMES = {"Canto superior esquerdo", "Canto superior direito",
            "Canto inferior esquerdo", "Canto inferior direito"};

    private HudPrefs() {}

    public static void place(RelativeLayout.LayoutParams lp, int pos, int mx, int my) {
        boolean right = pos == POS_TOP_RIGHT || pos == POS_BOTTOM_RIGHT;
        boolean bottom = pos == POS_BOTTOM_LEFT || pos == POS_BOTTOM_RIGHT;
        lp.addRule(right ? RelativeLayout.ALIGN_PARENT_RIGHT : RelativeLayout.ALIGN_PARENT_LEFT);
        lp.addRule(bottom ? RelativeLayout.ALIGN_PARENT_BOTTOM : RelativeLayout.ALIGN_PARENT_TOP);
        // à direita, abaixo da pílula do painel/ocultar; embaixo, acima da pausa
        lp.setMargins(right ? 0 : mx, bottom ? 0 : my + (right ? my * 4 : 0), right ? mx : 0, bottom ? my : 0);
    }

    /** CPU do processo do jogo (utime+stime de /proc/self/stat, em ticks de 10 ms), ou -1. O
     *  /proc/stat (CPU do sistema) é bloqueado para apps desde o Android 8: era o 0% de antes. */
    public static long processCpuTicks() {
        try (BufferedReader br = new BufferedReader(new FileReader("/proc/self/stat"))) {
            String l = br.readLine();
            if (l == null) return -1;
            // o nome do processo (campo 2) vem entre parênteses e pode ter espaços
            String[] t = l.substring(l.lastIndexOf(')') + 2).trim().split("\\s+");
            return Long.parseLong(t[11]) + Long.parseLong(t[12]);   // campos 14 e 15
        } catch (Exception e) {
            return -1;
        }
    }

    /** Uso da GPU em % (Exynos/Mali da Samsung: /sys/kernel/gpu/gpu_busy), ou -1. */
    public static int gpuBusy() {
        String s = readFirstLine("/sys/kernel/gpu/gpu_busy");
        if (s == null) s = readFirstLine("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");   // Adreno
        if (s == null) return -1;
        try {
            return Integer.parseInt(s.replace("%", "").trim().split("\\s+")[0]);
        } catch (Exception e) {
            return -1;
        }
    }

    /** Frequência da GPU em MHz, ou -1. */
    public static int gpuMhz() {
        String s = readFirstLine("/sys/kernel/gpu/gpu_clock");
        if (s == null) {
            s = readFirstLine("/sys/class/kgsl/kgsl-3d0/gpuclk");
            if (s != null) try { return (int) (Long.parseLong(s.trim()) / 1000000L); } catch (Exception e) { return -1; }
        }
        if (s == null) return -1;
        try {
            return Integer.parseInt(s.trim());
        } catch (Exception e) {
            return -1;
        }
    }

    private static String readFirstLine(String path) {
        try (BufferedReader br = new BufferedReader(new FileReader(path))) {
            return br.readLine();
        } catch (Exception e) {
            return null;
        }
    }
}
