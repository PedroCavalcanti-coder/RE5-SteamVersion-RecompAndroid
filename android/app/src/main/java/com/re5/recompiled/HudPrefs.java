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

    /** Thread mais ocupada desde a amostra anterior. pct = % de UM núcleo (100 = uma thread que não
     *  para). A CPU do processo no HUD é a média de todos os núcleos: num aparelho de 8 núcleos a
     *  thread principal no limite aparece como ~12%, e o HUD parecia dizer "CPU sobrando". */
    public static final class BusiestThread {
        public float pct = -1f;
        public String name = "";
    }

    /** Lê utime+stime de /proc/self/task/*&#47;stat; prev guarda os tiques da amostra anterior por tid. */
    public static BusiestThread busiestThread(java.util.Map<Integer, Long> prev, long wallDeltaMs) {
        BusiestThread out = new BusiestThread();
        java.util.Map<Integer, Long> cur = new java.util.HashMap<>();
        java.io.File[] tasks = new java.io.File("/proc/self/task").listFiles();
        if (tasks == null) return out;
        long best = -1;
        for (java.io.File t : tasks) {
            int tid;
            try {
                tid = Integer.parseInt(t.getName());
            } catch (NumberFormatException e) {
                continue;
            }
            try (BufferedReader br = new BufferedReader(new FileReader(new java.io.File(t, "stat")))) {
                String l = br.readLine();
                if (l == null) continue;
                int a = l.indexOf('('), b = l.lastIndexOf(')');
                if (a < 0 || b < a) continue;
                String[] f = l.substring(b + 2).trim().split("\\s+");
                long ticks = Long.parseLong(f[11]) + Long.parseLong(f[12]);
                cur.put(tid, ticks);
                Long p = prev.get(tid);
                if (p != null && ticks - p > best) {
                    best = ticks - p;
                    out.name = l.substring(a + 1, b);
                }
            } catch (Exception ignored) {
                // a thread saiu entre o listFiles e a leitura
            }
        }
        prev.clear();
        prev.putAll(cur);
        if (best >= 0 && wallDeltaMs > 0) out.pct = Math.min(100f, 100f * best * 10f / wallDeltaMs);   // 1 tique = 10 ms
        return out;
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
            long v = Long.parseLong(s.trim());
            // Exynos (/sys/kernel/gpu/gpu_clock) dá kHz em vários kernels (949000 no A54): nenhuma
            // GPU de celular passa de 10 GHz, então valor acima de 10000 é kHz.
            return (int) (v > 10000L ? v / 1000L : v);
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
