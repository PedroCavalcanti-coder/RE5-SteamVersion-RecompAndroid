package com.re5.recompiled;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.ArrayAdapter;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import android.content.SharedPreferences;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.Spinner;
import java.io.BufferedReader;
import java.io.FileReader;
import java.io.FileWriter;

import androidx.documentfile.provider.DocumentFile;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * Tela inicial (vertical): estado da instalação, importação/movimentação dos
 * arquivos do jogo e o botão de jogar. O jogo em si roda no GameActivity, deitado.
 */
public final class LauncherActivity extends Activity {
    private static final int REQUEST_ARCHIVE_PICK = 1001;
    private static final int REQUEST_STORAGE_PERMISSION = 1002;

    private static final int BG = 0xFF0A0B0D;
    private static final int SURFACE = 0xFF15171B;
    private static final int STROKE = 0xFF25282E;
    private static final int TEXT = 0xFFF1F1F1;
    private static final int TEXT_DIM = 0xFF9BA1A8;
    private static final int RED = 0xFFC8102E;
    private static final int RED_DARK = 0xFF7A0A1C;
    private static final int AMBER = 0xFFE8A33D;
    private static final int GREEN = 0xFF46B37F;

    private final Handler main = new Handler(Looper.getMainLooper());

    private TextView statusChip;
    private TextView rowState, rowSize, rowWhere, rowDevice;
    private View migrateCard, installCard, permCard, rarCleanCard, progressCard, filesActions;
    private TextView migrateText, installRarButton, rarCleanText, progressTitle, progressDetail;
    private ProgressBar progressBar;
    private TextView playButton;
    private GradientDrawable playBackground;

    private boolean busy;
    private long gameSize = -1;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(Color.TRANSPARENT);
        getWindow().setNavigationBarColor(BG);
        setContentView(buildScreen());
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
        String launchedAt = GameSession.consumeLaunch(this);
        if (launchedAt != null) reportLastRun(launchedAt);
    }

    // ------------------------------------------------------------------ estado

    private boolean hasStoragePermission() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void refresh() {
        File dir = GameStorage.gameDir(this);
        boolean installed = GameStorage.isInstalled(dir);
        boolean perm = hasStoragePermission();
        File legacy = GameStorage.legacyDir();
        boolean legacyData = perm && GameStorage.hasData(legacy);
        File rar = GameStorage.downloadedRar();
        boolean rarExists = perm && rar.isFile();

        rowState.setText(installed ? "Instalados" : GameStorage.hasData(dir) ? "Falta o re5dx9.exe" : "Não instalados");
        rowState.setTextColor(installed ? GREEN : AMBER);
        rowWhere.setText("Pasta do app — apagada junto ao desinstalar");
        rowDevice.setText(Build.MANUFACTURER.substring(0, 1).toUpperCase(Locale.ROOT) + Build.MANUFACTURER.substring(1)
                + " " + Build.MODEL + " · Android " + Build.VERSION.RELEASE + " · " + vulkanVersion());
        filesActions.setVisibility(GameStorage.hasData(dir) ? View.VISIBLE : View.GONE);
        updateSize(dir);

        migrateCard.setVisibility(legacyData && !busy ? View.VISIBLE : View.GONE);
        if (legacyData) {
            migrateText.setText("Os arquivos estão em " + legacy.getAbsolutePath()
                    + ", fora da pasta do app: o Android não os conta no tamanho do app nem os apaga ao "
                    + "desinstalar. Mova-os para a pasta do app (não precisa de espaço extra).");
        }

        installCard.setVisibility(!installed && !legacyData && !busy ? View.VISIBLE : View.GONE);
        installRarButton.setVisibility(rarExists ? View.VISIBLE : View.GONE);
        if (rarExists) installRarButton.setText("Extrair Download/game.rar (" + GameStorage.formatSize(rar.length()) + ")");

        permCard.setVisibility(!perm && !installed && !busy ? View.VISIBLE : View.GONE);

        rarCleanCard.setVisibility(installed && rarExists && !busy ? View.VISIBLE : View.GONE);
        if (rarExists) rarCleanText.setText("Download/game.rar ainda ocupa " + GameStorage.formatSize(rar.length())
                + ". O jogo já está instalado; pode apagar.");

        boolean ready = installed && !busy;
        playButton.setEnabled(ready);
        playButton.setBackground(ready ? playBackground : flat(SURFACE_2, 14));
        playButton.setTextColor(ready ? Color.WHITE : 0xFF5C6168);
        if (busy) setChip("TRABALHANDO…", AMBER);
        else if (installed) setChip("PRONTO", GREEN);
        else setChip("FALTAM ARQUIVOS", AMBER);
    }

    private void updateSize(File dir) {
        rowSize.setText(gameSize < 0 ? "calculando…" : GameStorage.formatSize(gameSize));
        new Thread(() -> {
            long s = GameStorage.sizeOf(dir);
            main.post(() -> {
                gameSize = s;
                rowSize.setText(s == 0 ? "—" : GameStorage.formatSize(s));
            });
        }, "re5-size").start();
    }

    private String vulkanVersion() {
        for (FeatureInfo fi : getPackageManager().getSystemAvailableFeatures()) {
            if (PackageManager.FEATURE_VULKAN_HARDWARE_VERSION.equals(fi.name)) {
                return "Vulkan " + (fi.version >>> 22) + "." + ((fi.version >> 12) & 0x3FF);
            }
        }
        return "sem Vulkan";
    }

    private void setChip(String text, int color) {
        statusChip.setText("●  " + text);
        statusChip.setTextColor(color);
        statusChip.setBackground(pill((color & 0x00FFFFFF) | 0x26000000, (color & 0x00FFFFFF) | 0x80000000));
    }

    private void setBusy(boolean b, String title) {
        busy = b;
        if (b) {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            progressTitle.setText(title);
            progressDetail.setText("");
            progressBar.setIndeterminate(true);
        } else {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
        progressCard.setVisibility(b ? View.VISIBLE : View.GONE);
        refresh();
    }

    // ------------------------------------------------------------------ ações

    private void launchGame() {
        File dir = GameStorage.gameDir(this);
        GameSession.markStarted(this);
        Intent intent = new Intent(this, GameActivity.class);
        intent.putExtra("GAME_DIR", dir.getAbsolutePath());
        startActivity(intent);
    }

    private void reportLastRun(String since) {
        new Thread(() -> {
            String log = GameSession.readRecentLog(since, 200);
            String error = GameSession.findError(log);
            if (error == null) return;
            main.post(() -> new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle("O jogo fechou com erro")
                    .setMessage(error + "\n\nO registro completo pode ser copiado para enviar a quem desenvolve.")
                    .setPositiveButton("Copiar registro", (d, w) -> copyToClipboard(log))
                    .setNegativeButton("Fechar", null)
                    .show());
        }, "re5-crashlog").start();
    }

    private void copyToClipboard(String text) {
        ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        if (cm != null) {
            cm.setPrimaryClip(ClipData.newPlainText("RE5", text));
            Toast.makeText(this, "Registro copiado.", Toast.LENGTH_SHORT).show();
        }
    }

    private void requestStorageAccess() {
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (ActivityNotFoundException e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[]{
                    Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE
            }, REQUEST_STORAGE_PERMISSION);
        }
    }

    private void migrateLegacy() {
        File from = GameStorage.legacyDir();
        File to = GameStorage.gameDir(this);
        setBusy(true, "Movendo arquivos para a pasta do app…");
        new Thread(() -> {
            String err = null;
            try {
                GameStorage.moveTree(from, to, (file, done, total) -> main.post(() -> showProgress(file, done, total)));
            } catch (Exception e) {
                err = e.getMessage();
            }
            String finalErr = err;
            main.post(() -> {
                setBusy(false, null);
                if (finalErr != null) {
                    alert("Não foi possível mover tudo", finalErr
                            + "\n\nO que já foi movido está na pasta do app; toque em mover de novo para continuar.");
                } else {
                    Toast.makeText(this, "Arquivos movidos para a pasta do app.", Toast.LENGTH_LONG).show();
                }
            });
        }, "re5-move").start();
    }

    private void showProgress(String file, long done, long total) {
        progressBar.setIndeterminate(false);
        progressBar.setMax(1000);
        progressBar.setProgress(total > 0 ? (int) (done * 1000 / total) : 0);
        progressDetail.setText(GameStorage.formatSize(done) + " de " + GameStorage.formatSize(total) + "\n" + file);
    }

    private void openArchivePicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQUEST_ARCHIVE_PICK);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_ARCHIVE_PICK && resultCode == RESULT_OK && data != null && data.getData() != null) {
            startExtraction(data.getData(), null);
        }
    }

    private void startExtraction(Uri uri, String directPath) {
        File target = GameStorage.gameDir(this);
        setBusy(true, "Extraindo arquivos do jogo…");
        new ArchiveExtractor().extractArchive(this, uri, directPath, target, new ArchiveExtractor.Callback() {
            @Override
            public void onProgress(String currentFile, long processedCount, int percent) {
                main.post(() -> progressDetail.setText(processedCount + " arquivos\n" + currentFile));
            }

            @Override
            public void onSuccess(int totalFiles, long totalBytes) {
                main.post(() -> {
                    gameSize = -1;
                    setBusy(false, null);
                    if (GameStorage.isInstalled(target)) {
                        Toast.makeText(LauncherActivity.this, "Jogo instalado.", Toast.LENGTH_LONG).show();
                        if (uri != null) offerDeleteArchive(uri);
                    } else {
                        alert("Arquivo incompleto", "A extração terminou, mas não encontrei "
                                + GameStorage.DATA_DIR_NAME + "/ e " + GameStorage.EXE_NAME
                                + " na pasta do jogo. O arquivo tem a pasta do jogo completa?");
                    }
                });
            }

            @Override
            public void onError(String error) {
                main.post(() -> {
                    setBusy(false, null);
                    alert("Erro na extração", error);
                });
            }
        });
    }

    private void offerDeleteArchive(Uri uri) {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Apagar o arquivo compactado?")
                .setMessage("O jogo já foi extraído para a pasta do app. O arquivo original não é mais necessário.")
                .setPositiveButton("Apagar", (d, w) -> {
                    boolean ok = false;
                    try {
                        if (DocumentsContract.isDocumentUri(this, uri)) {
                            ok = DocumentsContract.deleteDocument(getContentResolver(), uri);
                        } else {
                            DocumentFile df = DocumentFile.fromSingleUri(this, uri);
                            ok = df != null && df.delete();
                        }
                    } catch (Exception ignored) {
                    }
                    Toast.makeText(this, ok ? "Arquivo apagado." : "Não foi possível apagar.", Toast.LENGTH_SHORT).show();
                    refresh();
                })
                .setNegativeButton("Manter", null)
                .show();
    }

    private void confirmDeleteRar() {
        File rar = GameStorage.downloadedRar();
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Apagar Download/game.rar?")
                .setMessage("Libera " + GameStorage.formatSize(rar.length()) + ". O jogo instalado não é afetado.")
                .setPositiveButton("Apagar", (d, w) -> {
                    Toast.makeText(this, rar.delete() ? "game.rar apagado." : "Não foi possível apagar.",
                            Toast.LENGTH_SHORT).show();
                    refresh();
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private void confirmDeleteGame() {
        File dir = GameStorage.gameDir(this);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Apagar os arquivos do jogo?")
                .setMessage("Remove " + (gameSize > 0 ? GameStorage.formatSize(gameSize) + " " : "")
                        + "da pasta do app, inclusive saves. Para jogar de novo será preciso importar o jogo outra vez.")
                .setPositiveButton("Apagar tudo", (d, w) -> {
                    setBusy(true, "Apagando arquivos do jogo…");
                    new Thread(() -> {
                        GameStorage.deleteTree(dir);
                        main.post(() -> {
                            gameSize = -1;
                            setBusy(false, null);
                        });
                    }, "re5-delete").start();
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    /** Abre a pasta do jogo no app Arquivos (via GameFilesProvider). */
    private void openInFilesApp() {
        Uri root = DocumentsContract.buildRootUri(GameFilesProvider.AUTHORITY, GameFilesProvider.ROOT_ID);
        Uri dirDoc = DocumentsContract.buildDocumentUri(GameFilesProvider.AUTHORITY, GameFilesProvider.ROOT_DOC_ID);
        Intent[] attempts = {
                new Intent(Intent.ACTION_VIEW).setDataAndType(root, DocumentsContract.Root.MIME_TYPE_ITEM),
                new Intent(Intent.ACTION_VIEW).setDataAndType(dirDoc, DocumentsContract.Document.MIME_TYPE_DIR),
                new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).putExtra(DocumentsContract.EXTRA_INITIAL_URI, dirDoc),
        };
        for (Intent i : attempts) {
            try {
                i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                startActivity(i);
                return;
            } catch (ActivityNotFoundException | SecurityException ignored) {
            }
        }
        Toast.makeText(this, "Nenhum app de arquivos disponível.", Toast.LENGTH_SHORT).show();
    }

    /** Navegador simples dentro do próprio app. */
    private void browse(File dir) {
        File base = GameStorage.gameDir(this);
        File[] children = dir.listFiles();
        List<File> entries = new ArrayList<>(children != null ? Arrays.asList(children) : new ArrayList<>());
        entries.sort((a, b) -> a.isDirectory() != b.isDirectory()
                ? (a.isDirectory() ? -1 : 1) : a.getName().compareToIgnoreCase(b.getName()));
        List<String> labels = new ArrayList<>();
        boolean atRoot = dir.equals(base);
        if (!atRoot) labels.add("⬑  ..");
        for (File f : entries) {
            labels.add(f.isDirectory() ? "▸  " + f.getName() + "/" : "    " + f.getName() + "   " + GameStorage.formatSize(f.length()));
        }
        String rel = atRoot ? "" : dir.getAbsolutePath().substring(base.getAbsolutePath().length());
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("game" + rel)
                .setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, labels), (d, which) -> {
                    if (!atRoot && which == 0) {
                        browse(dir.getParentFile());
                        return;
                    }
                    File f = entries.get(atRoot ? which : which - 1);
                    if (f.isDirectory()) browse(f);
                    else browse(dir);
                })
                .setNeutralButton("App Arquivos", (d, w) -> openInFilesApp())
                .setNegativeButton("Fechar", null)
                .show();
    }

    private void alert(String title, String msg) {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title).setMessage(msg).setPositiveButton("OK", null).show();
    }

    // ------------------------------------------------------------------ layout

    private View buildScreen() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(BG);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(col);

        // Capa
        FrameLayout hero = new FrameLayout(this);
        ImageView cover = new ImageView(this);
        cover.setImageResource(R.drawable.re5_hero);
        cover.setScaleType(ImageView.ScaleType.CENTER_CROP);
        hero.addView(cover, match());
        View shade = new View(this);
        shade.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                new int[]{0x66000000, 0x00000000, 0x990A0B0D, BG}));
        hero.addView(shade, match());

        LinearLayout heroText = new LinearLayout(this);
        heroText.setOrientation(LinearLayout.VERTICAL);
        heroText.setGravity(Gravity.BOTTOM);
        heroText.setPadding(dp(20), 0, dp(20), dp(6));
        TextView kicker = text("RECOMPILAÇÃO ESTÁTICA · ARM64", 11, RED, true);
        kicker.setLetterSpacing(0.12f);
        heroText.addView(kicker);
        TextView title = text("RESIDENT EVIL 5", 34, TEXT, true);
        title.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        title.setLetterSpacing(0.02f);
        heroText.addView(title);
        statusChip = text("", 11, TEXT, true);
        statusChip.setPadding(dp(12), dp(5), dp(12), dp(5));
        LinearLayout.LayoutParams chipLp = wrap();
        chipLp.topMargin = dp(8);
        heroText.addView(statusChip, chipLp);
        TextView btnSettings = new TextView(this);
        btnSettings.setText("⚙");
        btnSettings.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        btnSettings.setGravity(Gravity.CENTER);
        btnSettings.setTextColor(TEXT);
        GradientDrawable setBg = new GradientDrawable();
        setBg.setShape(GradientDrawable.OVAL);
        setBg.setColor(0xAA15171B);
        setBg.setStroke(dp(1), 0x66FFFFFF);
        btnSettings.setBackground(setBg);
        FrameLayout.LayoutParams setLp = new FrameLayout.LayoutParams(dp(44), dp(44), Gravity.TOP | Gravity.END);
        setLp.setMargins(0, dp(38), dp(18), 0);
        btnSettings.setOnClickListener(v -> showSettingsDialog());
        hero.addView(btnSettings, setLp);
        hero.addView(heroText, match());
        col.addView(hero, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(340)));

        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(16), dp(10), dp(16), dp(16));
        col.addView(body);

        // Progresso (mover/extrair/apagar)
        LinearLayout prog = card(body);
        progressCard = prog;
        progressTitle = text("", 14, TEXT, true);
        prog.addView(progressTitle);
        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setProgressTintList(android.content.res.ColorStateList.valueOf(RED));
        LinearLayout.LayoutParams pbLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(8));
        pbLp.setMargins(0, dp(10), 0, dp(8));
        prog.addView(progressBar, pbLp);
        progressDetail = text("", 12, TEXT_DIM, false);
        progressDetail.setMaxLines(2);
        prog.addView(progressDetail);
        prog.setVisibility(View.GONE);

        // Mover de /sdcard/re5
        LinearLayout mig = card(body);
        migrateCard = mig;
        mig.addView(cardTitle("Mover arquivos para a pasta do app", AMBER));
        migrateText = text("", 13, TEXT_DIM, false);
        mig.addView(migrateText);
        mig.addView(button("Mover agora", RED, TEXT, v -> migrateLegacy()), buttonLp());

        // Permissão
        LinearLayout perm = card(body);
        permCard = perm;
        perm.addView(cardTitle("Acesso aos arquivos", AMBER));
        perm.addView(text("Permita o acesso para o app achar o game.rar em Download ou uma instalação antiga em /sdcard/re5. "
                + "Não é preciso para importar escolhendo o arquivo.", 13, TEXT_DIM, false));
        perm.addView(button("Permitir acesso", SURFACE_2, TEXT, v -> requestStorageAccess()), buttonLp());

        // Instalar
        LinearLayout inst = card(body);
        installCard = inst;
        inst.addView(cardTitle("Instalar o jogo", TEXT));
        inst.addView(text("Escolha o game.rar (ou .zip) com a pasta do jogo: re5dx9.exe e nativePC_MT/.", 13, TEXT_DIM, false));
        installRarButton = button("", RED, TEXT, v -> startExtraction(null, GameStorage.downloadedRar().getAbsolutePath()));
        inst.addView(installRarButton, buttonLp());
        inst.addView(button("Escolher arquivo…", SURFACE_2, TEXT, v -> openArchivePicker()), buttonLp());

        // Configurações (Multiplayer, Controles, HUD)
        LinearLayout cfg = card(body);
        cfg.addView(cardTitle("Configurações do Jogo", TEXT));
        cfg.addView(text("Multiplayer Cooperativo (LAN), controles na tela e monitor de telemetria.", 13, TEXT_DIM, false));
        cfg.addView(button("⚙ Abrir Configurações", SURFACE_2, TEXT, v -> showSettingsDialog()), buttonLp());

        // Arquivos do jogo
        LinearLayout info = card(body);
        info.addView(cardTitle("Arquivos do jogo", TEXT));
        rowState = infoRow(info, "Situação");
        rowSize = infoRow(info, "Tamanho");
        rowWhere = infoRow(info, "Local");
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.addView(button("Ver arquivos", SURFACE_2, TEXT, v -> browse(GameStorage.gameDir(this))), weightLp());
        actions.addView(button("Apagar", SURFACE_2, 0xFFFF8A8A, v -> confirmDeleteGame()), weightLp());
        filesActions = actions;
        LinearLayout.LayoutParams aLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(46));
        aLp.topMargin = dp(12);
        info.addView(actions, aLp);

        // game.rar sobrando
        LinearLayout clean = card(body);
        rarCleanCard = clean;
        clean.addView(cardTitle("Liberar espaço", TEXT));
        rarCleanText = text("", 13, TEXT_DIM, false);
        clean.addView(rarCleanText);
        clean.addView(button("Apagar game.rar", SURFACE_2, 0xFFFF8A8A, v -> confirmDeleteRar()), buttonLp());

        // Aparelho
        LinearLayout dev = card(body);
        dev.addView(cardTitle("Aparelho", TEXT));
        rowDevice = text("", 13, TEXT_DIM, false);
        dev.addView(rowDevice);

        root.addView(scroll, match());

        // Botão de jogar fixo embaixo
        LinearLayout bottom = new LinearLayout(this);
        bottom.setOrientation(LinearLayout.VERTICAL);
        bottom.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[]{0x000A0B0D, BG, BG}));
        bottom.setPadding(dp(16), dp(22), dp(16), dp(16));
        playButton = button("JOGAR", RED, Color.WHITE, v -> launchGame());
        playButton.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        playButton.setLetterSpacing(0.2f);
        playBackground = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[]{RED, RED_DARK});
        playBackground.setCornerRadius(dp(14));
        bottom.addView(playButton, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(58)));
        FrameLayout.LayoutParams bLp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM);
        root.addView(bottom, bLp);

        // Borda a borda: a capa passa por baixo da barra de status; o resto respeita as barras.
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            int bottomInset = insets.getInsets(WindowInsets.Type.systemBars()).bottom;
            bottom.setPadding(dp(16), dp(22), dp(16), dp(16) + bottomInset);
            body.setPadding(dp(16), dp(10), dp(16), dp(110) + bottomInset);
            return insets;
        });
        return root;
    }

    private static final int SURFACE_2 = 0xFF22252B;

    private LinearLayout card(LinearLayout parent) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(16), dp(14), dp(16), dp(16));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(SURFACE);
        bg.setCornerRadius(dp(16));
        bg.setStroke(dp(1), STROKE);
        c.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(12);
        parent.addView(c, lp);
        return c;
    }

    private TextView cardTitle(String s, int color) {
        TextView t = text(s, 15, color, true);
        t.setPadding(0, 0, 0, dp(6));
        return t;
    }

    private TextView infoRow(LinearLayout parent, String label) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(5), 0, dp(5));
        TextView l = text(label, 13, TEXT_DIM, false);
        row.addView(l, new LinearLayout.LayoutParams(dp(92), ViewGroup.LayoutParams.WRAP_CONTENT));
        TextView v = text("", 13, TEXT, false);
        row.addView(v, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        parent.addView(row);
        return v;
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setTypeface(Typeface.create(bold ? "sans-serif-medium" : "sans-serif", bold ? Typeface.BOLD : Typeface.NORMAL));
        t.setLineSpacing(0, 1.15f);
        return t;
    }

    private TextView button(String s, int bg, int fg, View.OnClickListener l) {
        TextView b = text(s, 14, fg, true);
        b.setGravity(Gravity.CENTER);
        b.setAllCaps(false);
        GradientDrawable d = new GradientDrawable();
        d.setColor(bg);
        d.setCornerRadius(dp(12));
        b.setBackground(d);
        b.setClickable(true);
        b.setFocusable(true);
        b.setForeground(getDrawable(android.R.drawable.list_selector_background));
        b.setOnClickListener(l);
        return b;
    }

    private LinearLayout.LayoutParams buttonLp() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(46));
        lp.topMargin = dp(12);
        return lp;
    }

    private LinearLayout.LayoutParams weightLp() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
        lp.setMargins(dp(4), 0, dp(4), 0);
        return lp;
    }

    private GradientDrawable flat(int color, int radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radiusDp));
        return d;
    }

    private GradientDrawable pill(int bg, int stroke) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(bg);
        d.setCornerRadius(dp(20));
        d.setStroke(dp(1), stroke);
        return d;
    }

    private static FrameLayout.LayoutParams match() {
        return new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
    }

    private static LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private static class LanConfig {
        String name = "Jogador";
        String country = "BR";
        String hosts = "";
        long id = 0;
    }

    private File lanConfigFile() {
        return new File(GameStorage.gameDir(this), "user/drive_c/Users/Player/re5_lan.ini");
    }

    private LanConfig loadLanConfig() {
        LanConfig cfg = new LanConfig();
        File f = lanConfigFile();
        if (f.isFile()) {
            try (BufferedReader r = new BufferedReader(new FileReader(f))) {
                String line;
                while ((line = r.readLine()) != null) {
                    line = line.trim();
                    if (line.startsWith("#") || line.startsWith(";") || !line.contains("=")) continue;
                    int eq = line.indexOf('=');
                    String key = line.substring(0, eq).trim();
                    String val = line.substring(eq + 1).trim();
                    if (key.equalsIgnoreCase("nome") || key.equalsIgnoreCase("name")) cfg.name = val;
                    else if (key.equalsIgnoreCase("pais") || key.equalsIgnoreCase("country")) cfg.country = val;
                    else if (key.equalsIgnoreCase("hosts")) cfg.hosts = val;
                    else if (key.equalsIgnoreCase("id")) {
                        try { cfg.id = Long.parseLong(val); } catch (NumberFormatException ignored) {}
                    }
                }
            } catch (Exception ignored) {}
        }
        return cfg;
    }

    private void saveLanConfig(String name, String country, String hosts, long existingId) {
        File f = lanConfigFile();
        File parent = f.getParentFile();
        if (parent != null && !parent.exists()) parent.mkdirs();

        long id = existingId > 0 ? existingId : (100000 + (long)(Math.random() * 899999));
        name = name != null && !name.trim().isEmpty() ? name.trim() : "Jogador";
        country = country != null && country.trim().length() >= 2 ? country.trim().substring(0, 2).toUpperCase(Locale.US) : "BR";
        hosts = hosts != null ? hosts.trim() : "";

        try (FileWriter w = new FileWriter(f)) {
            w.write("# Identidade do jogador no multiplayer LAN do RE5\n");
            w.write("nome=" + name + "\n");
            w.write("pais=" + country + "\n");
            w.write("hosts=" + hosts + "\n");
            w.write("id=" + id + "\n");
        } catch (Exception e) {
            Toast.makeText(this, "Erro ao salvar re5_lan.ini: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    private void showSettingsDialog() {
        LanConfig lan = loadLanConfig();
        SharedPreferences prefs = getSharedPreferences("re5_settings", MODE_PRIVATE);
        boolean touchEnabled = prefs.getBoolean("touch_enabled", true);
        float touchOpacity = prefs.getFloat("touch_opacity", 0.85f);
        float touchScale = prefs.getFloat("touch_scale", 1.0f);
        boolean hudEnabled = prefs.getBoolean("hud_enabled", true);

        ScrollView scroll = new ScrollView(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(20), dp(16), dp(20), dp(16));
        scroll.addView(col);

        // --- 1. MULTIPLAYER LAN ---
        col.addView(cardTitle("🎮 Multiplayer Cooperativo (LAN)", RED));
        col.addView(text("Configure sua identidade para jogar em modo cooperativo na mesma rede Wi-Fi / Hotspot.", 12, TEXT_DIM, false));

        col.addView(dialogFieldLabel("Nome do Jogador"));
        EditText editName = dialogEditText(lan.name);
        col.addView(editName);

        col.addView(dialogFieldLabel("País / Bandeira (ex: BR, US, PT)"));
        EditText editCountry = dialogEditText(lan.country);
        col.addView(editCountry);

        col.addView(dialogFieldLabel("IP do Host Parceiro (deixe vazio se você for criar a sala)"));
        EditText editHosts = dialogEditText(lan.hosts);
        editHosts.setHint("Ex: 192.168.1.100");
        col.addView(editHosts);

        // Divisor
        col.addView(dialogDivider());

        // --- 2. CONTROLES NA TELA ---
        col.addView(cardTitle("🕹 Controles Virtuais na Tela", RED));
        CheckBox checkTouch = new CheckBox(this);
        checkTouch.setText("Exibir botões táteis virtuais");
        checkTouch.setTextColor(TEXT);
        checkTouch.setChecked(touchEnabled);
        col.addView(checkTouch);

        col.addView(dialogFieldLabel("Opacidade dos Botões"));
        Spinner spinOpacity = new Spinner(this);
        String[] opacities = {"50% (Sutil)", "75% (Médio)", "85% (Padrão)", "100% (Sólido)"};
        float[] opVals = {0.50f, 0.75f, 0.85f, 1.0f};
        ArrayAdapter<String> opAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, opacities);
        spinOpacity.setAdapter(opAdapter);
        int opIdx = 2;
        if (Math.abs(touchOpacity - 0.5f) < 0.1f) opIdx = 0;
        else if (Math.abs(touchOpacity - 0.75f) < 0.05f) opIdx = 1;
        else if (Math.abs(touchOpacity - 1.0f) < 0.05f) opIdx = 3;
        spinOpacity.setSelection(opIdx);
        col.addView(spinOpacity);

        col.addView(dialogFieldLabel("Tamanho dos Botões"));
        Spinner spinScale = new Spinner(this);
        String[] scales = {"Pequeno (85%)", "Normal (100%)", "Grande (115%)"};
        float[] scVals = {0.85f, 1.0f, 1.15f};
        ArrayAdapter<String> scAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, scales);
        spinScale.setAdapter(scAdapter);
        int scIdx = 1;
        if (touchScale < 0.95f) scIdx = 0;
        else if (touchScale > 1.05f) scIdx = 2;
        spinScale.setSelection(scIdx);
        col.addView(spinScale);

        // Divisor
        col.addView(dialogDivider());

        // --- 3. TELEMETRIA / HUD ---
        col.addView(cardTitle("📊 Monitor de Desempenho (HUD)", RED));
        CheckBox checkHud = new CheckBox(this);
        checkHud.setText("Exibir FPS, RAM, CPU e GPU na tela");
        checkHud.setTextColor(TEXT);
        checkHud.setChecked(hudEnabled);
        col.addView(checkHud);
        col.addView(text("Mostra contador de FPS em tempo real e consumo de recursos durante a partida.", 12, TEXT_DIM, false));

        // Divisor
        col.addView(dialogDivider());

        // --- 4. AVANÇADO: variáveis RE5_* (re5_env.txt, lido pelo runtime) ---
        col.addView(cardTitle("🔧 Avançado (testes de desempenho)", RED));
        col.addView(text("Uma variável por linha, ex.: RE5_FPS_LOG=1 ou RE5_CPUS=4. Vai para re5_env.txt na pasta do jogo "
                + "(PLANO_MESTRE_OTIMIZACAO.md). Deixe vazio no uso normal.", 12, TEXT_DIM, false));
        File envFile = new File(GameStorage.gameDir(this), "re5_env.txt");
        EditText editEnv = dialogEditText(readTextFile(envFile));
        editEnv.setHint("RE5_FPS_LOG=1");
        editEnv.setSingleLine(false);
        editEnv.setMinLines(3);
        col.addView(editEnv);

        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Configurações")
                .setView(scroll)
                .setPositiveButton("Salvar", (d, w) -> {
                    String newName = editName.getText().toString();
                    String newCountry = editCountry.getText().toString();
                    String newHosts = editHosts.getText().toString();
                    saveLanConfig(newName, newCountry, newHosts, lan.id);

                    boolean newTouch = checkTouch.isChecked();
                    float newOp = opVals[spinOpacity.getSelectedItemPosition()];
                    float newSc = scVals[spinScale.getSelectedItemPosition()];
                    boolean newHud = checkHud.isChecked();

                    writeEnvFile(envFile, editEnv.getText().toString());

                    prefs.edit()
                            .putBoolean("touch_enabled", newTouch)
                            .putFloat("touch_opacity", newOp)
                            .putFloat("touch_scale", newSc)
                            .putBoolean("hud_enabled", newHud)
                            .apply();

                    Toast.makeText(this, "Configurações salvas com sucesso!", Toast.LENGTH_SHORT).show();
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private static String readTextFile(File f) {
        try (java.io.BufferedReader br = new java.io.BufferedReader(new java.io.FileReader(f))) {
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = br.readLine()) != null) sb.append(line).append('
');
            return sb.toString().trim();
        } catch (java.io.IOException e) {
            return "";
        }
    }

    private void writeEnvFile(File f, String text) {
        String t = text.trim();
        try {
            if (t.isEmpty()) {
                //noinspection ResultOfMethodCallIgnored
                f.delete();
                return;
            }
            try (java.io.FileWriter w = new java.io.FileWriter(f)) {
                w.write(t + "
");
            }
        } catch (java.io.IOException e) {
            Toast.makeText(this, "Não consegui gravar re5_env.txt: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private TextView dialogFieldLabel(String label) {
        TextView t = text(label, 12, TEXT_DIM, false);
        t.setPadding(0, dp(10), 0, dp(4));
        return t;
    }

    private EditText dialogEditText(String value) {
        EditText e = new EditText(this);
        e.setText(value != null ? value : "");
        e.setTextColor(TEXT);
        e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        e.setPadding(dp(12), dp(10), dp(12), dp(10));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(SURFACE_2);
        bg.setCornerRadius(dp(8));
        bg.setStroke(dp(1), STROKE);
        e.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(4);
        e.setLayoutParams(lp);
        return e;
    }

    private View dialogDivider() {
        View v = new View(this);
        v.setBackgroundColor(STROKE);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1));
        lp.setMargins(0, dp(16), 0, dp(14));
        v.setLayoutParams(lp);
        return v;
    }
}
