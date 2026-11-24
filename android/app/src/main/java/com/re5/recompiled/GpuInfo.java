package com.re5.recompiled;

import android.content.SharedPreferences;
import android.os.Build;

import java.io.File;
import java.util.Locale;

/**
 * Família da GPU do aparelho e o renderizador Vulkan nativo que serve a ela
 * (RE5_GFX: vkadreno para Adreno, vkmali para Mali; runtime/include/re5/renderer.h).
 * O nó do driver do kernel decide primeiro (/dev/kgsl-3d0 = Adreno, /dev/mali0 = Mali);
 * o nome do SoC é só o desempate. O runtime confere o vendorID de verdade e aborta com
 * mensagem se a escolha não casar.
 */
final class GpuInfo {
    private GpuInfo() {}

    static final String PREF_RENDERER = "gfx_renderer";

    private static String soc() {
        String soc = (Build.VERSION.SDK_INT >= 31 && Build.SOC_MANUFACTURER != null) ? Build.SOC_MANUFACTURER : "";
        String model = (Build.VERSION.SDK_INT >= 31 && Build.SOC_MODEL != null) ? Build.SOC_MODEL : "";
        String hw = Build.HARDWARE != null ? Build.HARDWARE : "";
        return (soc + " " + model + " " + hw).toLowerCase(Locale.ROOT);
    }

    static boolean isAdreno() {
        if (new File("/dev/kgsl-3d0").exists()) return true;
        String s = soc();
        return s.contains("qti") || s.contains("qualcomm") || s.contains("qcom")
                || s.contains(" sm") || s.contains(" sdm") || s.contains(" qcs");
    }

    static boolean isMali() {
        if (isAdreno()) return false;
        if (new File("/dev/mali0").exists() || new File("/dev/mali").exists()) return true;
        String s = soc();
        /* Exynos 2200+ (Xclipse/AMD) e MediaTek com PowerVR existem: sem o nó, só o Tensor é certo. */
        return s.contains("tensor") || s.contains("google");
    }

    /** "vkadreno", "vkmali" ou null (nenhum renderizador nativo serve a esta GPU). */
    static String nativeRenderer() {
        if (isAdreno()) return "vkadreno";
        if (isMali()) return "vkmali";
        return null;
    }

    /**
     * O antigo "re5vk" saiu do runtime (que o recusa): vira o nativo da GPU do aparelho, ou a
     * DXVK quando nenhum serve. Valores desconhecidos também voltam à DXVK.
     */
    static void migrateRendererPref(SharedPreferences prefs) {
        String v = prefs.getString(PREF_RENDERER, "");
        if (v == null || v.isEmpty() || v.equals("dxvk") || v.equals("vkadreno") || v.equals("vkmali")) return;
        String to = v.equals("re5vk") ? nativeRenderer() : null;
        prefs.edit().putString(PREF_RENDERER, to != null ? to : "").apply();
    }
}
