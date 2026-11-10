package com.re5.recompiled;

import android.content.Context;
import android.os.Environment;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Onde ficam os arquivos do jogo.
 *
 * A pasta é a área própria do app no armazenamento externo
 * (Android/data/com.re5.recompiled/files/game): o Android conta o tamanho dela no
 * app, apaga tudo ao desinstalar, e o GameFilesProvider a mostra no app Arquivos.
 * Instalações antigas usavam /sdcard/re5, que o Android não apaga nem conta;
 * {@link #moveTree} traz esses arquivos para cá.
 */
public final class GameStorage {
    public static final String EXE_NAME = "re5dx9.exe";
    public static final String CLEAN_EXE_NAME = "re5dx9.clean.exe";
    public static final String DATA_DIR_NAME = "nativePC_MT";

    private GameStorage() {}

    public interface Progress {
        /** Chamado de uma thread de trabalho. */
        void onProgress(String currentFile, long doneBytes, long totalBytes);
    }

    public static File gameDir(Context ctx) {
        File base = ctx.getExternalFilesDir(null);
        if (base == null) base = ctx.getFilesDir();   // armazenamento externo indisponível
        File dir = new File(base, "game");
        if (!dir.exists()) dir.mkdirs();
        return dir;
    }

    /** Pasta pública usada pelas versões anteriores do app. */
    public static File legacyDir() {
        return new File(Environment.getExternalStorageDirectory(), "re5");
    }

    public static File downloadedRar() {
        return new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "game.rar");
    }

    public static boolean hasData(File dir) {
        File data = new File(dir, DATA_DIR_NAME);
        return data.isDirectory();
    }

    public static boolean hasExe(File dir) {
        return new File(dir, EXE_NAME).isFile() || new File(dir, CLEAN_EXE_NAME).isFile();
    }

    /** SHA-256 do re5dx9.clean.exe para o qual o código deste APK foi gerado
     *  (config/known_builds.json, "steam-2023-02-13-oep-dump-reconstructed"). */
    public static final String CLEAN_EXE_SHA256 = "4534003fe7949750c8c2bffd337f9d735eb9d27498a66e77e77cf1a87dfa4036";

    /** O APK não leva nenhum arquivo do jogo: o re5dx9.clean.exe vem da pasta do jogo do usuário,
     *  preparado por ele. null se está lá e é a build certa; senão, a mensagem para o usuário. */
    public static String checkCleanExe(File dir) {
        File exe = new File(dir, CLEAN_EXE_NAME);
        if (!exe.isFile())
            return L.t("Falta o re5dx9.clean.exe na pasta do jogo. Este app não traz arquivos do jogo: coloque o seu ")
                    + L.t("re5dx9.clean.exe (executável sem proteção, gerado da sua cópia) junto da pasta nativePC_MT, ")
                    + L.t("ou inclua-o no .zip/.rar do jogo.") + "\n\n" + dir.getAbsolutePath();
        String h = sha256(exe);
        if (!CLEAN_EXE_SHA256.equalsIgnoreCase(h))
            return L.t("O re5dx9.clean.exe da pasta do jogo não é a build suportada (Steam, 13/02/2023).")
                    + "\n\nSHA-256: " + h + "\n" + L.t("esperado: ") + CLEAN_EXE_SHA256;
        return null;
    }

    static String sha256(File f) {
        try (java.io.InputStream in = new java.io.FileInputStream(f)) {
            java.security.MessageDigest md = java.security.MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[1 << 16];
            int r;
            while ((r = in.read(buf)) != -1) md.update(buf, 0, r);
            StringBuilder sb = new StringBuilder();
            for (byte b : md.digest()) sb.append(String.format(java.util.Locale.US, "%02x", b));
            return sb.toString();
        } catch (Exception e) {
            return "?";
        }
    }

    public static boolean isInstalled(File dir) {
        return hasData(dir) && hasExe(dir);
    }

    public static long sizeOf(File f) {
        if (f.isFile()) return f.length();
        long total = 0;
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) total += sizeOf(c);
        }
        return total;
    }

    public static boolean deleteTree(File f) {
        boolean ok = true;
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) ok &= deleteTree(c);
        }
        return f.delete() && ok;
    }

    public static String formatSize(long bytes) {
        if (bytes >= 1L << 30) return String.format(Locale.getDefault(), L.t("%.1f GB"), bytes / (double) (1L << 30));
        if (bytes >= 1L << 20) return String.format(Locale.getDefault(), L.t("%.0f MB"), bytes / (double) (1L << 20));
        return String.format(Locale.getDefault(), L.t("%d KB"), Math.max(1, bytes >> 10));
    }

    /**
     * Move o conteúdo de {@code src} para dentro de {@code dst}, arquivo a arquivo:
     * cada arquivo é copiado e só então apagado na origem, então o espaço extra
     * necessário é o do maior arquivo, e uma interrupção não perde nada (basta
     * mover de novo; o que já chegou é sobrescrito).
     */
    public static void moveTree(File src, File dst, Progress progress) throws IOException {
        List<File> files = new ArrayList<>();
        collectFiles(src, files);
        long total = 0;
        for (File f : files) total += f.length();

        String root = src.getCanonicalPath();
        long done = 0;
        byte[] buf = new byte[1 << 20];
        for (File f : files) {
            String rel = f.getCanonicalPath().substring(root.length() + 1);
            File out = new File(dst, rel);
            File parent = out.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                throw new IOException(L.t("não foi possível criar ") + parent);
            }
            long len = f.length();
            // renameTo só funciona no mesmo sistema de arquivos; /sdcard e a pasta do
            // app normalmente não são, e aí copia.
            if (!f.renameTo(out)) {
                copyFile(f, out, buf);
                if (!f.delete()) throw new IOException(L.t("não foi possível apagar ") + f);
            }
            done += len;
            if (progress != null) progress.onProgress(rel, done, total);
        }
        deleteEmptyDirs(src);
    }

    private static void collectFiles(File dir, List<File> out) {
        File[] children = dir.listFiles();
        if (children == null) return;
        for (File c : children) {
            if (c.isDirectory()) collectFiles(c, out);
            else out.add(c);
        }
    }

    private static void deleteEmptyDirs(File dir) {
        File[] children = dir.listFiles();
        if (children != null) {
            for (File c : children) {
                if (c.isDirectory()) deleteEmptyDirs(c);
            }
        }
        dir.delete();   // falha (e fica) se ainda tiver algo dentro
    }

    private static void copyFile(File from, File to, byte[] buf) throws IOException {
        File tmp = new File(to.getPath() + ".part");
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(tmp)) {
            int r;
            while ((r = in.read(buf)) != -1) out.write(buf, 0, r);
        }
        if (to.exists() && !to.delete()) throw new IOException(L.t("não foi possível substituir ") + to);
        if (!tmp.renameTo(to)) throw new IOException(L.t("não foi possível gravar ") + to);
        to.setLastModified(from.lastModified());
    }
}
