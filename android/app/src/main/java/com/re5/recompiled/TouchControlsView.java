package com.re5.recompiled;

import android.content.Context;
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
 * {@link SDLActivity#nativeTouchPointer}. O runtime marca a entrada como toque, soma o
 * "controle de toque" ao usuário 0 do XInput (o jogo mostra os ícones do controle - as
 * mesmas letras dos botões daqui - e navega todos os menus pelo analógico) e diz qual
 * dispositivo foi usado por último ({@link SDLActivity#nativeActiveInput}). O toque é o
 * padrão; com um controle ou teclado físico em uso os botões somem, um aviso diz qual
 * dispositivo está em uso, e tudo volta no primeiro toque.
 *
 * Layout do controle no RE5 (tipo D, o padrão): LT mira, RT atira, LB faca, RB mapa,
 * A corre/recarrega, B parceiro, X ação, Y inventário, Back pula a cena, Start pausa,
 * D-pad troca rápida, giro 180 = analógico para baixo + A.
 *
 * Gameplay x menu: o cursor escondido pelo jogo = gameplay (nativeIsCursorHidden). No
 * menu somem os botões só de combate; tocar sem arrastar clica onde o dedo tocou, e
 * arrastar na metade esquerda vira o analógico (navegação).
 */
public class TouchControlsView extends View {

    public interface OnMenuToggleListener {
        void onToggleMenu();
    }

    // XINPUT_GAMEPAD_* (re5/touch.h)
    private static final int DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008;
    private static final int START = 0x0010, BACK = 0x0020, LB = 0x0100, RB = 0x0200;
    private static final int A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
    private static final int TRIG_LT = -1, TRIG_RT = -2, QUICK_TURN = -3;

    // re5_input_source
    private static final int SRC_KBM = 1, SRC_PAD = 2;
    // RE5_TOUCH_PTR_*
    private static final int PTR_DOWN = 0, PTR_MOVE = 1, PTR_UP = 2;

    /** Onde o botão aparece: em todo lugar ou só no gameplay. */
    private static final int SHOW_ALWAYS = 0, SHOW_GAMEPLAY = 1;

    private static final float STICK_DEADZONE = 0.10f;   // fração do raio
    private static final long TICK_MS = 16;

    private OnMenuToggleListener menuToggleListener;

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint subTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF tmpRect = new RectF();

    static final class TouchButton {
        final String label, gameSub, menuSub;
        final int color, bits, show;
        final boolean round, dragLook;
        final RectF bounds = new RectF();
        int pointerId = -1;

        TouchButton(String label, String gameSub, String menuSub, int color, int bits, int show,
                    boolean round, boolean dragLook) {
            this.label = label;
            this.gameSub = gameSub;
            this.menuSub = menuSub;
            this.color = color;
            this.bits = bits;
            this.show = show;
            this.round = round;
            this.dragLook = dragLook;
        }

        boolean isDown() { return pointerId != -1; }
    }

    private final List<TouchButton> buttons = new ArrayList<>();
    private TouchButton btnA, btnB, btnX, btnY, btnLT, btnRT, btnLB, btnRB, btnTurn;
    private TouchButton btnUp, btnDown, btnLeft, btnRight, btnStart, btnBack;

    // Pílulas do topo (do app, não do jogo)
    private final RectF pillMenu = new RectF(), pillHide = new RectF();

    // Analógico esquerdo flutuante
    private int stickPointer = -1;
    private float stickRestX, stickRestY, stickBaseX, stickBaseY, stickKnobX, stickKnobY;
    private float stickRadius, stickKnobRadius;
    private float lx, ly;

    // Câmera: arrasto no lado direito (ou segurando RT) vira o analógico direito
    private int lookPointer = -1;
    private float lookLastX, lookLastY, lookAccX, lookAccY;
    private long lookLastTick;
    private float rx, ry;
    private float lookSensitivity = 1.0f;

    // Toque ainda indefinido (menu): vira clique se soltar parado, ou arrasto
    private int pendingPointer = -1;
    private float pendingX, pendingY;
    private boolean pendingLeft;
    private int dragPointer = -1;   // arrasto do "mouse" no menu
    private float dragLastX, dragLastY;

    // Giro 180: analógico para baixo e depois A
    private long turnStart = 0;

    private boolean inGameplay = false;
    private boolean controlsEnabled = true;
    private boolean hideWithPad = true;
    private boolean hiddenByDevice = false;
    private float controlsOpacity = 0.85f;
    private float controlsScale = 1.0f;

    // Último estado enviado ao runtime (só manda quando muda)
    private int sentButtons = -1;
    private float sentLx, sentLy, sentRx, sentRy, sentLt, sentRt;

    // Aviso de dispositivo (toque / teclado / controle) no alto da tela
    private int lastSource = 0;
    private String notice;
    private long noticeAt = -10000;
    private static final long NOTICE_MS = 3000;

    private Vibrator vibrator;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private final Runnable stateCheck = new Runnable() {
        @Override
        public void run() {
            // Liga o controle de toque já no início: o jogo vê o controle desde a abertura.
            if (sentButtons == -1) sendPad();
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
                // Aviso de troca (a conexão em si o GameActivity avisa com o nome do aparelho).
                if (SystemClock.uptimeMillis() - noticeAt > 2500) {
                    if (src == SRC_PAD) showNotice("🎮 Usando o controle · toque na tela para voltar");
                    else if (src == SRC_KBM) showNotice("⌨ Usando o teclado · toque na tela para voltar");
                    else if (lastSource == SRC_PAD || lastSource == SRC_KBM) showNotice("✋ Usando o toque");
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
            handler.postDelayed(this, 120);
        }
    };

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            updateLook();
            sendPad();
            if (lookPointer != -1 || rx != 0 || ry != 0 || turnStart != 0) {
                handler.postDelayed(this, TICK_MS);
            } else {
                ticking = false;
            }
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

    public void setOnMenuToggleListener(OnMenuToggleListener listener) { menuToggleListener = listener; }

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
        controlsScale = Math.max(0.7f, Math.min(1.4f, scale));
        layoutButtons(getWidth(), getHeight());
        invalidate();
    }

    public void setLookSensitivity(float s) { lookSensitivity = Math.max(0.3f, Math.min(3f, s)); }

    public void setHideWithPad(boolean hide) { hideWithPad = hide; }

    /** Aviso curto no alto da tela (controle/teclado conectado, dispositivo em uso). */
    public void showNotice(String text) {
        notice = text;
        noticeAt = SystemClock.uptimeMillis();
        invalidate();
        handler.postDelayed(this::invalidate, NOTICE_MS + 50);
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

    private TouchButton add(String label, String gameSub, String menuSub, int color, int bits, int show,
                            boolean round, boolean dragLook) {
        TouchButton b = new TouchButton(label, gameSub, menuSub, color, bits, show, round, dragLook);
        buttons.add(b);
        return b;
    }

    private void init() {
        vibrator = (Vibrator) getContext().getSystemService(Context.VIBRATOR_SERVICE);

        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setColor(Color.WHITE);
        textPaint.setFakeBoldText(true);
        subTextPaint.setTextAlign(Paint.Align.CENTER);
        subTextPaint.setColor(0xDDFFFFFF);

        // Cores do controle do Xbox nos botões de face: o jogo mostra as mesmas.
        btnA = add("A", "CORRER", "OK", 0xFF22C55E, A, SHOW_ALWAYS, true, false);
        btnB = add("B", "PARCEIRO", "VOLTAR", 0xFFEF4444, B, SHOW_ALWAYS, true, false);
        btnX = add("X", "AÇÃO", "", 0xFF3B82F6, X, SHOW_ALWAYS, true, false);
        btnY = add("Y", "INVENTÁRIO", "", 0xFFEAB308, Y, SHOW_ALWAYS, true, false);
        btnLT = add("LT", "MIRAR", "", 0xFF94A3B8, TRIG_LT, SHOW_GAMEPLAY, true, false);
        btnRT = add("RT", "ATIRAR", "", 0xFFF97316, TRIG_RT, SHOW_GAMEPLAY, true, true);
        btnLB = add("LB", "FACA", "", 0xFF94A3B8, LB, SHOW_ALWAYS, true, false);
        btnRB = add("RB", "MAPA", "", 0xFF94A3B8, RB, SHOW_ALWAYS, true, false);
        btnTurn = add("180°", "GIRO", "", 0xFF8B5CF6, QUICK_TURN, SHOW_GAMEPLAY, true, false);
        btnUp = add("▲", "", "", 0xFF64748B, DPAD_UP, SHOW_ALWAYS, false, false);
        btnDown = add("▼", "", "", 0xFF64748B, DPAD_DOWN, SHOW_ALWAYS, false, false);
        btnLeft = add("◀", "", "", 0xFF64748B, DPAD_LEFT, SHOW_ALWAYS, false, false);
        btnRight = add("▶", "", "", 0xFF64748B, DPAD_RIGHT, SHOW_ALWAYS, false, false);
        btnBack = add("PULAR", "BACK", "BACK", 0xFF64748B, BACK, SHOW_ALWAYS, false, false);
        btnStart = add("PAUSA", "START", "START", 0xFF64748B, START, SHOW_ALWAYS, false, false);
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
        layoutButtons(w, h);
    }

    private void circle(TouchButton b, float cx, float cy, float r) {
        b.bounds.set(cx - r, cy - r, cx + r, cy + r);
    }

    private void layoutButtons(int w, int h) {
        if (w <= 0 || h <= 0) return;
        float s = controlsScale;

        stickRadius = dp(62) * s;
        stickKnobRadius = dp(26) * s;
        stickRestX = dp(115) * s;
        stickRestY = h - dp(110) * s;
        if (stickPointer == -1) resetStickVisual();

        // Topo: painel do app no centro, ocultar à direita; Back e Start ao lado.
        float topY = dp(12);
        pillMenu.set(w / 2f - dp(52), topY, w / 2f + dp(52), topY + dp(32));
        pillHide.set(w - dp(50), topY, w - dp(14), topY + dp(32));
        btnStart.bounds.set(w - dp(136), topY, w - dp(60), topY + dp(32));
        btnBack.bounds.set(w - dp(222), topY, w - dp(146), topY + dp(32));

        // Face (losango do controle): Y em cima, B à direita, A embaixo, X à esquerda.
        float fr = dp(29) * s, gap = dp(60) * s;
        float fcx = w - dp(118) * s, fcy = h - dp(118) * s;
        circle(btnA, fcx, fcy + gap, dp(32) * s);
        circle(btnB, fcx + gap, fcy, fr);
        circle(btnX, fcx - gap, fcy, fr);
        circle(btnY, fcx, fcy - gap, fr);

        // Gatilhos: RT (atirar) acima do losango; arrastar a partir dele também mira.
        circle(btnRT, w - dp(62) * s, fcy - gap - dp(78) * s, dp(36) * s);
        circle(btnRB, w - dp(150) * s, fcy - gap - dp(92) * s, dp(25) * s);
        circle(btnTurn, fcx - gap - dp(82) * s, h - dp(44) * s, dp(24) * s);

        // LT (mirar) e LB (faca) acima do analógico: no RE5 não se anda mirando, o
        // polegar esquerdo fica livre para segurar a mira.
        circle(btnLT, dp(62) * s, stickRestY - stickRadius - dp(78) * s, dp(34) * s);
        circle(btnLB, dp(150) * s, stickRestY - stickRadius - dp(92) * s, dp(25) * s);

        // D-pad (troca rápida de arma; navegação nos menus) embaixo, entre os dois lados.
        float dsz = dp(23) * s, dcx = w * 0.5f - dp(120) * s, dcy = h - dp(62) * s, dgap = dp(27) * s;
        btnUp.bounds.set(dcx - dsz, dcy - dgap - dsz, dcx + dsz, dcy - dgap + dsz * 0.6f);
        btnDown.bounds.set(dcx - dsz, dcy + dgap - dsz * 0.6f, dcx + dsz, dcy + dgap + dsz);
        btnLeft.bounds.set(dcx - dgap - dsz * 1.3f, dcy - dsz * 0.8f, dcx - dgap + dsz * 0.6f, dcy + dsz * 0.8f);
        btnRight.bounds.set(dcx + dgap - dsz * 0.6f, dcy - dsz * 0.8f, dcx + dgap + dsz * 1.3f, dcy + dsz * 0.8f);
    }

    private void resetStickVisual() {
        stickBaseX = stickKnobX = stickRestX;
        stickBaseY = stickKnobY = stickRestY;
    }

    private boolean visible(TouchButton b) {
        if (!controlsEnabled || hiddenByDevice) return false;
        return b.show == SHOW_ALWAYS || inGameplay;
    }

    // ------------------------------------------------------------------ desenho

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        drawPill(canvas, pillMenu, "⚙ PAINEL", 0xFF3B82F6, false);
        drawPill(canvas, pillHide, controlsEnabled ? "🎮" : "✋", 0xFF64748B, false);
        drawNotice(canvas);
        if (!controlsEnabled || hiddenByDevice) return;

        float mod = inGameplay ? 1f : 0.8f;
        drawStick(canvas, inGameplay ? 1f : 0.55f);
        for (TouchButton b : buttons) {
            if (!visible(b)) continue;
            if (b == btnStart || b == btnBack) {
                drawPill(canvas, b.bounds, b.label, b.color, b.isDown());
            } else if (b.round) {
                drawRound(canvas, b, mod);
            } else {
                drawRect(canvas, b, mod);
            }
        }
        if (!inGameplay) drawMenuHint(canvas);
    }

    private void drawMenuHint(Canvas canvas) {
        textPaint.setTextSize(dp(11));
        String t = "Toque para escolher · arraste à esquerda para navegar";
        float tw = textPaint.measureText(t);
        float cx = getWidth() / 2f, y = dp(62);
        tmpRect.set(cx - tw / 2 - dp(12), y - dp(15), cx + tw / 2 + dp(12), y + dp(8));
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0x990F172A);
        canvas.drawRoundRect(tmpRect, dp(12), dp(12), paint);
        textPaint.setColor(0xEEFFFFFF);
        canvas.drawText(t, cx, y, textPaint);
    }

    private int alpha(float a) { return Math.max(0, Math.min(255, (int) (a * 255))); }

    private void drawStick(Canvas canvas, float mod) {
        float m = controlsOpacity * mod;
        boolean on = stickPointer != -1;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(alpha(0.20f * m) << 24 | 0x1E293B);
        canvas.drawCircle(stickBaseX, stickBaseY, stickRadius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(2));
        paint.setColor(alpha(0.40f * m) << 24 | 0x38BDF8);
        canvas.drawCircle(stickBaseX, stickBaseY, stickRadius, paint);
        // zona morta
        paint.setStrokeWidth(dp(1));
        paint.setColor(alpha(0.20f * m) << 24 | 0xFFFFFF);
        canvas.drawCircle(stickBaseX, stickBaseY, stickRadius * STICK_DEADZONE + stickKnobRadius * 0.2f, paint);

        paint.setStyle(Paint.Style.FILL);
        paint.setColor(alpha((on ? 0.80f : 0.45f) * m) << 24 | (on ? 0x0284C7 : 0x1E293B));
        canvas.drawCircle(stickKnobX, stickKnobY, stickKnobRadius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(2));
        paint.setColor(alpha(0.9f * m) << 24 | 0xFFFFFF);
        canvas.drawCircle(stickKnobX, stickKnobY, stickKnobRadius, paint);
    }

    private void drawPill(Canvas canvas, RectF r, String label, int color, boolean down) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(down ? 0xCC000000 : 0x770F172A);
        canvas.drawRoundRect(r, dp(16), dp(16), paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1.2f));
        paint.setColor(down ? Color.WHITE : color);
        canvas.drawRoundRect(r, dp(16), dp(16), paint);
        textPaint.setTextSize(dp(11));
        textPaint.setColor(Color.WHITE);
        float ty = r.centerY() - (textPaint.descent() + textPaint.ascent()) / 2;
        canvas.drawText(label, r.centerX(), ty, textPaint);
    }

    private void drawRound(Canvas canvas, TouchButton b, float mod) {
        float r = b.bounds.width() / 2f, cx = b.bounds.centerX(), cy = b.bounds.centerY();
        float m = controlsOpacity * mod;
        boolean down = b.isDown();
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(alpha((down ? 0.80f : 0.30f) * m) << 24 | (b.color & 0xFFFFFF));
        canvas.drawCircle(cx, cy, r, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(down ? dp(2.8f) : dp(1.6f));
        paint.setColor(alpha((down ? 1f : 0.65f) * m) << 24 | (down ? 0xFFFFFF : b.color & 0xFFFFFF));
        canvas.drawCircle(cx, cy, r, paint);

        String sub = inGameplay ? b.gameSub : b.menuSub;
        textPaint.setColor(alpha(Math.min(1f, m + 0.15f)) << 24 | 0xFFFFFF);
        textPaint.setTextSize(r > dp(30) ? dp(17) : dp(14));
        if (sub != null && !sub.isEmpty()) {
            canvas.drawText(b.label, cx, cy + dp(1), textPaint);
            subTextPaint.setTextSize(dp(r > dp(26) ? 8.5f : 7.5f));
            subTextPaint.setColor(alpha(Math.min(1f, m + 0.1f)) << 24 | 0xFFFFFF);
            canvas.drawText(sub, cx, cy + dp(13), subTextPaint);
        } else {
            canvas.drawText(b.label, cx, cy - (textPaint.descent() + textPaint.ascent()) / 2, textPaint);
        }
    }

    private void drawRect(Canvas canvas, TouchButton b, float mod) {
        float m = controlsOpacity * mod;
        boolean down = b.isDown();
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(alpha((down ? 0.80f : 0.30f) * m) << 24 | (b.color & 0xFFFFFF));
        canvas.drawRoundRect(b.bounds, dp(7), dp(7), paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1.4f));
        paint.setColor(alpha((down ? 1f : 0.6f) * m) << 24 | 0xFFFFFF);
        canvas.drawRoundRect(b.bounds, dp(7), dp(7), paint);
        textPaint.setTextSize(dp(12));
        textPaint.setColor(alpha(Math.min(1f, m + 0.15f)) << 24 | 0xFFFFFF);
        canvas.drawText(b.label, b.bounds.centerX(), b.bounds.centerY() - (textPaint.descent() + textPaint.ascent()) / 2, textPaint);
    }

    // ------------------------------------------------------------------ toque

    @Override
    public boolean onTouchEvent(MotionEvent e) {
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

    private TouchButton hit(float x, float y) {
        TouchButton best = null;
        float bestD = Float.MAX_VALUE;
        for (TouchButton b : buttons) {
            if (!visible(b)) continue;
            float cx = b.bounds.centerX(), cy = b.bounds.centerY();
            float d;
            if (b.round) {
                float r = b.bounds.width() / 2f * 1.15f;   // folga para o polegar
                float dx = x - cx, dy = y - cy;
                d = dx * dx + dy * dy;
                if (d > r * r) continue;
            } else {
                tmpRect.set(b.bounds);
                tmpRect.inset(-dp(5), -dp(5));
                if (!tmpRect.contains(x, y)) continue;
                d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            }
            if (d < bestD) {
                bestD = d;
                best = b;
            }
        }
        return best;
    }

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
        // Voltou para o toque depois do controle físico: os botões reaparecem já neste toque.
        hiddenByDevice = false;
        if (!controlsEnabled) {
            // Sem botões: a tela inteira é toque direto (clique onde tocar).
            startPending(id, x, y, false);
            return;
        }

        TouchButton b = hit(x, y);
        if (b != null) {
            b.pointerId = id;
            vibrateClick();
            if (b == btnTurn) {
                turnStart = SystemClock.uptimeMillis();
                startTick();
            }
            if (b.dragLook && lookPointer == -1) startLook(id, x, y);
            return;
        }

        if (inGameplay) {
            if (x < getWidth() * 0.45f) {
                if (stickPointer == -1) startStick(id, x, y);
            } else if (lookPointer == -1) {
                startLook(id, x, y);
            }
            return;
        }
        // Menu: só decide depois (toque parado = clique; arrasto à esquerda = analógico).
        startPending(id, x, y, x < getWidth() * 0.45f);
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
        // A base vai para onde o dedo tocou, sem passar da borda da tela.
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
        if (id == stickPointer) {
            updateStick(x, y);
        }
        if (id == lookPointer) {
            lookAccX += x - lookLastX;
            lookAccY += y - lookLastY;
            lookLastX = x;
            lookLastY = y;
        }
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
        for (TouchButton b : buttons) if (b.pointerId == id) b.pointerId = -1;
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
            // Passou da borda: a base acompanha o dedo (não "trava" o analógico).
            float over = dist - stickRadius;
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
        if (lookPointer == -1) tx = ty = 0;
        // suaviza: subir rápido, descer rápido também (o dedo parado para a câmera)
        rx += (tx - rx) * 0.6f;
        ry += (ty - ry) * 0.6f;
        if (Math.abs(rx) < 0.01f) rx = 0;
        if (Math.abs(ry) < 0.01f) ry = 0;
    }

    private float lookCurve(float v) {
        float a = Math.abs(v) * lookSensitivity;
        if (a < 0.015f) return 0;
        // 0,22 = logo acima da zona morta do jogo; deflexão total a ~1 dp/ms.
        float out = 0.22f + 0.78f * Math.min(1f, a / 1.0f);
        return Math.signum(v) * out;
    }

    private void startTick() {
        if (ticking) return;
        ticking = true;
        handler.postDelayed(tick, TICK_MS);
    }

    // ------------------------------------------------------------------ para o runtime

    private void sendPad() {
        int bits = 0;
        float lt = 0, rt = 0, sx = lx, sy = ly;
        for (TouchButton b : buttons) {
            if (!b.isDown()) continue;
            if (b.bits > 0) bits |= b.bits;
            else if (b.bits == TRIG_LT) lt = 1f;
            else if (b.bits == TRIG_RT) rt = 1f;
        }
        if (turnStart != 0) {   // giro 180: analógico para baixo, A depois de 50 ms, tudo por 200 ms
            long t = SystemClock.uptimeMillis() - turnStart;
            if (t < 200) {
                sx = 0;
                sy = -1f;
                if (t >= 50) bits |= A;
            } else {
                turnStart = 0;
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

    /** Solta o que só existe no gameplay (troca menu <-> jogo). */
    private void releaseGameplayOnly() {
        for (TouchButton b : buttons) if (b.show == SHOW_GAMEPLAY) b.pointerId = -1;
        if (lookPointer != -1) lookPointer = -1;
        rx = ry = 0;
        sendPad();
    }

    private void releaseAll() {
        for (TouchButton b : buttons) b.pointerId = -1;
        if (dragPointer != -1) pointer(PTR_UP, dragLastX, dragLastY);
        dragPointer = pendingPointer = -1;
        stickPointer = lookPointer = -1;
        lx = ly = rx = ry = 0;
        turnStart = 0;
        resetStickVisual();
        sendPad();
    }

    private void vibrateClick() {
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
