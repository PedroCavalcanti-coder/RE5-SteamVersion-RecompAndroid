package com.re5.recompiled;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.Uri;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

import java.io.File;

/**
 * Extração do jogo num serviço em primeiro plano: antes rodava numa thread solta do launcher e o
 * Android a matava quando o app saía da tela (ou a activity era recriada), e a extração recomeçava.
 * Agora ela segue com o app fechado, mostra o progresso na notificação, e o launcher só lê
 * {@link #state}. A extração é retomável (ArchiveExtractor pula o que já está completo).
 */
public class ExtractService extends Service {
    private static final String CHANNEL = "re5_extract";
    private static final int NOTIF_ID = 5;
    private static final String EXTRA_URI = "uri", EXTRA_PATH = "path";

    /** Estado lido pelo launcher (thread de UI) a cada 500 ms. */
    public static final class State {
        public volatile boolean running, finished, reported;
        public volatile String detail = "", error;
        public volatile long files;
        public volatile int permille = -1;   // -1 = indeterminado
        public volatile Uri uri;
    }

    public static final State state = new State();

    private PowerManager.WakeLock wake;
    private long lastNotify;

    public static boolean start(Context ctx, Uri uri, String directPath) {
        if (state.running) return false;
        state.running = true;
        state.finished = false;
        state.reported = false;
        state.error = null;
        state.detail = "";
        state.files = 0;
        state.permille = -1;
        state.uri = uri;
        Intent i = new Intent(ctx, ExtractService.class);
        if (uri != null) i.putExtra(EXTRA_URI, uri.toString());
        if (directPath != null) i.putExtra(EXTRA_PATH, directPath);
        ctx.startForegroundService(i);
        return true;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null && nm.getNotificationChannel(CHANNEL) == null)
            nm.createNotificationChannel(new NotificationChannel(CHANNEL, L.t("Extração do jogo"),
                    NotificationManager.IMPORTANCE_LOW));
        Notification n = notification(L.t("Preparando…"), -1);
        if (Build.VERSION.SDK_INT >= 29) startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        else startForeground(NOTIF_ID, n);

        if (intent == null) {   // recriado pelo sistema sem pedido: nada a fazer
            finish(null);
            return START_NOT_STICKY;
        }
        PowerManager pm = getSystemService(PowerManager.class);
        if (pm != null) {
            wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "re5:extract");
            wake.acquire(6 * 60 * 60 * 1000L);
        }
        String u = intent.getStringExtra(EXTRA_URI);
        Uri uri = u != null ? Uri.parse(u) : null;
        String path = intent.getStringExtra(EXTRA_PATH);
        File target = GameStorage.gameDir(this);
        new ArchiveExtractor().extractArchive(this, uri, path, target, new ArchiveExtractor.Callback() {
            @Override
            public void onProgress(String currentFile, long processedCount, int permille) {
                state.files = processedCount;
                state.detail = currentFile;
                if (permille >= 0) state.permille = permille;
                long now = System.currentTimeMillis();
                if (now - lastNotify > 1000) {
                    lastNotify = now;
                    if (nm != null) nm.notify(NOTIF_ID, notification(currentFile, state.permille));
                }
            }

            @Override
            public void onSuccess(int totalFiles, long totalBytes) {
                finish(null);
            }

            @Override
            public void onError(String error) {
                finish(error);
            }
        });
        return START_NOT_STICKY;
    }

    private void finish(String error) {
        state.error = error;
        state.finished = true;
        state.running = false;
        if (wake != null && wake.isHeld()) wake.release();
        stopForeground(STOP_FOREGROUND_REMOVE);
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {   // aviso final (fica até tocar)
            Notification.Builder b = new Notification.Builder(this, CHANNEL)
                    .setSmallIcon(error == null ? android.R.drawable.stat_sys_download_done : android.R.drawable.stat_notify_error)
                    .setContentTitle(error == null ? L.t("Jogo extraído") : L.t("Erro na extração"))
                    .setContentText(error == null ? L.t("Toque para abrir.") : error)
                    .setContentIntent(openLauncher())
                    .setAutoCancel(true);
            nm.notify(NOTIF_ID + 1, b.build());
        }
        stopSelf();
    }

    private PendingIntent openLauncher() {
        Intent i = new Intent(this, LauncherActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(this, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private Notification notification(String text, int permille) {
        Notification.Builder b = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle(L.t("Extraindo arquivos do jogo…"))
                .setContentText(text)
                .setOnlyAlertOnce(true)
                .setOngoing(true)
                .setContentIntent(openLauncher());
        if (permille >= 0) b.setProgress(1000, permille, false);
        else b.setProgress(0, 0, true);
        return b.build();
    }
}
