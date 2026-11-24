package com.re5.recompiled;

import android.content.Context;
import android.net.Uri;
import android.os.Environment;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class ArchiveExtractor {
    private static final String TAG = "ArchiveExtractor";
    private static final int BUFFER_SIZE = 64 * 1024;

    public interface Callback {
        void onProgress(String currentFile, long processedCount, int percent);
        void onSuccess(int totalFiles, long totalBytes);
        void onError(String error);
    }

    private volatile boolean isCancelled = false;
    private Process unrarProcess = null;

    public void cancel() {
        isCancelled = true;
        if (unrarProcess != null) {
            try {
                unrarProcess.destroy();
            } catch (Exception ignored) {}
        }
    }

    private static File findExecutableUnrar(Context context) {
        // 1. Tenta carregar a partir de nativeLibraryDir (extraído pelo Android com permissão RX)
        File nativeLib = new File(context.getApplicationInfo().nativeLibraryDir, "libunrar.so");
        if (nativeLib.exists() && nativeLib.canExecute()) {
            return nativeLib;
        }

        // 2. Tenta em /data/local/tmp/unrar
        File tmpUnrar = new File("/data/local/tmp/unrar");
        if (tmpUnrar.exists() && tmpUnrar.canExecute()) {
            return tmpUnrar;
        }

        // 3. Tenta em codeCacheDir (geralmente permite execução)
        File codeCache = new File(context.getCodeCacheDir(), "unrar");
        if (extractFromAssets(context, codeCache)) {
            codeCache.setExecutable(true, false);
            return codeCache;
        }

        return nativeLib;
    }

    private static boolean extractFromAssets(Context context, File target) {
        try (InputStream in = context.getAssets().open("unrar");
             FileOutputStream out = new FileOutputStream(target)) {
            byte[] buf = new byte[BUFFER_SIZE];
            int r;
            while ((r = in.read(buf)) != -1) {
                out.write(buf, 0, r);
            }
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Falha ao extrair asset unrar", e);
            return false;
        }
    }

    public void extractArchive(Context context, Uri archiveUri, String directFilePath, File targetDir, Callback callback) {
        new Thread(() -> {
            try {
                if (!targetDir.exists() && !targetDir.mkdirs()) {
                    callback.onError(L.t("Não foi possível criar a pasta: ") + targetDir.getAbsolutePath());
                    return;
                }

                String fileName = "";
                if (directFilePath != null) {
                    fileName = new File(directFilePath).getName();
                } else if (archiveUri != null) {
                    fileName = archiveUri.getLastPathSegment();
                }
                if (fileName == null) fileName = "arquivo";
                fileName = fileName.toLowerCase();

                boolean isRar = fileName.endsWith(".rar");

                if (isRar) {
                    extractRarNative(context, archiveUri, directFilePath, targetDir, callback);
                } else {
                    extractZip(context, archiveUri, targetDir, callback);
                }
            } catch (Exception e) {
                Log.e(TAG, "Falha na extração", e);
                callback.onError(L.t("Erro na descompactação: ") + e.getMessage());
            }
        }).start();
    }

    private void extractRarNative(Context context, Uri archiveUri, String directFilePath, File targetDir, Callback callback) throws Exception {
        File unrarBin = findExecutableUnrar(context);

        String rarPath = directFilePath;
        File tempRar = null;

        if (rarPath == null && archiveUri != null) {
            // Tenta obter o caminho direto de Download caso venha de /sdcard
            File test = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "game.rar");
            if (test.exists()) {
                rarPath = test.getAbsolutePath();
            } else {
                tempRar = new File(context.getCacheDir(), "import_temp.rar");
                try (InputStream in = context.getContentResolver().openInputStream(archiveUri);
                     FileOutputStream out = new FileOutputStream(tempRar)) {
                    if (in == null) {
                        callback.onError(L.t("Não foi possível abrir o arquivo."));
                        return;
                    }
                    byte[] buf = new byte[BUFFER_SIZE];
                    int r;
                    long copied = 0;
                    while ((r = in.read(buf)) != -1) {
                        if (isCancelled) {
                            if (tempRar.exists()) tempRar.delete();
                            callback.onError(L.t("Cancelado."));
                            return;
                        }
                        out.write(buf, 0, r);
                        copied += r;
                        callback.onProgress(L.t("Copiando arquivo para extração..."), copied / (1024 * 1024), -1);
                    }
                }
                rarPath = tempRar.getAbsolutePath();
            }
        }

        if (rarPath == null) {
            callback.onError(L.t("Arquivo .rar não encontrado."));
            return;
        }

        long archiveSize = new File(rarPath).length();

        // Retomável: lista o arquivo, apaga o que ficou pela metade (tamanho diferente) e extrai
        // pulando o que já está completo (-o-). Se tudo está numa pasta "game/" (o game.rar), extrai
        // na pasta acima, direto no lugar: antes ia para game/game/ e era movido depois, e uma
        // segunda tentativa deixava uma cópia inteira em game/game/ (o jogo passava de 16 GB).
        callback.onProgress(L.t("Lendo o conteúdo do arquivo…"), 0, -1);
        java.util.List<RarEntry> entries = listRar(unrarBin, rarPath);
        File base = targetDir;
        boolean wrapped = allUnder(entries, targetDir.getName());
        if (wrapped && targetDir.getParentFile() != null) {
            base = targetDir.getParentFile();
            File nested = new File(targetDir, targetDir.getName());   // sobra das extrações antigas
            if (nested.isDirectory()) {
                callback.onProgress(L.t("Apagando cópia duplicada de uma extração anterior…"), 0, -1);
                GameStorage.deleteTree(nested);
            }
        }
        long total = 0, done = 0;
        int skipped = 0;
        java.util.Map<String, Long> sizes = new java.util.HashMap<>();
        for (RarEntry e : entries) {
            if (e.dir || e.size < 0) continue;
            total += e.size;
            sizes.put(e.name, e.size);
            File f = new File(base, e.name);
            if (f.isFile()) {
                if (f.length() == e.size) {
                    done += e.size;
                    skipped++;
                } else if (!f.delete()) {
                    Log.w(TAG, "não apaguei o arquivo incompleto " + f);
                }
            }
        }
        boolean resume = !sizes.isEmpty();
        if (skipped > 0) Log.i(TAG, "retomando: " + skipped + " arquivos já completos");

        ProcessBuilder pb = new ProcessBuilder(
                unrarBin.getAbsolutePath(),
                "x",
                "-y",
                resume ? "-o-" : "-o+",
                rarPath,
                base.getAbsolutePath() + File.separator
        );
        pb.redirectErrorStream(true);
        unrarProcess = pb.start();

        int fileCount = 0;

        try (BufferedReader reader = new BufferedReader(new InputStreamReader(unrarProcess.getInputStream()))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (isCancelled) {
                    unrarProcess.destroy();
                    callback.onError(L.t("Extração cancelada pelo usuário."));
                    return;
                }
                line = line.trim();
                if (line.startsWith("Extracting ") || line.startsWith("Extraindo ")) {
                    fileCount++;
                    String display = line.replace("Extracting ", "").replace("Extraindo ", "").trim();
                    // "nome   OK": o arquivo terminou
                    String name = display.endsWith("OK") ? display.substring(0, display.length() - 2).trim() : display;
                    Long sz = sizes.get(name.replace('\\', '/'));
                    if (sz != null) done += sz;
                    callback.onProgress(name, skipped + fileCount,
                            total > 0 ? (int) Math.min(1000, done * 1000 / total) : -1);
                }
            }
        }

        int exitCode = unrarProcess.waitFor();

        if (tempRar != null && tempRar.exists()) {
            tempRar.delete();
        }

        if (exitCode == 0 || exitCode == 1) {
            if (!wrapped) organizeExtractedFolder(targetDir);
            callback.onSuccess(fileCount, archiveSize);
        } else {
            callback.onError(L.t("UnRAR falhou com código de saída: ") + exitCode);
        }
    }

    private void organizeExtractedFolder(File targetDir) {
        File subGame = new File(targetDir, "game");
        if (subGame.exists() && subGame.isDirectory()) {
            File[] files = subGame.listFiles();
            if (files != null) for (File f : files) mergeInto(f, new File(targetDir, f.getName()));
            GameStorage.deleteTree(subGame);
        }
    }

    /** Move src para dest juntando pastas (renameTo falha se dest já existe: era a duplicação). */
    private static void mergeInto(File src, File dest) {
        if (src.isDirectory()) {
            if (!dest.exists() && src.renameTo(dest)) return;
            dest.mkdirs();
            File[] kids = src.listFiles();
            if (kids != null) for (File k : kids) mergeInto(k, new File(dest, k.getName()));
            src.delete();
        } else {
            if (dest.exists()) dest.delete();
            if (!src.renameTo(dest)) Log.w(TAG, "não movi " + src + " -> " + dest);
        }
    }

    /** Uma entrada do arquivo (unrar lt): caminho com '/', tamanho, pasta ou não. */
    private static final class RarEntry {
        String name;
        long size = -1;
        boolean dir;
    }

    private static java.util.List<RarEntry> listRar(File unrarBin, String rarPath) {
        java.util.List<RarEntry> out = new java.util.ArrayList<>();
        try {
            Process p = new ProcessBuilder(unrarBin.getAbsolutePath(), "lt", "-c-", rarPath)
                    .redirectErrorStream(true).start();
            RarEntry cur = null;
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                String line;
                while ((line = r.readLine()) != null) {
                    String t = line.trim();
                    if (t.startsWith("Name: ")) {
                        cur = new RarEntry();
                        cur.name = t.substring(6).replace('\\', '/');
                        out.add(cur);
                    } else if (cur != null && t.startsWith("Type: ")) {
                        cur.dir = t.substring(6).trim().equalsIgnoreCase("Directory");
                    } else if (cur != null && t.startsWith("Size: ")) {
                        try { cur.size = Long.parseLong(t.substring(6).trim()); } catch (NumberFormatException ignored) {}
                    }
                }
            }
            p.waitFor();
        } catch (Exception e) {
            Log.w(TAG, "unrar lt: " + e);
        }
        return out;
    }

    private static boolean allUnder(java.util.List<RarEntry> entries, String top) {
        if (entries.isEmpty()) return false;
        for (RarEntry e : entries)
            if (!e.name.equals(top) && !e.name.startsWith(top + "/")) return false;
        return true;
    }

    private void extractZip(Context context, Uri archiveUri, File targetDir, Callback callback) throws Exception {
        InputStream rawInput = context.getContentResolver().openInputStream(archiveUri);
        if (rawInput == null) {
            callback.onError(L.t("Não foi possível abrir o arquivo ZIP selecionado."));
            return;
        }

        long totalBytesExtracted = 0;
        int fileCount = 0;
        byte[] buffer = new byte[BUFFER_SIZE];

        try (ZipInputStream zis = new ZipInputStream(new BufferedInputStream(rawInput, BUFFER_SIZE))) {
            ZipEntry entry;
            while ((entry = zis.getNextEntry()) != null) {
                if (isCancelled) {
                    callback.onError(L.t("Extração cancelada pelo usuário."));
                    return;
                }

                String entryName = entry.getName();
                File targetFile = new File(targetDir, entryName);

                String canonicalDest = targetDir.getCanonicalPath();
                String canonicalTarget = targetFile.getCanonicalPath();
                if (!canonicalTarget.startsWith(canonicalDest + File.separator) && !canonicalTarget.equals(canonicalDest)) {
                    zis.closeEntry();
                    continue;
                }

                if (entry.isDirectory()) {
                    if (!targetFile.exists()) targetFile.mkdirs();
                } else {
                    File parent = targetFile.getParentFile();
                    if (parent != null && !parent.exists()) parent.mkdirs();
                    if (entry.getSize() >= 0 && targetFile.isFile() && targetFile.length() == entry.getSize()) {
                        fileCount++;   // já extraído numa tentativa anterior
                        zis.closeEntry();
                        callback.onProgress(entryName, fileCount, -1);
                        continue;
                    }

                    try (BufferedOutputStream bos = new BufferedOutputStream(new FileOutputStream(targetFile), BUFFER_SIZE)) {
                        int len;
                        while ((len = zis.read(buffer)) > 0) {
                            if (isCancelled) {
                                callback.onError(L.t("Extração cancelada."));
                                return;
                            }
                            bos.write(buffer, 0, len);
                            totalBytesExtracted += len;
                        }
                    }
                    fileCount++;
                }
                zis.closeEntry();

                callback.onProgress(entryName, fileCount, -1);
            }
        }

        organizeExtractedFolder(targetDir);
        callback.onSuccess(fileCount, totalBytesExtracted);
    }
}
