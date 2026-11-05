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
    private TextView rowLanProfile, rowLanHosts, rowLanIp;
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
        L.init(this);
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

        rowState.setText(installed ? L.t("Instalados") : GameStorage.hasData(dir) ? L.t("Falta o re5dx9.exe") : L.t("Não instalados"));
        rowState.setTextColor(installed ? GREEN : AMBER);
        rowWhere.setText(L.t("Pasta do app — apagada junto ao desinstalar"));
        rowDevice.setText(Build.MANUFACTURER.substring(0, 1).toUpperCase(Locale.ROOT) + Build.MANUFACTURER.substring(1)
                + " " + Build.MODEL + L.t(" · Android ") + Build.VERSION.RELEASE + " · " + vulkanVersion());
        filesActions.setVisibility(GameStorage.hasData(dir) ? View.VISIBLE : View.GONE);
        updateSize(dir);
        refreshLan();

        migrateCard.setVisibility(legacyData && !busy ? View.VISIBLE : View.GONE);
        if (legacyData) {
            migrateText.setText(L.t("Os arquivos estão em ") + legacy.getAbsolutePath()
                    + L.t(", fora da pasta do app: o Android não os conta no tamanho do app nem os apaga ao ")
                    + L.t("desinstalar. Mova-os para a pasta do app (não precisa de espaço extra)."));
        }

        installCard.setVisibility(!installed && !legacyData && !busy ? View.VISIBLE : View.GONE);
        installRarButton.setVisibility(rarExists ? View.VISIBLE : View.GONE);
        if (rarExists) installRarButton.setText(L.t("Extrair Download/game.rar (") + GameStorage.formatSize(rar.length()) + ")");

        permCard.setVisibility(!perm && !installed && !busy ? View.VISIBLE : View.GONE);

        rarCleanCard.setVisibility(installed && rarExists && !busy ? View.VISIBLE : View.GONE);
        if (rarExists) rarCleanText.setText(L.t("Download/game.rar ainda ocupa ") + GameStorage.formatSize(rar.length())
                + L.t(". O jogo já está instalado; pode apagar."));

        boolean ready = installed && !busy;
        playButton.setEnabled(ready);
        playButton.setBackground(ready ? playBackground : flat(SURFACE_2, 14));
        playButton.setTextColor(ready ? Color.WHITE : 0xFF5C6168);
        if (busy) setChip(L.t("TRABALHANDO…"), AMBER);
        else if (installed) setChip(L.t("PRONTO"), GREEN);
        else setChip(L.t("FALTAM ARQUIVOS"), AMBER);
    }

    private void updateSize(File dir) {
        rowSize.setText(gameSize < 0 ? L.t("calculando…") : GameStorage.formatSize(gameSize));
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
        return L.t("sem Vulkan");
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
        intent.putExtra("LAUNCH_ID", Long.toString(System.currentTimeMillis()) + "-" + System.nanoTime());
        // O jogo abre na mesma tela do launcher (ex.: a tela externa do Z Flip, aberta por um
        // mod/MultiStar): sem isso o Android manda a activity nova para a tela principal.
        android.app.ActivityOptions opts = android.app.ActivityOptions.makeBasic();
        if (getDisplay() != null) opts.setLaunchDisplayId(getDisplay().getDisplayId());
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(intent, opts.toBundle());
    }

    private void reportLastRun(String since) {
        new Thread(() -> {
            String log = GameSession.readRecentLog(since, 200);
            String error = GameSession.findError(log);
            if (error == null) return;
            main.post(() -> new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(L.t("O jogo fechou com erro"))
                    .setMessage(error + L.t("\n\nO registro completo pode ser copiado para enviar a quem desenvolve."))
                    .setPositiveButton(L.t("Copiar registro"), (d, w) -> copyToClipboard(log))
                    .setNegativeButton(L.t("Fechar"), null)
                    .show());
        }, "re5-crashlog").start();
    }

    private void copyToClipboard(String text) {
        ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        if (cm != null) {
            cm.setPrimaryClip(ClipData.newPlainText("RE5", text));
            Toast.makeText(this, L.t("Registro copiado."), Toast.LENGTH_SHORT).show();
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
        setBusy(true, L.t("Movendo arquivos para a pasta do app…"));
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
                    alert(L.t("Não foi possível mover tudo"), finalErr
                            + L.t("\n\nO que já foi movido está na pasta do app; toque em mover de novo para continuar."));
                } else {
                    Toast.makeText(this, L.t("Arquivos movidos para a pasta do app."), Toast.LENGTH_LONG).show();
                }
            });
        }, "re5-move").start();
    }

    private void showProgress(String file, long done, long total) {
        progressBar.setIndeterminate(false);
        progressBar.setMax(1000);
        progressBar.setProgress(total > 0 ? (int) (done * 1000 / total) : 0);
        progressDetail.setText(GameStorage.formatSize(done) + L.t(" de ") + GameStorage.formatSize(total) + "\n" + file);
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
        if (requestCode == REQUEST_SETTINGS && resultCode == RESULT_OK && data != null
                && data.getBooleanExtra(SettingsActivity.RESULT_OPEN_LAN, false)) {
            showLanDialog();
        }
    }

    private void startExtraction(Uri uri, String directPath) {
        File target = GameStorage.gameDir(this);
        setBusy(true, L.t("Extraindo arquivos do jogo…"));
        new ArchiveExtractor().extractArchive(this, uri, directPath, target, new ArchiveExtractor.Callback() {
            @Override
            public void onProgress(String currentFile, long processedCount, int percent) {
                main.post(() -> progressDetail.setText(processedCount + L.t(" arquivos\n") + currentFile));
            }

            @Override
            public void onSuccess(int totalFiles, long totalBytes) {
                main.post(() -> {
                    gameSize = -1;
                    setBusy(false, null);
                    if (GameStorage.isInstalled(target)) {
                        Toast.makeText(LauncherActivity.this, L.t("Jogo instalado."), Toast.LENGTH_LONG).show();
                        if (uri != null) offerDeleteArchive(uri);
                    } else {
                        alert(L.t("Arquivo incompleto"), L.t("A extração terminou, mas não encontrei ")
                                + GameStorage.DATA_DIR_NAME + "/ e " + GameStorage.EXE_NAME
                                + L.t(" na pasta do jogo. O arquivo tem a pasta do jogo completa?"));
                    }
                });
            }

            @Override
            public void onError(String error) {
                main.post(() -> {
                    setBusy(false, null);
                    alert(L.t("Erro na extração"), error);
                });
            }
        });
    }

    private void offerDeleteArchive(Uri uri) {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(L.t("Apagar o arquivo compactado?"))
                .setMessage(L.t("O jogo já foi extraído para a pasta do app. O arquivo original não é mais necessário."))
                .setPositiveButton(L.t("Apagar"), (d, w) -> {
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
                    Toast.makeText(this, ok ? L.t("Arquivo apagado.") : L.t("Não foi possível apagar."), Toast.LENGTH_SHORT).show();
                    refresh();
                })
                .setNegativeButton(L.t("Manter"), null)
                .show();
    }

    private void confirmDeleteRar() {
        File rar = GameStorage.downloadedRar();
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(L.t("Apagar Download/game.rar?"))
                .setMessage(L.t("Libera ") + GameStorage.formatSize(rar.length()) + L.t(". O jogo instalado não é afetado."))
                .setPositiveButton(L.t("Apagar"), (d, w) -> {
                    Toast.makeText(this, rar.delete() ? L.t("game.rar apagado.") : L.t("Não foi possível apagar."),
                            Toast.LENGTH_SHORT).show();
                    refresh();
                })
                .setNegativeButton(L.t("Cancelar"), null)
                .show();
    }

    private void confirmDeleteGame() {
        File dir = GameStorage.gameDir(this);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(L.t("Apagar os arquivos do jogo?"))
                .setMessage(L.t("Remove ") + (gameSize > 0 ? GameStorage.formatSize(gameSize) + " " : "")
                        + L.t("da pasta do app, inclusive saves. Para jogar de novo será preciso importar o jogo outra vez."))
                .setPositiveButton(L.t("Apagar tudo"), (d, w) -> {
                    setBusy(true, L.t("Apagando arquivos do jogo…"));
                    new Thread(() -> {
                        GameStorage.deleteTree(dir);
                        main.post(() -> {
                            gameSize = -1;
                            setBusy(false, null);
                        });
                    }, "re5-delete").start();
                })
                .setNegativeButton(L.t("Cancelar"), null)
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
        Toast.makeText(this, L.t("Nenhum app de arquivos disponível."), Toast.LENGTH_SHORT).show();
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
                .setNeutralButton(L.t("App Arquivos"), (d, w) -> openInFilesApp())
                .setNegativeButton(L.t("Fechar"), null)
                .show();
    }

    private void alert(String title, String msg) {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title).setMessage(msg).setPositiveButton(L.t("OK"), null).show();
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
        TextView kicker = text(L.t("RECOMPILAÇÃO ESTÁTICA · ARM64"), 11, RED, true);
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
        btnSettings.setOnClickListener(v -> openSettings(SettingsActivity.TAB_GRAPHICS));
        hero.addView(btnSettings, setLp);
        // Idioma: ao lado do ⚙ (EN, ES, PT, ZH, RU, JA; padrão = o do sistema)
        TextView btnLang = new TextView(this);
        btnLang.setText("🌐");
        btnLang.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        btnLang.setGravity(Gravity.CENTER);
        GradientDrawable langBg = new GradientDrawable();
        langBg.setShape(GradientDrawable.OVAL);
        langBg.setColor(0xAA15171B);
        langBg.setStroke(dp(1), 0x66FFFFFF);
        btnLang.setBackground(langBg);
        FrameLayout.LayoutParams langLp = new FrameLayout.LayoutParams(dp(44), dp(44), Gravity.TOP | Gravity.END);
        langLp.setMargins(0, dp(38), dp(72), 0);
        btnLang.setOnClickListener(v -> chooseLanguage());
        hero.addView(btnLang, langLp);
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
        mig.addView(cardTitle(L.t("Mover arquivos para a pasta do app"), AMBER));
        migrateText = text("", 13, TEXT_DIM, false);
        mig.addView(migrateText);
        mig.addView(button(L.t("Mover agora"), RED, TEXT, v -> migrateLegacy()), buttonLp());

        // Permissão
        LinearLayout perm = card(body);
        permCard = perm;
        perm.addView(cardTitle(L.t("Acesso aos arquivos"), AMBER));
        perm.addView(text(L.t("Permita o acesso para o app achar o game.rar em Download ou uma instalação antiga em /sdcard/re5. ")
                + L.t("Não é preciso para importar escolhendo o arquivo."), 13, TEXT_DIM, false));
        perm.addView(button(L.t("Permitir acesso"), SURFACE_2, TEXT, v -> requestStorageAccess()), buttonLp());

        // Instalar
        LinearLayout inst = card(body);
        installCard = inst;
        inst.addView(cardTitle(L.t("Instalar o jogo"), TEXT));
        inst.addView(text(L.t("Escolha o game.rar (ou .zip) com a pasta do jogo: re5dx9.exe e nativePC_MT/."), 13, TEXT_DIM, false));
        installRarButton = button("", RED, TEXT, v -> startExtraction(null, GameStorage.downloadedRar().getAbsolutePath()));
        inst.addView(installRarButton, buttonLp());
        inst.addView(button(L.t("Escolher arquivo…"), SURFACE_2, TEXT, v -> openArchivePicker()), buttonLp());

        // Multiplayer: toda a configuração da LAN fica aqui (o jogo não tem tela para isso)
        LinearLayout mp = card(body);
        mp.addView(cardTitle(L.t("Multiplayer cooperativo (LAN)"), TEXT));
        rowLanProfile = infoRow(mp, L.t("Jogador"));
        rowLanHosts = infoRow(mp, L.t("Procurar em"));
        rowLanIp = infoRow(mp, L.t("IP deste aparelho"));
        mp.addView(text(L.t("Os dois aparelhos na mesma rede Wi-Fi (ou um no hotspot do outro). No jogo: Multiplayer > ")
                + L.t("criar ou entrar na sessão."), 12, TEXT_DIM, false));
        mp.addView(button(L.t("Editar multiplayer"), SURFACE_2, TEXT, v -> showLanDialog()), buttonLp());

        // Atalhos, como nos emuladores: cada um abre a aba certa das configurações.
        LinearLayout tiles = card(body);
        tiles.addView(cardTitle(L.t("Ajustes"), TEXT));
        String[][] t = {
                {"🎨", L.t("Gráficos")}, {"🕹", L.t("Controles")}, {"✋", L.t("Editar layout")},
                {"📊", "HUD"}, {"⚡", L.t("Desempenho")}, {"🌐", L.t("Multiplayer")}};
        Runnable[] act = {
                () -> openSettings(SettingsActivity.TAB_GRAPHICS),
                () -> openSettings(SettingsActivity.TAB_CONTROLS),
                () -> startActivity(new Intent(this, ControlsEditorActivity.class)),
                () -> openSettings(SettingsActivity.TAB_HUD),
                () -> openSettings(SettingsActivity.TAB_PERF),
                this::showLanDialog};
        for (int r = 0; r < 2; r++) {
            LinearLayout line = new LinearLayout(this);
            line.setOrientation(LinearLayout.HORIZONTAL);
            for (int c = 0; c < 3; c++) {
                int i = r * 3 + c;
                line.addView(tile(t[i][0], t[i][1], act[i]), weightLp());
            }
            LinearLayout.LayoutParams lLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(84));
            lLp.topMargin = dp(8);
            tiles.addView(line, lLp);
        }

        // Arquivos do jogo
        LinearLayout info = card(body);
        info.addView(cardTitle(L.t("Arquivos do jogo"), TEXT));
        rowState = infoRow(info, L.t("Situação"));
        rowSize = infoRow(info, L.t("Tamanho"));
        rowWhere = infoRow(info, L.t("Local"));
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.addView(button(L.t("Ver arquivos"), SURFACE_2, TEXT, v -> browse(GameStorage.gameDir(this))), weightLp());
        actions.addView(button(L.t("Apagar"), SURFACE_2, 0xFFFF8A8A, v -> confirmDeleteGame()), weightLp());
        filesActions = actions;
        LinearLayout.LayoutParams aLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(46));
        aLp.topMargin = dp(12);
        info.addView(actions, aLp);

        // game.rar sobrando
        LinearLayout clean = card(body);
        rarCleanCard = clean;
        clean.addView(cardTitle(L.t("Liberar espaço"), TEXT));
        rarCleanText = text("", 13, TEXT_DIM, false);
        clean.addView(rarCleanText);
        clean.addView(button(L.t("Apagar game.rar"), SURFACE_2, 0xFFFF8A8A, v -> confirmDeleteRar()), buttonLp());

        // Aparelho
        LinearLayout dev = card(body);
        dev.addView(cardTitle(L.t("Aparelho"), TEXT));
        rowDevice = text("", 13, TEXT_DIM, false);
        dev.addView(rowDevice);

        root.addView(scroll, match());

        // Botão de jogar fixo embaixo
        LinearLayout bottom = new LinearLayout(this);
        bottom.setOrientation(LinearLayout.VERTICAL);
        bottom.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[]{0x000A0B0D, BG, BG}));
        bottom.setPadding(dp(16), dp(22), dp(16), dp(16));
        playButton = button(L.t("JOGAR"), RED, Color.WHITE, v -> launchGame());
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
        String name = L.t("Jogador");
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

    /* Mesmo formato que o runtime grava (steam_lan.c, write_identity): ele relê no início do jogo. */
    private void saveLanConfig(String name, String country, String hosts, long existingId) {
        File f = lanConfigFile();
        File parent = f.getParentFile();
        if (parent != null && !parent.exists()) parent.mkdirs();

        long id = existingId >= 100000 && existingId < 1000000 ? existingId : (100000 + (long) (Math.random() * 900000));
        name = name != null ? name.replace("#", "").trim() : "";
        if (name.isEmpty()) name = L.t("Jogador");
        if (name.length() > LAN_NAME_MAX) name = name.substring(0, LAN_NAME_MAX);
        country = country != null && country.trim().length() >= 2 ? country.trim().substring(0, 2).toUpperCase(Locale.US) : "BR";
        hosts = hosts != null ? hosts.replace(" ", "").replace(";", ",").trim() : "";

        try (FileWriter w = new FileWriter(f)) {
            w.write("# Identidade do jogador no multiplayer LAN do RE5.\n"
                    + "# Tambem editavel no launcher do app (Multiplayer).\n"
                    + "# nome:  o que os outros jogadores veem (ate " + LAN_NAME_MAX + " caracteres, sem '#').\n"
                    + "# pais:  codigo de duas letras da bandeira (BR, PT, US...).\n"
                    + "# hosts: IPs de hosts para procurar direto, separados por virgula (rede sem broadcast).\n"
                    + "# id:    seu codigo unico na rede; nao copie este arquivo para outro aparelho.\n"
                    + "#        Apague a linha para gerar um novo.\n");
            w.write("nome=" + name + "\n");
            w.write("pais=" + country + "\n");
            w.write("hosts=" + hosts + "\n");
            w.write("id=" + id + "\n");
        } catch (Exception e) {
            Toast.makeText(this, L.t("Erro ao salvar re5_lan.ini: ") + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    private static final int LAN_NAME_MAX = 24;   // RE5_LAN_MAX_NAME_LEN

    private void refreshLan() {
        LanConfig lan = loadLanConfig();
        rowLanProfile.setText(lan.name + (lan.id > 0 ? " #" + lan.id : "") + " · " + lan.country);
        rowLanHosts.setText(lan.hosts.isEmpty() ? L.t("rede local (automático)") : lan.hosts);
        String ips = localIpv4();
        rowLanIp.setText(ips.isEmpty() ? L.t("sem rede") : ips);
    }

    /** IPv4 das interfaces ativas (Wi-Fi, hotspot): o parceiro digita um destes em "Procurar em". */
    private static String localIpv4() {
        StringBuilder sb = new StringBuilder();
        try {
            for (java.net.NetworkInterface ni : java.util.Collections.list(java.net.NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                String n = ni.getName();
                if (n.startsWith("rmnet") || n.startsWith("ccmni") || n.startsWith("dummy")) continue;   // dados móveis
                for (java.net.InetAddress a : java.util.Collections.list(ni.getInetAddresses())) {
                    if (a instanceof java.net.Inet4Address && !a.isLoopbackAddress()) {
                        if (sb.length() > 0) sb.append(", ");
                        sb.append(a.getHostAddress());
                    }
                }
            }
        } catch (Exception ignored) {}
        return sb.toString();
    }

    private static boolean validHosts(String hosts) {
        if (hosts.isEmpty()) return true;
        for (String h : hosts.split(",")) {
            if (!h.matches("\\d{1,3}(\\.\\d{1,3}){3}")) return false;
            for (String p : h.split("\\.")) if (Integer.parseInt(p) > 255) return false;
        }
        return true;
    }

    private void showLanDialog() {
        LanConfig lan = loadLanConfig();
        ScrollView scroll = new ScrollView(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(20), dp(16), dp(20), dp(16));
        scroll.addView(col);

        col.addView(text(L.t("Sua identidade no cooperativo em rede local. Vale na próxima vez que o jogo abrir."), 12, TEXT_DIM, false));

        col.addView(dialogFieldLabel(L.t("Nome do jogador (até ") + LAN_NAME_MAX + L.t(" letras)")));
        EditText editName = dialogEditText(lan.name);
        editName.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(LAN_NAME_MAX)});
        col.addView(editName);

        col.addView(dialogFieldLabel(L.t("País da bandeira (duas letras: BR, PT, US...)")));
        EditText editCountry = dialogEditText(lan.country);
        editCountry.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(2),
                new android.text.InputFilter.AllCaps()});
        col.addView(editCountry);

        col.addView(dialogFieldLabel(L.t("Procurar sessão nestes IPs (opcional)")));
        EditText editHosts = dialogEditText(lan.hosts);
        editHosts.setHint(L.t("Ex.: 192.168.1.100, 192.168.43.1"));
        col.addView(editHosts);
        String ips = localIpv4();
        col.addView(text(L.t("Deixe vazio para achar a sessão sozinho na rede. Use só se a rede bloqueia a busca: ")
                + L.t("o IP do aparelho que cria a sessão. O deste aparelho: ") + (ips.isEmpty() ? L.t("sem rede") : ips) + ".",
                12, TEXT_DIM, false));

        AlertDialog dlg = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(L.t("Multiplayer (LAN)"))
                .setView(scroll)
                .setPositiveButton(L.t("Salvar"), null)
                .setNegativeButton(L.t("Cancelar"), null)
                .create();
        dlg.setOnShowListener(d -> dlg.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            String hosts = editHosts.getText().toString().replace(" ", "").replace(";", ",");
            if (!validHosts(hosts)) {
                editHosts.setError(L.t("IPs no formato 192.168.0.10, separados por vírgula"));
                return;
            }
            saveLanConfig(editName.getText().toString(), editCountry.getText().toString(), hosts, lan.id);
            refreshLan();
            Toast.makeText(this, L.t("Multiplayer salvo"), Toast.LENGTH_SHORT).show();
            dlg.dismiss();
        }));
        dlg.show();
    }

    private static final int REQUEST_SETTINGS = 1003;

    /** Escolha de idioma; a tela é refeita no idioma novo. */
    private void chooseLanguage() {
        String[] names = new String[L.NAMES.length];
        for (int i = 0; i < names.length; i++) names[i] = i < 2 ? L.t(L.NAMES[i]) : L.NAMES[i];
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(L.t("Idioma"))
                .setSingleChoiceItems(names, L.savedIndex(this), (d, w) -> {
                    L.save(this, w);
                    d.dismiss();
                    recreate();
                }).show();
    }

    private void openSettings(int tab) {
        startActivityForResult(SettingsActivity.open(this, tab), REQUEST_SETTINGS);
    }

    /** Atalho da grade: ícone grande e nome. */
    private View tile(String icon, String label, Runnable r) {
        LinearLayout t = new LinearLayout(this);
        t.setOrientation(LinearLayout.VERTICAL);
        t.setGravity(Gravity.CENTER);
        t.setBackground(flat(SURFACE_2, 14));
        t.setClickable(true);
        t.setForeground(getDrawable(android.R.drawable.list_selector_background));
        TextView i = text(icon, 24, TEXT, false);
        i.setGravity(Gravity.CENTER);
        t.addView(i);
        TextView l = text(label, 12, TEXT, true);
        l.setGravity(Gravity.CENTER);
        l.setPadding(0, dp(4), 0, 0);
        t.addView(l);
        t.setOnClickListener(v -> r.run());
        return t;
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
