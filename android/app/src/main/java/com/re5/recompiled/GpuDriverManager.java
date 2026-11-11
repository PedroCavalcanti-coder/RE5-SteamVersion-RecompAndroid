package com.re5.recompiled;

import android.content.Context;
import android.net.Uri;
import android.os.Build;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Gerenciador de drivers Vulkan customizados (Turnip / Qualcomm Adreno) em
 * {@code files/gpu_drivers/<id>/} no armazenamento interno privado do app,
 * exigido pelo SELinux do Android para {@code dlopen}/AdrenoTools.
 */
public final class GpuDriverManager {

    private static final String DIR_NAME = "gpu_drivers";
    private static final long MAX_UNCOMPRESSED_BYTES = 128L * 1024L * 1024L;
    private static final int MAX_ENTRIES = 1024;
    private static final int MAX_META_BYTES = 1024 * 1024;

    private GpuDriverManager() {}

    public static final class DriverInfo {
        public final String id;
        public final String name;
        public final String driverVersion;
        public final String author;
        public final String description;
        public final String libraryName;
        public final int minApi;
        public final File dir;

        DriverInfo(String id, String name, String driverVersion, String author,
                   String description, String libraryName, int minApi, File dir) {
            this.id = id;
            this.name = name;
            this.driverVersion = driverVersion;
            this.author = author;
            this.description = description;
            this.libraryName = libraryName;
            this.minApi = minApi;
            this.dir = dir;
        }

        public String displayLabel() {
            if (driverVersion == null || driverVersion.isEmpty()) return name;
            return name + " (" + driverVersion + ")";
        }

        public String driverPathWithSlash() {
            String p = dir.getAbsolutePath();
            return p.endsWith("/") ? p : p + "/";
        }
    }

    public static File driversRoot(Context ctx) {
        return new File(ctx.getFilesDir(), DIR_NAME);
    }

    public static List<DriverInfo> list(Context ctx) {
        File root = driversRoot(ctx);
        File[] children = root.listFiles();
        if (children == null) return Collections.emptyList();

        List<DriverInfo> out = new ArrayList<>();
        for (File child : children) {
            if (!child.isDirectory() || child.getName().startsWith(".")) continue;
            DriverInfo info = readDriverDir(child.getName(), child);
            if (info != null) out.add(info);
        }
        Collections.sort(out, new Comparator<DriverInfo>() {
            @Override public int compare(DriverInfo a, DriverInfo b) {
                return a.name.compareToIgnoreCase(b.name);
            }
        });
        return out;
    }

    public static DriverInfo getSelected(Context ctx, String id) {
        if (!isSafeSingleSegment(id)) return null;
        File dir = new File(driversRoot(ctx), id);
        if (!dir.isDirectory()) return null;
        return readDriverDir(id, dir);
    }

    public static boolean delete(Context ctx, String id) {
        if (!isSafeSingleSegment(id)) return false;
        File dir = new File(driversRoot(ctx), id);
        if (!dir.exists()) return false;
        return deleteRecursive(dir);
    }

    public static DriverInfo importZip(Context ctx, Uri uri) throws IOException {
        if (uri == null) throw new IOException(L.t("Arquivo inválido."));
        File root = driversRoot(ctx);
        if (!root.exists() && !root.mkdirs()) {
            throw new IOException(L.t("Não foi possível criar a pasta de drivers."));
        }

        File stage = new File(root, ".importing_" + System.currentTimeMillis());
        if (stage.exists()) deleteRecursive(stage);
        if (!stage.mkdirs()) {
            throw new IOException(L.t("Não foi possível preparar a extração do driver."));
        }

        boolean moved = false;
        try {
            extractZip(ctx, uri, stage);
            File contentRoot = resolveContentRoot(stage);
            DriverInfo parsed = validateMetaAndLib("tmp", contentRoot);

            String id = allocateUniqueId(root, parsed.name.isEmpty() ? parsed.libraryName : parsed.name);
            File dest = new File(root, id);
            if (!contentRoot.renameTo(dest)) {
                copyRecursive(contentRoot, dest);
            } else {
                moved = true;
            }
            if (moved && !contentRoot.equals(stage)) {
                deleteRecursive(stage);
            }
            DriverInfo installed = readDriverDir(id, dest);
            if (installed == null) {
                deleteRecursive(dest);
                throw new IOException(L.t("Falha ao validar o driver após a extração."));
            }
            return installed;
        } finally {
            if (stage.exists()) deleteRecursive(stage);
        }
    }

    private static void extractZip(Context ctx, Uri uri, File stage) throws IOException {
        String stageCanonical = stage.getCanonicalPath();
        String stagePrefix = stageCanonical.endsWith(File.separator)
                ? stageCanonical : stageCanonical + File.separator;

        long totalBytes = 0;
        int entries = 0;
        byte[] buf = new byte[32 * 1024];

        try (InputStream raw = ctx.getContentResolver().openInputStream(uri)) {
            if (raw == null) throw new IOException(L.t("Não foi possível abrir o arquivo selecionado."));
            try (ZipInputStream zis = new ZipInputStream(raw)) {
                ZipEntry entry;
                while ((entry = zis.getNextEntry()) != null) {
                    entries++;
                    if (entries > MAX_ENTRIES) {
                        throw new IOException(L.t("Arquivo ZIP excede o limite de entradas permitidas."));
                    }
                    String name = entry.getName();
                    if (name == null || name.isEmpty()) continue;
                    name = name.replace('\\', '/');
                    if (name.startsWith("__MACOSX/") || name.equals("__MACOSX")) continue;

                    File target = new File(stage, name);
                    String targetCanonical = target.getCanonicalPath();
                    if (!targetCanonical.equals(stageCanonical) && !targetCanonical.startsWith(stagePrefix)) {
                        throw new IOException(L.t("Arquivo ZIP inválido (caminho fora do destino)."));
                    }

                    if (entry.isDirectory()) {
                        if (!target.exists() && !target.mkdirs()) {
                            throw new IOException(L.t("Erro ao criar pasta durante a extração."));
                        }
                        continue;
                    }

                    File parent = target.getParentFile();
                    if (parent != null && !parent.exists() && !parent.mkdirs()) {
                        throw new IOException(L.t("Erro ao criar pasta durante a extração."));
                    }

                    try (FileOutputStream fos = new FileOutputStream(target)) {
                        int n;
                        while ((n = zis.read(buf)) > 0) {
                            totalBytes += n;
                            if (totalBytes > MAX_UNCOMPRESSED_BYTES) {
                                throw new IOException(L.t("Driver ZIP muito grande (limite de 128 MB descompactado)."));
                            }
                            fos.write(buf, 0, n);
                        }
                    }
                }
            }
        }
    }

    private static File resolveContentRoot(File stage) throws IOException {
        File directMeta = new File(stage, "meta.json");
        if (directMeta.isFile()) return stage;

        File[] children = stage.listFiles();
        File singleSubdir = null;
        if (children != null) {
            for (File child : children) {
                if (child.getName().startsWith(".")) continue;
                if (child.isDirectory() && singleSubdir == null) {
                    singleSubdir = child;
                } else {
                    singleSubdir = null;
                    break;
                }
            }
        }
        if (singleSubdir != null && new File(singleSubdir, "meta.json").isFile()) {
            return singleSubdir;
        }
        throw new IOException(L.t("meta.json não encontrado no pacote de driver (.zip)."));
    }

    private static DriverInfo readDriverDir(String id, File dir) {
        try {
            return validateMetaAndLib(id, dir);
        } catch (IOException ignored) {
            return null;
        }
    }

    private static DriverInfo validateMetaAndLib(String id, File dir) throws IOException {
        File metaFile = new File(dir, "meta.json");
        if (!metaFile.isFile()) {
            throw new IOException(L.t("meta.json não encontrado no pacote de driver (.zip)."));
        }
        if (metaFile.length() > MAX_META_BYTES) {
            throw new IOException(L.t("meta.json muito grande."));
        }
        String rawJson = readUtf8(metaFile);
        final JSONObject obj;
        try {
            obj = new JSONObject(rawJson);
        } catch (JSONException e) {
            throw new IOException(L.t("meta.json inválido no pacote de driver."));
        }

        String lib = obj.optString("libraryName", "").trim();
        if (!isSafeLibraryName(lib)) {
            throw new IOException(L.t("meta.json sem libraryName .so válido."));
        }
        int minApi = obj.optInt("minApi", 0);
        if (minApi > Build.VERSION.SDK_INT) {
            throw new IOException(L.t("Este driver exige Android API ") + minApi + "+.");
        }
        File libFile = new File(dir, lib);
        if (!libFile.isFile()) {
            throw new IOException(L.t("Biblioteca do driver não encontrada no ZIP: ") + lib);
        }

        String name = obj.optString("name", "").trim();
        if (name.isEmpty()) name = lib;
        String ver = obj.optString("driverVersion", "").trim();
        String author = obj.optString("author", "").trim();
        String desc = obj.optString("description", "").trim();
        return new DriverInfo(id, name, ver, author, desc, lib, minApi, dir);
    }

    private static boolean isSafeSingleSegment(String s) {
        if (s == null || s.isEmpty()) return false;
        if (s.equals(".") || s.equals("..") || s.startsWith(".")) return false;
        return s.indexOf('/') < 0 && s.indexOf('\\') < 0;
    }

    private static boolean isSafeLibraryName(String s) {
        if (!isSafeSingleSegment(s)) return false;
        return s.toLowerCase(Locale.ROOT).endsWith(".so");
    }

    private static String allocateUniqueId(File root, String hint) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < hint.length(); i++) {
            char c = hint.charAt(i);
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
                sb.append(c);
            } else if (sb.length() > 0 && sb.charAt(sb.length() - 1) != '-') {
                sb.append('-');
            }
        }
        String base = sb.toString().replaceAll("^-+|-+$", "");
        if (base.isEmpty() || base.startsWith(".")) base = "driver";
        if (base.length() > 48) base = base.substring(0, 48);

        String candidate = base;
        int n = 2;
        while (new File(root, candidate).exists()) {
            candidate = base + "-" + n++;
        }
        return candidate;
    }

    private static String readUtf8(File f) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        try (InputStream in = new FileInputStream(f)) {
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        return new String(out.toByteArray(), StandardCharsets.UTF_8);
    }

    private static void copyRecursive(File src, File dst) throws IOException {
        if (src.isDirectory()) {
            if (!dst.exists() && !dst.mkdirs()) {
                throw new IOException(L.t("Erro ao copiar pasta do driver."));
            }
            File[] children = src.listFiles();
            if (children != null) {
                for (File child : children) copyRecursive(child, new File(dst, child.getName()));
            }
            return;
        }
        byte[] buf = new byte[32 * 1024];
        try (InputStream in = new FileInputStream(src);
             FileOutputStream out = new FileOutputStream(dst)) {
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    private static boolean deleteRecursive(File f) {
        if (f.isDirectory()) {
            File[] children = f.listFiles();
            if (children != null) {
                for (File child : children) deleteRecursive(child);
            }
        }
        return f.delete();
    }
}
