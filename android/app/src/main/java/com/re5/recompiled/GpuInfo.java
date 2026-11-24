package com.re5.recompiled;

import android.os.Build;

import java.io.File;
import java.util.Locale;

/**
 * Família da GPU do aparelho (Adreno ou Mali), para os ajustes da tela de configurações
 * (driver Vulkan customizado e Modo Turbo, só Adreno). O nó do driver do kernel decide
 * primeiro (/dev/kgsl-3d0 = Adreno, /dev/mali0 = Mali); o nome do SoC é só o desempate.
 */
final class GpuInfo {
    private GpuInfo() {}

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
}
