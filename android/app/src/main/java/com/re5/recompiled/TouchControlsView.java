package com.re5.recompiled;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.List;

/**
 * Controles na tela, como um dispositivo de TOQUE do runtime (runtime/src/win32/touch.c).
 *
 * Nada aqui vira tecla nem mouse do SDL: os botões e os analógicos vão por
 * {@link SDLActivity#nativeTouchPad} e o toque direto nos menus por
 * {@link SDLActivity#nativeTouchPointer}. O runtime soma o "controle de toque" ao usuário 0 do
 * XInput (o jogo mostra os ícones do controle e navega os menus pelo analógico) e diz qual
 * dispositivo foi usado por último ({@link SDLActivity#nativeActiveInput}).
 *
 * Layout do controle no RE5 (tipo D, o padrão): LT mira, RT atira, LB faca, RB mapa, A corre e
 * (mirando) recarrega, B parceiro, X ação, Y inventário, Back pula a cena, Start pausa, D-pad
 * troca rápida.
 *
 * Como cada ícone age (pedido do usuário, 04/11):
 * - Correr, Mirar e Faca ALTERNAM: um toque liga, outro desliga; ligar um desliga os outros
 *   (no RE5 não se corre mirando).
 * - Arma: segurada, atira mirando ou dá a facada com a faca pronta. Sem mira nem faca, segurar a
 *   arma mira e atira (tiro rápido) e soltar volta.
 * - Recarregar: mirando, recarrega; sem mira, mira, recarrega e volta.
 * - Inventário: abre e liga o modo de toque nele (toque escolhe, arrastar à esquerda navega) até
 *   tocar no ícone de novo.
 * - Menus: toque direto (padrão) ou botões (OK/Voltar com setas e analógico), escolha do usuário.
 *
 * Cada controle tem posição (fração da tela) e tamanho próprios, salvos em "re5_layout". O
 * editor (setEditMode) é desenhado aqui mesmo: barra com voltar, Opções, Restaurar e Salvar;
 * arrastar move, pinça com dois dedos muda o tamanho. O launcher e o painel do jogo usam o mesmo.
 *
 * Gameplay x menu: o cursor escondido pelo jogo = gameplay (nativeIsCursorHidden).
 */
public class TouchControlsView extends View {

    public interface OnMenuToggleListener {
        void onToggleMenu();
    }

    /** Editor de layout: a barra do alto chama de volta quem o abriu. */
    public interface EditorListener {
        void onEditorOptions();
        void onEditorClose(boolean saved);
    }

    // XINPUT_GAMEPAD_* (re5/touch.h)
    private static final int DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008;
    private static final int START = 0x0010, BACK = 0x0020, LB = 0x0100, RB = 0x0200;
    private static final int A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;

    // re5_input_source
    private static final int SRC_KBM = 1, SRC_PAD = 2;
    // RE5_TOUCH_PTR_*
    private static final int PTR_DOWN = 0, PTR_MOVE = 1, PTR_UP = 2;

    /** O que o controle faz. */
    private static final int ACT_HOLD = 0, ACT_RUN = 1, ACT_AIM = 2, ACT_KNIFE = 3, ACT_FIRE = 4, ACT_RELOAD = 5,
            ACT_INVENTORY = 6, ACT_DPAD = 7, ACT_STICK = 8;
    /** Onde aparece. */
    private static final int SHOW_ALWAYS = 0, SHOW_GAMEPLAY = 1, SHOW_MENU = 2, SHOW_MENU_BUTTONS = 3;

    private static final float STICK_DEADZONE = 0.10f;   // fração do raio
    private static final long TICK_MS = 16;
    private static final long FIRE_AIM_DELAY_MS = 150;  // tiro rápido: mira antes de atirar
    private static final String PREFS_LAYOUT = "re5_layout";

    private OnMenuToggleListener menuToggleListener;
    private EditorListener editorListener;

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint iconPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF tmpRect = new RectF();

    static final class Ctl {
        final String id, label;
        final int icon, act, bits, show;
        final float defX, defY, defSize;   // centro (fração da tela) e diâmetro (dp)
        float x, y, scale = 1f;
        Bitmap bmp;
        final RectF bounds = new RectF();
        int pointerId = -1;
        boolean latched;
        int dpadBits;                       // D-pad: direção do dedo agora

        Ctl(String id, String label, int icon, int act, int bits, int show, float defX, float defY, float defSize) {
            this.id = id;
            this.label = label;
            this.icon = icon;
            this.act = act;
            this.bits = bits;
            this.show = show;
            this.defX = defX;
            this.defY = defY;
            this.defSize = defSize;
            this.x = defX;
            this.y = defY;
        }

        boolean isDown() { return pointerId != -1; }
    }

    private final List<Ctl> ctls = new ArrayList<>();
    private Ctl cDpad, cStick, cFire, cRun, cAim, cKnife, cReload, cAction, cPartner, cInventory, cMap;
    private Ctl cSwitch, cPause, cOk, cBackMenu;

    // Pílulas do topo (do app, não do jogo)
    private final RectF pillMenu = new RectF(), pillHide = new RectF();

    // Analógico esquerdo flutuante
    private int stickPointer = -1;
    private float stickBaseX, stickBaseY, stickKnobX, stickKnobY;
    private float stickRadius, stickKnobRadius;
    private float lx, ly;

    // Câmera: arrasto livre no lado direito vira o analógico direito
    private int lookPointer = -1;
    private float lookLastX, lookLastY, lookAccX, lookAccY;
    private long lookLastTick;
    private float rx, ry;
    private float lookSensitivity = 1.0f;
    private boolean invertY = false;

    // Toque ainda indefinido (menu/inventário): vira clique se soltar parado, ou arrasto
    private int pendingPointer = -1;
    private float pendingX, pendingY;
    private boolean pendingLeft;
    private int dragPointer = -1;
    private float dragLastX, dragLastY;

    // Sequências com tempo
    private long fireStart = 0;     // tiro rápido sem mira
    private long reloadStart = 0;   // recarregar
    private boolean reloadWithAim;  // a sequência segura LT (não estava mirando)

    private boolean inGameplay = false;
    private boolean inventoryMode = false;
    private boolean menuButtons = false;   // menus por botões em vez de toque direto
    private boolean controlsEnabled = true;
    private boolean hideWithPad = true;
    private boolean hiddenByDevice = false;
    private boolean vibration = true;
    private float controlsOpacity = 0.85f;
    private float controlsScale = 1.0f;

    // Editor
    private boolean editMode = false;
    private Ctl editCtl;
    private int editPointer = -1, editPointer2 = -1;
    private float editOffX, editOffY, editPinchStart, editScaleStart;
    private final RectF barBack = new RectF(), barOptions = new RectF(), barReset = new RectF(), barSave = new RectF();

    // Último estado enviado ao runtime (só manda quando muda)
    private int sentButtons = -1;
    private float sentLx, sentLy, sentRx, sentRy, sentLt, sentRt;

    // Aviso de dispositivo no alto da tela
    private int lastSource = 0;
    private String notice;
    private long noticeAt = -10000;
    private static final long NOTICE_MS = 3000;

    private Vibrator vibrator;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private final Runnable stateCheck = new Runnable() {
        @Override
        public void run() {
            if (!editMode) {
                if (sentButtons == -1) sendPad();   // o jogo vê o controle de toque desde a abertura
                boolean was = inGameplay;
                try {
                    inGameplay = SDLActivity.nativeIsCursorHidden();
                } catch (Throwable ignored) {
                    inGameplay = true;
                }
                int src = 0;
                try {
                    src = SDLActivity.nativeActiveInput();
                } catch (Throwable ignored) {}
                if (src != lastSource && src != 0) {
                    if (SystemClock.uptimeMillis() - noticeAt > 2500) {
                        if (src == SRC_PAD) showNotice(L.t("🎮 Usando o controle · toque na tela para voltar"));
                        else if (src == SRC_KBM) showNotice(L.t("⌨ Usando o teclado · toque na tela para voltar"));
                        else if (lastSource == SRC_PAD || lastSource == SRC_KBM) showNotice(L.t("✋ Usando o toque"));
                    }
                    lastSource = src;
                }
                boolean hide = hideWithPad && (src == SRC_PAD || src == SRC_KBM);
                if (hide != hiddenByDevice) {
                    hiddenByDevice = hide;
                    if (hide) releaseAll();
                    invalidate();
                }
                if (was != inGameplay) {
                    releaseGameplayOnly();
                    invalidate();
                }
            }
            handler.postDelayed(this, 120);
        }
    };

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            updateLook();
            sendPad();
            if (lookPointer != -1 || rx != 0 || ry != 0 || fireStart != 0 || reloadStart != 0) {
                handler.postDelayed(this, TICK_MS);
            } else {
                ticking = false;
            }
            invalidate();
        }
    };
    private boolean ticking = false;

    public TouchControlsView(Context context) {
        super(context);
        init();
    }

    public TouchControlsView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    // ------------------------------------------------------------------ configuração

    public void setOnMenuToggleListener(OnMenuToggleListener listener) { menuToggleListener = listener; }

    public void setEditorListener(EditorListener l) { editorListener = l; }

    public void setControlsEnabled(boolean enabled) {
        controlsEnabled = enabled;
        if (!enabled) releaseAll();
        invalidate();
    }

    public void setControlsOpacity(float opacity) {
        controlsOpacity = Math.max(0.2f, Math.min(1.0f, opacity));
        invalidate();
    }

    public void setControlsScale(float scale) {
        controlsScale = Math.max(0.6f, Math.min(1.6f, scale));
        layoutControls();
        invalidate();
    }

    /** 0,5 a 4: 1 chega à rotação máxima com ~0,45 dp/ms de dedo (antes era 1 dp/ms). */
    public void setLookSensitivity(float s) { lookSensitivity = Math.max(0.3f, Math.min(4f, s)); }

    public void setInvertY(boolean inv) { invertY = inv; }

    public void setVibration(boolean on) { vibration = on; }

    public void setMenuButtons(boolean buttons) {
        menuButtons = buttons;
        invalidate();
    }

    public void setHideWithPad(boolean hide) { hideWithPad = hide; }

    /** Editor de layout: tudo aparece, nada vai ao jogo. */
    public void setEditMode(boolean on) {
        if (on) releaseAll();
        editMode = on;
        editCtl = null;
        editPointer = editPointer2 = -1;
        invalidate();
    }

    public boolean isEditMode() { return editMode; }

    /** Aviso curto no alto da tela (controle/teclado conectado, dispositivo em uso). */
    public void showNotice(String text) {
        notice = text;
        noticeAt = SystemClock.uptimeMillis();
        invalidate();
        handler.postDelayed(this::invalidate, NOTICE_MS + 50);
    }

    // ------------------------------------------------------------------ controles

    private Ctl add(String id, String label, int icon, int act, int bits, int show, float x, float y, float size) {
        Ctl c = new Ctl(id, label, icon, act, bits, show, x, y, size);
        ctls.add(c);
        return c;
    }

    private void init() {
        vibrator = (Vibrator) getContext().getSystemService(Context.VIBRATOR_SERVICE);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setColor(Color.WHITE);
        textPaint.setFakeBoldText(true);

        // Posições padrão: a tela de referência do usuário (04/11), com a arma do lado esquerdo,
        // junto do analógico e das setas.
        cDpad = add("dpad", "", R.drawable.ic_ctl_dpad, ACT_DPAD, 0, SHOW_ALWAYS, 0.085f, 0.33f, 104);
        cStick = add("stick", "", R.drawable.ic_ctl_joystick, ACT_STICK, 0, SHOW_ALWAYS, 0.125f, 0.74f, 132);
        cFire = add("fire", "", R.drawable.ic_ctl_gun, ACT_FIRE, 0, SHOW_GAMEPLAY, 0.285f, 0.58f, 86);
        cPartner = add("partner", "", R.drawable.ic_ctl_partner, ACT_HOLD, B, SHOW_GAMEPLAY, 0.795f, 0.24f, 58);
        cInventory = add("inventory", "", R.drawable.ic_ctl_inventory, ACT_INVENTORY, Y, SHOW_GAMEPLAY, 0.86f, 0.24f, 58);
        cMap = add("map", "", R.drawable.ic_ctl_map, ACT_HOLD, RB, SHOW_GAMEPLAY, 0.925f, 0.24f, 58);
        cAction = add("action", "", R.drawable.ic_ctl_hand, ACT_HOLD, X, SHOW_GAMEPLAY, 0.845f, 0.42f, 66);
        cKnife = add("knife", "", R.drawable.ic_ctl_knife, ACT_KNIFE, 0, SHOW_GAMEPLAY, 0.935f, 0.45f, 62);
        cAim = add("aim", "", R.drawable.ic_ctl_aim, ACT_AIM, 0, SHOW_GAMEPLAY, 0.757f, 0.52f, 76);
        cReload = add("reload", "", R.drawable.ic_ctl_reload, ACT_RELOAD, 0, SHOW_GAMEPLAY, 0.865f, 0.665f, 72);
        cRun = add("run", "", R.drawable.ic_ctl_run, ACT_RUN, 0, SHOW_GAMEPLAY, 0.74f, 0.76f, 68);
        cSwitch = add("switch", "", R.drawable.ic_ctl_switch, ACT_HOLD, BACK, SHOW_ALWAYS, 0.378f, 0.935f, 48);
        cPause = add("pause", "", R.drawable.ic_ctl_pause, ACT_HOLD, START, SHOW_ALWAYS, 0.49f, 0.935f, 48);
        // Menus: Voltar sempre; OK só no modo "menus por botões".
        cBackMenu = add("menu_back", L.t("VOLTAR"), 0, ACT_HOLD, B, SHOW_MENU, 0.60f, 0.70f, 64);
        cOk = add("menu_ok", L.t("OK"), 0, ACT_HOLD, A, SHOW_MENU_BUTTONS, 0.64f, 0.45f, 72);
        for (Ctl c : ctls) {
            if (c.icon != 0) c.bmp = BitmapFactory.decodeResource(getResources(), c.icon);
        }
        loadLayout();
    }

    /** Posições e tamanhos salvos (fração da tela; escala por controle). */
    private void loadLayout() {
        SharedPreferences p = getContext().getSharedPreferences(PREFS_LAYOUT, Context.MODE_PRIVATE);
        for (Ctl c : ctls) {
            c.x = p.getFloat(c.id + "_x", c.defX);
            c.y = p.getFloat(c.id + "_y", c.defY);
            c.scale = p.getFloat(c.id + "_s", 1f);
        }
        layoutControls();
    }

    public void saveLayout() {
        SharedPreferences.Editor e = getContext().getSharedPreferences(PREFS_LAYOUT, Context.MODE_PRIVATE).edit();
        for (Ctl c : ctls) {
            e.putFloat(c.id + "_x", c.x).putFloat(c.id + "_y", c.y).putFloat(c.id + "_s", c.scale);
        }
        e.apply();
    }

    public void resetLayout() {
        for (Ctl c : ctls) {
            c.x = c.defX;
            c.y = c.defY;
            c.scale = 1f;
        }
        layoutControls();
        invalidate();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        handler.post(stateCheck);
    }

    @Override
    protected void onDetachedFromWindow() {
        handler.removeCallbacks(stateCheck);
        handler.removeCallbacks(tick);
        ticking = false;
        super.onDetachedFromWindow();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        layoutControls();
    }

    private float sizeOf(Ctl c) { return dp(c.defSize) * c.scale * controlsScale; }

    private void placeCtl(Ctl c) {
        float w = getWidth(), h = getHeight();
        float r = sizeOf(c) / 2f;
        float cx = Math.max(r, Math.min(w - r, c.x * w)), cy = Math.max(r, Math.min(h - r, c.y * h));
        c.bounds.set(cx - r, cy - r, cx + r, cy + r);
    }

    private void layoutControls() {
        int w = getWidth(), h = getHeight();
        if (w <= 0 || h <= 0) return;
        for (Ctl c : ctls) placeCtl(c);
        stickRadius = cStick.bounds.width() / 2f;
        stickKnobRadius = stickRadius * 0.42f;
        if (stickPointer == -1) resetStickVisual();
        float topY = dp(12);
        pillMenu.set(w / 2f - dp(52), topY, w / 2f + dp(52), topY + dp(32));
        pillHide.set(w - dp(50), topY, w - dp(14), topY + dp(32));
        float bh = dp(46), by = dp(14);
        barBack.set(dp(16), by, dp(16) + bh, by + bh);
        barSave.set(w - dp(16) - dp(150), by, w - dp(16), by + bh);
        barReset.set(barSave.left - dp(12) - dp(176), by, barSave.left - dp(12), by + bh);
        barOptions.set(barReset.left - dp(12) - dp(150), by, barReset.left - dp(12), by + bh);
    }

    private void resetStickVisual() {
        stickBaseX = stickKnobX = cStick.bounds.centerX();
        stickBaseY = stickKnobY = cStick.bounds.centerY();
    }

    private boolean visible(Ctl c) {
        if (editMode) return true;
        if (!controlsEnabled || hiddenByDevice) return false;
        boolean menu = !inGameplay;
        switch (c.show) {
            case SHOW_GAMEPLAY: return !menu;
            case SHOW_MENU: return menu;
            case SHOW_MENU_BUTTONS: return menu && menuButtons;
            default: return true;
        }
    }

    // ------------------------------------------------------------------ desenho

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (editMode) {
            drawEditor(canvas);
            return;
        }
        drawPill(canvas, pillMenu, L.t("⚙ PAINEL"), 0xFF3B82F6);
        drawPill(canvas, pillHide, controlsEnabled ? "🎮" : "✋", 0xFF64748B);
        drawNotice(canvas);
        if (!controlsEnabled || hiddenByDevice) return;

        if (inGameplay || menuButtons || inventoryMode) drawStick(canvas, inGameplay ? 1f : 0.6f);
        for (Ctl c : ctls) {
            if (c == cStick || !visible(c)) continue;
            drawCtl(canvas, c, false);
        }
        if (inventoryMode) drawHint(canvas, L.t("Inventário: toque para escolher · arraste à esquerda para navegar · 🎒 fecha"));
        else if (!inGameplay) {
            drawHint(canvas, menuButtons ? L.t("Menus por botões: setas ou analógico para escolher, OK para confirmar")
                                         : L.t("Toque para escolher · arraste à esquerda para navegar"));
        }
    }

    private int alpha(float a) { return Math.max(0, Math.min(255, (int) (a * 255))); }

    private void drawCtl(Canvas canvas, Ctl c, boolean selected) {
        float m = controlsOpacity * (inGameplay || editMode ? 1f : 0.85f);
        boolean active = c.isDown() || c.latched || (c == cInventory && inventoryMode)
                || (c == cFire && (fireStart != 0)) || (c == cReload && reloadStart != 0);
        float r = c.bounds.width() / 2f, cx = c.bounds.centerX(), cy = c.bounds.centerY();
        // fundo: escuro em repouso; vermelho do RE5 quando ativo (ligado ou apertado)
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(alpha((active ? 0.70f : 0.28f) * m) << 24 | (active ? 0xB91C1C : 0x0B0F19));
        canvas.drawCircle(cx, cy, r * 0.96f, paint);
        if (c.bmp != null) {
            iconPaint.setAlpha(alpha(Math.min(1f, (active ? 1f : 0.85f) * m + 0.1f)));
            canvas.drawBitmap(c.bmp, null, c.bounds, iconPaint);
        } else {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(2.2f));
            paint.setColor(alpha(0.9f * m) << 24 | 0xFFFFFF);
            canvas.drawCircle(cx, cy, r - dp(2), paint);
            textPaint.setTextSize(r > dp(32) ? dp(15) : dp(12));
            textPaint.setColor(alpha(Math.min(1f, m + 0.15f)) << 24 | 0xFFFFFF);
            canvas.drawText(c.label, cx, cy - (textPaint.descent() + textPaint.ascent()) / 2, textPaint);
        }
        if (c.latched) {   // ligado (alterna): anel extra
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(3));
            paint.setColor(alpha(0.95f) << 24 | 0xF87171);
            canvas.drawCircle(cx, cy, r + dp(3), paint);
        }
        if (c == cDpad && c.dpadBits != 0) drawDpadPress(canvas, c);
        if (selected) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(2));
            paint.setColor(0xFF38BDF8);
            tmpRect.set(c.bounds);
            tmpRect.inset(-dp(6), -dp(6));
            canvas.drawRoundRect(tmpRect, dp(10), dp(10), paint);
        }
    }

    private void drawDpadPress(Canvas canvas, Ctl c) {
        float r = c.bounds.width() / 2f, cx = c.bounds.centerX(), cy = c.bounds.centerY();
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0x99B91C1C);
        float d = r * 0.55f, s = r * 0.26f;
        if ((c.dpadBits & DPAD_UP) != 0) canvas.drawCircle(cx, cy - d, s, paint);
        if ((c.dpadBits & DPAD_DOWN) != 0) canvas.drawCircle(cx, cy + d, s, paint);
        if ((c.dpadBits & DPAD_LEFT) != 0) canvas.drawCircle(cx - d, cy, s, paint);
        if ((c.dpadBits & DPAD_RIGHT) != 0) canvas.drawCircle(cx + d, cy, s, paint);
    }

    private void drawStick(Canvas canvas, float mod) {
        float m = controlsOpacity * mod;
        boolean on = stickPointer != -1;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(alpha(0.22f * m) << 24 | 0x0B0F19);
        canvas.drawCircle(stickBaseX, stickBaseY, stickRadius, paint);
        if (cStick.bmp != null) {
            iconPaint.setAlpha(alpha(0.75f * m));
            tmpRect.set(stickBaseX - stickRadius, stickBaseY - stickRadius, stickBaseX + stickRadius, stickBaseY + stickRadius);
            canvas.drawBitmap(cStick.bmp, null, tmpRect, iconPaint);
        }
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(alpha((on ? 0.85f : 0.45f) * m) << 24 | (on ? 0xB91C1C : 0x1E293B));
        canvas.drawCircle(stickKnobX, stickKnobY, stickKnobRadius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(2));
        paint.setColor(alpha(0.9f * m) << 24 | 0xFFFFFF);
        canvas.drawCircle(stickKnobX, stickKnobY, stickKnobRadius, paint);
    }

    private void drawPill(Canvas canvas, RectF r, String label, int color) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0x770F172A);
        canvas.drawRoundRect(r, dp(16), dp(16), paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1.2f));
        paint.setColor(color);
        canvas.drawRoundRect(r, dp(16), dp(16), paint);
        textPaint.setTextSize(dp(11));
        textPaint.setColor(Color.WHITE);
        canvas.drawText(label, r.centerX(), r.centerY() - (textPaint.descent() + textPaint.ascent()) / 2, textPaint);
    }

    private void drawHint(Canvas canvas, String t) {
        textPaint.setTextSize(dp(11));
        float tw = textPaint.measureText(t);
        float cx = getWidth() / 2f, y = dp(62);
        tmpRect.set(cx - tw / 2 - dp(12), y - dp(15), cx + tw / 2 + dp(12), y + dp(8));
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0x990F172A);
        canvas.drawRoundRect(tmpRect, dp(12), dp(12), paint);
        textPaint.setColor(0xEEFFFFFF);
        canvas.drawText(t, cx, y, textPaint);
    }

    private void drawNotice(Canvas canvas) {
        if (notice == null || SystemClock.uptimeMillis() - noticeAt > NOTICE_MS) return;
        textPaint.setTextSize(dp(13));
        float tw = textPaint.measureText(notice);
        float cx = getWidth() / 2f, y = dp(96);
        tmpRect.set(cx - tw / 2 - dp(18), y - dp(22), cx + tw / 2 + dp(18), y + dp(12));
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xE00F172A);
        canvas.drawRoundRect(tmpRect, dp(17), dp(17), paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1.2f));
        paint.setColor(0xAA38BDF8);
        canvas.drawRoundRect(tmpRect, dp(17), dp(17), paint);
        textPaint.setColor(Color.WHITE);
        canvas.drawText(notice, cx, y, textPaint);
    }

    private void drawEditor(Canvas canvas) {
        canvas.drawColor(0x55000000);
        for (Ctl c : ctls) drawCtl(canvas, c, c == editCtl);
        // barra do alto, como na tela de referência
        textPaint.setTextAlign(Paint.Align.LEFT);
        textPaint.setTextSize(dp(20));
        textPaint.setColor(Color.WHITE);
        canvas.drawText("←", barBack.left + dp(10), barBack.centerY() + dp(7), textPaint);
        canvas.drawText(L.t("CONTROLES NA TELA"), barBack.right + dp(10), barBack.centerY() + dp(7), textPaint);
        textPaint.setTextAlign(Paint.Align.CENTER);
        drawBarButton(canvas, barOptions, L.t("⚙ OPÇÕES"), false);
        drawBarButton(canvas, barReset, L.t("↺ RESTAURAR"), false);
        drawBarButton(canvas, barSave, L.t("✓ SALVAR"), true);
        textPaint.setTextSize(dp(13));
        textPaint.setColor(0xDDFFFFFF);
        String hint = editCtl == null ? L.t("Arraste para mover. Pince com dois dedos para mudar o tamanho.")
                : String.format(java.util.Locale.US, L.t("Tamanho: %d%%"), Math.round(editCtl.scale * 100));
        canvas.drawText(hint, getWidth() / 2f, barSave.bottom + dp(28), textPaint);
    }

    private void drawBarButton(Canvas canvas, RectF r, String label, boolean primary) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(primary ? 0xE6F1F5F9 : 0x99111827);
        canvas.drawRoundRect(r, r.height() / 2, r.height() / 2, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1.2f));
        paint.setColor(0x66FFFFFF);
        canvas.drawRoundRect(r, r.height() / 2, r.height() / 2, paint);
        textPaint.setTextSize(dp(14));
        textPaint.setColor(primary ? 0xFF111827 : Color.WHITE);
        canvas.drawText(label, r.centerX(), r.centerY() - (textPaint.descent() + textPaint.ascent()) / 2, textPaint);
    }

    // ------------------------------------------------------------------ toque

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (editMode) return onEditTouch(e);
        int action = e.getActionMasked();
        int idx = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                onDown(e.getPointerId(idx), e.getX(idx), e.getY(idx));
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) onMove(e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_UP:
                onUp(e.getPointerId(idx), e.getX(idx), e.getY(idx));
                break;
            case MotionEvent.ACTION_CANCEL:
                releaseAll();
                break;
        }
        sendPad();
        invalidate();
        return true;
    }

    private Ctl hit(float x, float y) {
        Ctl best = null;
        float bestD = Float.MAX_VALUE;
        for (Ctl c : ctls) {
            if (c == cStick || !visible(c)) continue;
            float r = c.bounds.width() / 2f * 1.12f;   // folga para o polegar
            float dx = x - c.bounds.centerX(), dy = y - c.bounds.centerY();
            float d = dx * dx + dy * dy;
            if (d <= r * r && d < bestD) {
                bestD = d;
                best = c;
            }
        }
        return best;
    }

    /** Toque fora de botão que vira clique/arrasto (menu por toque ou inventário). */
    private boolean directTouch() { return inventoryMode || (!inGameplay && !menuButtons); }

    private void onDown(int id, float x, float y) {
        if (pillMenu.contains(x, y)) {
            vibrateClick();
            if (menuToggleListener != null) menuToggleListener.onToggleMenu();
            return;
        }
        if (pillHide.contains(x, y)) {
            vibrateClick();
            setControlsEnabled(!controlsEnabled);
            return;
        }
        noteTouch();
        hiddenByDevice = false;   // voltou para o toque depois do controle físico
        if (!controlsEnabled) {
            startPending(id, x, y, false);
            return;
        }

        Ctl c = hit(x, y);
        if (c != null) {
            vibrateClick();
            press(c, id, x, y);
            return;
        }
        if (directTouch()) {
            startPending(id, x, y, x < getWidth() * 0.45f);
            return;
        }
        if (x < getWidth() * 0.45f) {
            if (stickPointer == -1) startStick(id, x, y);
        } else if (inGameplay && lookPointer == -1) {
            startLook(id, x, y);
        }
    }

    private void press(Ctl c, int id, float x, float y) {
        switch (c.act) {
            case ACT_RUN:
                setLatch(cRun, !cRun.latched);
                return;
            case ACT_AIM:
                setLatch(cAim, !cAim.latched);
                return;
            case ACT_KNIFE:
                setLatch(cKnife, !cKnife.latched);
                return;
            case ACT_RELOAD:
                if (reloadStart == 0) {
                    setLatchOff(cRun);   // o A do correr seguraria o A do recarregar
                    reloadWithAim = !cAim.latched;
                    reloadStart = SystemClock.uptimeMillis();
                    startTick();
                }
                return;
            case ACT_FIRE:
                c.pointerId = id;
                if (!cAim.latched && !cKnife.latched) {   // tiro rápido: mira e atira enquanto segura
                    setLatchOff(cRun);
                    fireStart = SystemClock.uptimeMillis();
                    startTick();
                }
                // o mesmo dedo arrastado a partir da arma também mexe a mira
                if (lookPointer == -1) startLook(id, x, y);
                return;
            case ACT_INVENTORY:
                c.pointerId = id;   // aperta Y; o modo de toque alterna
                inventoryMode = !inventoryMode;
                if (inventoryMode) releaseGameplayLatches();
                return;
            case ACT_DPAD:
                c.pointerId = id;
                c.dpadBits = dpadDir(c, x, y);
                return;
            default:
                c.pointerId = id;
                if (c == cPartner || c == cBackMenu) inventoryMode = false;   // B fecha o inventário
                if (c == cPause) inventoryMode = false;
        }
    }

    /** Alternáveis: ligar um desliga os outros (no RE5 não se corre mirando). */
    private void setLatch(Ctl c, boolean on) {
        if (on) {
            for (Ctl o : new Ctl[]{cRun, cAim, cKnife}) if (o != c) o.latched = false;
        }
        c.latched = on;
    }

    private void setLatchOff(Ctl c) { c.latched = false; }

    private void releaseGameplayLatches() {
        cRun.latched = cAim.latched = cKnife.latched = false;
        fireStart = 0;
    }

    private int dpadDir(Ctl c, float x, float y) {
        float dx = x - c.bounds.centerX(), dy = y - c.bounds.centerY();
        float r = c.bounds.width() / 2f;
        if (dx * dx + dy * dy < (r * 0.2f) * (r * 0.2f)) return 0;
        int bits = 0;
        // 8 direções: diagonal quando os dois eixos passam de ~40% do maior
        float ax = Math.abs(dx), ay = Math.abs(dy), mx = Math.max(ax, ay);
        if (ax > mx * 0.4f) bits |= dx < 0 ? DPAD_LEFT : DPAD_RIGHT;
        if (ay > mx * 0.4f) bits |= dy < 0 ? DPAD_UP : DPAD_DOWN;
        return bits;
    }

    private void startPending(int id, float x, float y, boolean left) {
        if (pendingPointer != -1 || dragPointer != -1) return;
        pendingPointer = id;
        pendingX = x;
        pendingY = y;
        pendingLeft = left;
    }

    private void startStick(int id, float x, float y) {
        stickPointer = id;
        stickBaseX = Math.max(stickRadius * 0.6f, x);
        stickBaseY = Math.min(getHeight() - stickRadius * 0.6f, Math.max(stickRadius, y));
        stickKnobX = x;
        stickKnobY = y;
        updateStick(x, y);
    }

    private void startLook(int id, float x, float y) {
        lookPointer = id;
        lookLastX = x;
        lookLastY = y;
        lookAccX = lookAccY = 0;
        lookLastTick = SystemClock.uptimeMillis();
        startTick();
    }

    private void onMove(int id, float x, float y) {
        if (id == stickPointer) updateStick(x, y);
        if (id == lookPointer) {
            lookAccX += x - lookLastX;
            lookAccY += y - lookLastY;
            lookLastX = x;
            lookLastY = y;
        }
        if (cDpad.pointerId == id) cDpad.dpadBits = dpadDir(cDpad, x, y);
        if (id == pendingPointer) {
            float dx = x - pendingX, dy = y - pendingY;
            if (dx * dx + dy * dy > dp(14) * dp(14)) {
                pendingPointer = -1;
                if (pendingLeft) {
                    startStick(id, pendingX, pendingY);
                    updateStick(x, y);
                } else {
                    dragPointer = id;
                    dragLastX = x;
                    dragLastY = y;
                    pointer(PTR_DOWN, pendingX, pendingY);
                    pointer(PTR_MOVE, x, y);
                }
            }
        }
        if (id == dragPointer) {
            dragLastX = x;
            dragLastY = y;
            pointer(PTR_MOVE, x, y);
        }
    }

    private void onUp(int id, float x, float y) {
        if (id == pendingPointer) {   // toque parado: clique onde tocou
            pendingPointer = -1;
            vibrateClick();
            pointer(PTR_DOWN, pendingX, pendingY);
            pointer(PTR_UP, pendingX, pendingY);
        }
        if (id == dragPointer) {
            dragPointer = -1;
            pointer(PTR_UP, x, y);
        }
        for (Ctl c : ctls) {
            if (c.pointerId != id) continue;
            c.pointerId = -1;
            if (c == cDpad) c.dpadBits = 0;
            if (c == cFire) fireStart = 0;
        }
        if (id == stickPointer) {
            stickPointer = -1;
            lx = ly = 0;
            resetStickVisual();
        }
        if (id == lookPointer) lookPointer = -1;
    }

    /** Analógico com zona morta radial e reescala (sem degrau na saída da zona morta). */
    private void updateStick(float x, float y) {
        float dx = x - stickBaseX, dy = y - stickBaseY;
        float dist = (float) Math.sqrt(dx * dx + dy * dy);
        if (dist > stickRadius) {
            float over = dist - stickRadius;   // passou da borda: a base acompanha o dedo
            stickBaseX += dx / dist * over;
            stickBaseY += dy / dist * over;
            dx = x - stickBaseX;
            dy = y - stickBaseY;
            dist = stickRadius;
        }
        stickKnobX = stickBaseX + dx;
        stickKnobY = stickBaseY + dy;
        float mag = dist / stickRadius;
        if (mag < STICK_DEADZONE || dist == 0) {
            lx = ly = 0;
            return;
        }
        float out = Math.min(1f, (mag - STICK_DEADZONE) / (1f - STICK_DEADZONE));
        lx = dx / dist * out;
        ly = -dy / dist * out;   // XInput: Y para cima positivo
    }

    /** Arrasto -> analógico direito proporcional à velocidade do dedo. */
    private void updateLook() {
        long now = SystemClock.uptimeMillis();
        float dt = Math.max(1, now - lookLastTick);
        lookLastTick = now;
        float vx = lookAccX / dp(1) / dt, vy = lookAccY / dp(1) / dt;   // dp por ms
        lookAccX = lookAccY = 0;
        float tx = lookCurve(vx), ty = -lookCurve(vy);
        if (invertY) ty = -ty;
        if (lookPointer == -1) tx = ty = 0;
        rx += (tx - rx) * 0.7f;
        ry += (ty - ry) * 0.7f;
        if (Math.abs(rx) < 0.01f) rx = 0;
        if (Math.abs(ry) < 0.01f) ry = 0;
    }

    private float lookCurve(float v) {
        float a = Math.abs(v) * lookSensitivity;
        if (a < 0.01f) return 0;
        // 0,25 = logo acima da zona morta do jogo; deflexão total a ~0,45 dp/ms (sensibilidade 1).
        // Antes eram 1 dp/ms: o dedo precisava correr o dobro e a câmera "não acompanhava".
        float out = 0.25f + 0.75f * Math.min(1f, a / 0.45f);
        return Math.signum(v) * out;
    }

    private void startTick() {
        if (ticking) return;
        ticking = true;
        handler.postDelayed(tick, TICK_MS);
    }

    // ------------------------------------------------------------------ editor

    private boolean onEditTouch(MotionEvent e) {
        int action = e.getActionMasked();
        int idx = e.getActionIndex();
        int id = e.getPointerId(idx);
        float x = e.getX(idx), y = e.getY(idx);
        switch (action) {
            case MotionEvent.ACTION_DOWN:
                if (barBack.contains(x, y)) {
                    loadLayout();   // descarta o que não foi salvo
                    if (editorListener != null) editorListener.onEditorClose(false);
                    return true;
                }
                if (barOptions.contains(x, y)) {
                    if (editorListener != null) editorListener.onEditorOptions();
                    return true;
                }
                if (barReset.contains(x, y)) {
                    vibrateClick();
                    resetLayout();
                    return true;
                }
                if (barSave.contains(x, y)) {
                    vibrateClick();
                    saveLayout();
                    if (editorListener != null) editorListener.onEditorClose(true);
                    return true;
                }
                editCtl = null;
                for (int i = ctls.size() - 1; i >= 0; i--) {
                    Ctl c = ctls.get(i);
                    float r = c.bounds.width() / 2f * 1.1f;
                    float dx = x - c.bounds.centerX(), dy = y - c.bounds.centerY();
                    if (dx * dx + dy * dy <= r * r) {
                        editCtl = c;
                        break;
                    }
                }
                if (editCtl != null) {
                    editPointer = id;
                    editOffX = editCtl.bounds.centerX() - x;
                    editOffY = editCtl.bounds.centerY() - y;
                    vibrateClick();
                }
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
                if (editCtl != null && editPointer != -1 && editPointer2 == -1) {
                    editPointer2 = id;
                    editPinchStart = pinchDist(e);
                    editScaleStart = editCtl.scale;
                }
                break;
            case MotionEvent.ACTION_MOVE:
                if (editCtl == null || editPointer == -1) break;
                if (editPointer2 != -1) {
                    float d = pinchDist(e);
                    if (editPinchStart > 0 && d > 0) {
                        editCtl.scale = Math.max(0.5f, Math.min(2.0f, editScaleStart * d / editPinchStart));
                        placeCtl(editCtl);
                    }
                } else {
                    int pi = e.findPointerIndex(editPointer);
                    if (pi < 0) break;
                    float w = Math.max(1, getWidth()), h = Math.max(1, getHeight());
                    editCtl.x = Math.max(0f, Math.min(1f, (e.getX(pi) + editOffX) / w));
                    editCtl.y = Math.max(0.1f, Math.min(1f, (e.getY(pi) + editOffY) / h));
                    placeCtl(editCtl);
                }
                if (editCtl == cStick) {
                    stickRadius = cStick.bounds.width() / 2f;
                    stickKnobRadius = stickRadius * 0.42f;
                    resetStickVisual();
                }
                break;
            case MotionEvent.ACTION_POINTER_UP:
                if (id == editPointer2) editPointer2 = -1;
                else if (id == editPointer) {
                    editPointer = editPointer2;   // o dedo que fica continua arrastando
                    editPointer2 = -1;
                    int pi = editPointer != -1 ? e.findPointerIndex(editPointer) : -1;
                    if (pi >= 0 && editCtl != null) {
                        editOffX = editCtl.bounds.centerX() - e.getX(pi);
                        editOffY = editCtl.bounds.centerY() - e.getY(pi);
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                editPointer = editPointer2 = -1;
                break;
        }
        invalidate();
        return true;
    }

    private float pinchDist(MotionEvent e) {
        int a = e.findPointerIndex(editPointer), b = e.findPointerIndex(editPointer2);
        if (a < 0 || b < 0) return 0;
        float dx = e.getX(a) - e.getX(b), dy = e.getY(a) - e.getY(b);
        return (float) Math.sqrt(dx * dx + dy * dy);
    }

    // ------------------------------------------------------------------ para o runtime

    private void sendPad() {
        if (editMode) return;
        int bits = 0;
        float lt = 0, rt = 0, sx = lx, sy = ly;
        for (Ctl c : ctls) {
            if (c.act == ACT_HOLD && c.isDown()) bits |= c.bits;
            if (c.act == ACT_INVENTORY && c.isDown()) bits |= Y;
        }
        bits |= cDpad.dpadBits;
        if (cRun.latched) bits |= A;
        if (cAim.latched) lt = 1f;
        if (cKnife.latched) bits |= LB;
        long now = SystemClock.uptimeMillis();
        if (cFire.isDown()) {
            if (fireStart != 0) {   // tiro rápido: mira, e atira depois de FIRE_AIM_DELAY_MS
                lt = 1f;
                if (now - fireStart >= FIRE_AIM_DELAY_MS) rt = 1f;
            } else {
                rt = 1f;   // mirando: tiro; faca pronta: facada
            }
        }
        if (reloadStart != 0) {   // recarregar: (mira) + A de 120 a 250 ms; tudo em 350 ms
            long t = now - reloadStart;
            if (t < 350) {
                if (reloadWithAim) lt = 1f;
                if (t >= 120 && t < 250) bits |= A;
            } else {
                reloadStart = 0;
            }
        }
        if (bits == sentButtons && sx == sentLx && sy == sentLy && rx == sentRx && ry == sentRy
                && lt == sentLt && rt == sentRt) return;
        sentButtons = bits;
        sentLx = sx;
        sentLy = sy;
        sentRx = rx;
        sentRy = ry;
        sentLt = lt;
        sentRt = rt;
        try {
            SDLActivity.nativeTouchPad(bits, sx, sy, rx, ry, lt, rt);
        } catch (Throwable ignored) {}
    }

    private void noteTouch() {
        try {
            SDLActivity.nativeTouchNote();   // o runtime marca o toque como dispositivo ativo
        } catch (Throwable ignored) {}
    }

    private void pointer(int action, float x, float y) {
        float w = Math.max(1, getWidth()), h = Math.max(1, getHeight());
        try {
            SDLActivity.nativeTouchPointer(action, x / w, y / h);
        } catch (Throwable ignored) {}
    }

    /** Troca menu <-> jogo: solta o que só existe no gameplay e desliga os alternáveis. */
    private void releaseGameplayOnly() {
        for (Ctl c : ctls) {
            if (c.show == SHOW_GAMEPLAY) c.pointerId = -1;
            if (c.show == SHOW_MENU || c.show == SHOW_MENU_BUTTONS) c.pointerId = -1;
        }
        releaseGameplayLatches();
        reloadStart = 0;
        inventoryMode = false;
        if (lookPointer != -1) lookPointer = -1;
        rx = ry = 0;
        sendPad();
    }

    private void releaseAll() {
        for (Ctl c : ctls) {
            c.pointerId = -1;
            c.dpadBits = 0;
        }
        releaseGameplayLatches();
        reloadStart = 0;
        if (dragPointer != -1) pointer(PTR_UP, dragLastX, dragLastY);
        dragPointer = pendingPointer = -1;
        stickPointer = lookPointer = -1;
        lx = ly = rx = ry = 0;
        resetStickVisual();
        sendPad();
    }

    private void vibrateClick() {
        if (!vibration) return;
        try {
            if (vibrator != null && vibrator.hasVibrator()) {
                if (Build.VERSION.SDK_INT >= 29) {
                    vibrator.vibrate(VibrationEffect.createPredefined(VibrationEffect.EFFECT_TICK));
                } else if (Build.VERSION.SDK_INT >= 26) {
                    vibrator.vibrate(VibrationEffect.createOneShot(12, VibrationEffect.DEFAULT_AMPLITUDE));
                } else {
                    vibrator.vibrate(12);
                }
            } else {
                performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            }
        } catch (Exception ignored) {}
    }

    private float dp(float v) {
        return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }
}
