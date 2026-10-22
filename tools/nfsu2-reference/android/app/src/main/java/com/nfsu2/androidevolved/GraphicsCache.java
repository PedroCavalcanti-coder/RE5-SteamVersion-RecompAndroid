package com.nfsu2.androidevolved;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.os.Process;
import java.io.File;
import java.util.List;

/** Only disposable graphics caches in this application's private cache directory. */
final class GraphicsCache {
    // Keep in sync with configurePipelineCache() in main.cpp. Epoch 2 excludes
    // caches from the previous DXVK preparation; those files remain untouched.
    static final String DIRECTORY = "dxvk-v2";

    static void prompt(Activity activity) {
        new AlertDialog.Builder(activity).setTitle("Reiniciar caché gráfica")
                .setMessage("Se cerrará la sesión actual del juego. Los shaders se volverán a compilar al jugar; las partidas y los ajustes se conservan.")
                .setNegativeButton("Cancelar", null)
                .setPositiveButton("Reiniciar", (dialog, which) -> reset(activity)).show();
    }

    private static void reset(Activity activity) {
        try {
            ActivityManager manager = (ActivityManager) activity.getSystemService(Activity.ACTIVITY_SERVICE);
            if (manager == null) throw new IllegalStateException("No se pudo comprobar el proceso del juego.");
            List<ActivityManager.RunningAppProcessInfo> running = manager.getRunningAppProcesses();
            if (running != null) for (ActivityManager.RunningAppProcessInfo process : running)
                if (process.uid == Process.myUid() && process.processName.equals(activity.getPackageName() + ":game"))
                    Process.killProcess(process.pid);
            File cache = new File(activity.getCacheDir(), DIRECTORY);
            if (cache.exists()) {
                File backup = new File(activity.getCacheDir(), DIRECTORY + "-reset-" + System.currentTimeMillis());
                if (!cache.isDirectory() || !cache.renameTo(backup))
                    throw new IllegalStateException("No se pudo reiniciar la caché gráfica.");
            }
            new AlertDialog.Builder(activity).setTitle("Caché gráfica preparada")
                    .setMessage("El próximo arranque generará una caché nueva. La anterior queda respaldada en la caché privada de la app.")
                    .setPositiveButton("Aceptar", null).show();
        } catch (RuntimeException error) {
            new AlertDialog.Builder(activity).setTitle("Caché gráfica").setMessage(error.getMessage())
                    .setPositiveButton("Aceptar", null).show();
        }
    }
}
