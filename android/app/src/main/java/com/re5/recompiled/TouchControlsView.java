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
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.List;

/**
 * TouchControlsView - Camada moderna e glassmórfica de controles táteis virtuais para Resident Evil 5.
 * 
 * Funcionalidades:
 * - Botões de ação aparecem APENAS durante o gameplay (cursor escondido no runtime).
 * - Nos menus (título, pausa, inventário, opções), botões de ação somem para liberar a tela.
 * - Toque direto com o dedo nos menus para clicar nos botões do jogo.
 * - Analógico virtual navega nos menus com D-Pad (Up/Down/Left/Right) com repetição suave.
 * - No gameplay, analógico move o personagem com WASD e o lado direito move a mira com trackpad relativo.
 * - Todos os botões essenciais: Mirar, Atirar, Ação/Melee, Recarregar, Faca, Corrida/Sprint,
 *   Giro Rápido 180°, Mapa/GPS, Atalhos Rápidos 1-2-3-4, Inventário, Parceiro, Pausa.
 */
public class TouchControlsView extends View {

    public interface OnMenuToggleListener {
        void onToggleMenu();
    }

    private OnMenuToggleListener menuToggleListener;

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint subTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    // Joystick virtual
    private boolean joystickActive = false;
    private int joystickPointerId = -1;
    private float joystickBaseX, joystickBaseY;
    private float joystickKnobX, joystickKnobY;
    private float joystickRadius;
    private float joystickKnobRadius;

    // Estado do analógico em Gameplay (WASD)
    private boolean keyW = false;
    private boolean keyA = false;
    private boolean keyS = false;
    private boolean keyD = false;

    // Estado do analógico em Menus (D-Pad)
    private int activeMenuDpadKey = 0;
    private long lastDpadRepeatTime = 0;
    private static final long DPAD_INITIAL_DELAY_MS = 320;
    private static final long DPAD_REPEAT_INTERVAL_MS = 180;

    // Trackpad de mira (área direita durante gameplay)
    private int lookPointerId = -1;
    private float lastLookX, lastLookY;

    // Toque no menu (clique com o dedo)
    private int menuTouchPointerId = -1;

    // Verificação periódica de estado de Menu vs Gameplay
    private final Handler stateCheckHandler = new Handler(Looper.getMainLooper());
    private boolean inGameplay = false; // true = gameplay 3D, false = menus/inventário/pausa

    // Configurações personalizáveis
    private boolean controlsEnabled = true;
    private float controlsOpacity = 0.85f;
    private float controlsScale = 1.0f;

    // Estrutura de botão de toque
    public static class TouchButton {
        final String id;
        final String label;
        final String subLabel;
        final int color;
        final int keycode;
        final int mouseButton; // 0 = nenhum, 1 = esquerdo, 3 = direito
        final boolean isQuickTurn;
        RectF bounds = new RectF();
        boolean isDown = false;
        int activePointerId = -1;

        TouchButton(String id, String label, String subLabel, int color, int keycode, int mouseButton) {
            this(id, label, subLabel, color, keycode, mouseButton, false);
        }

        TouchButton(String id, String label, String subLabel, int color, int keycode, int mouseButton, boolean isQuickTurn) {
            this.id = id;
            this.label = label;
            this.subLabel = subLabel;
            this.color = color;
            this.keycode = keycode;
            this.mouseButton = mouseButton;
            this.isQuickTurn = isQuickTurn;
        }
    }

    private final List<TouchButton> actionButtons = new ArrayList<>();
    private final List<TouchButton> quickSlots = new ArrayList<>();
    private final TouchButton btnMenu = new TouchButton("MENU", "⚙", "PAINEL", 0xFF3B82F6, 0, 0);
    private final TouchButton btnHideControls = new TouchButton("HIDE", "🎮", "", 0xFF64748B, 0, 0);

    private Vibrator vibrator;

    private final Runnable stateCheckRunnable = new Runnable() {
        @Override
        public void run() {
            boolean wasInGameplay = inGameplay;
            try {
                // nativeIsCursorHidden() é true quando o jogo escondeu o cursor (gameplay)
                inGameplay = SDLActivity.nativeIsCursorHidden();
            } catch (Throwable ignored) {
                inGameplay = true;
            }

            if (wasInGameplay != inGameplay) {
                if (!inGameplay) {
                    // Saiu do gameplay e entrou em menu: solta teclas WASD e mira
                    resetGameplayInputs();
                }
                invalidate();
            }

            // Atualiza repetição do D-Pad se segurado no menu
            if (!inGameplay && joystickActive && activeMenuDpadKey != 0) {
                long now = SystemClock.uptimeMillis();
                if (now - lastDpadRepeatTime >= DPAD_REPEAT_INTERVAL_MS) {
                    SDLActivity.onNativeKeyDown(activeMenuDpadKey);
                    SDLActivity.onNativeKeyUp(activeMenuDpadKey);
                    lastDpadRepeatTime = now;
                }
            }

            stateCheckHandler.postDelayed(this, 120);
        }
    };

    public TouchControlsView(Context context) {
        super(context);
        init();
    }

    public TouchControlsView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    public void setOnMenuToggleListener(OnMenuToggleListener listener) {
        this.menuToggleListener = listener;
    }

    public void setControlsEnabled(boolean enabled) {
        this.controlsEnabled = enabled;
        invalidate();
    }

    public void setControlsOpacity(float opacity) {
        this.controlsOpacity = Math.max(0.2f, Math.min(1.0f, opacity));
        invalidate();
    }

    public void setControlsScale(float scale) {
        this.controlsScale = Math.max(0.7f, Math.min(1.4f, scale));
        layoutButtons(getWidth(), getHeight());
        invalidate();
    }

    private void init() {
        vibrator = getContext() != null ? (Vibrator) getContext().getSystemService(Context.VIBRATOR_SERVICE) : null;
        joystickRadius = dp(65);
        joystickKnobRadius = dp(28);

        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setColor(Color.WHITE);
        textPaint.setFakeBoldText(true);

        subTextPaint.setTextAlign(Paint.Align.CENTER);
        subTextPaint.setColor(0xCCFFFFFF);
        subTextPaint.setFakeBoldText(false);

        // 1. Mirar (LT / Botão direito do mouse)
        actionButtons.add(new TouchButton("AIM", "MIRA", "LT", 0xFF3B82F6, 0, 3));
        // 2. Atirar / Ataque (RT / Botão esquerdo do mouse)
        actionButtons.add(new TouchButton("FIRE", "TIRO", "RT", 0xFFEF4444, 0, 1));
        // 3. Ação / Pegar / Melee (F)
        actionButtons.add(new TouchButton("ACTION", "AÇÃO", "F", 0xFF10B981, KeyEvent.KEYCODE_F, 0));
        // 4. Recarregar (R)
        actionButtons.add(new TouchButton("RELOAD", "RECARREGAR", "R", 0xFFF59E0B, KeyEvent.KEYCODE_R, 0));
        // 5. Correr / Sprint (Shift / A no controle)
        actionButtons.add(new TouchButton("SPRINT", "CORRER", "SHIFT", 0xFF06B6D4, KeyEvent.KEYCODE_SHIFT_LEFT, 0));
        // 6. Giro Rápido 180° (S + Space / Quick Turn)
        actionButtons.add(new TouchButton("180", "180°", "GIRO", 0xFF8B5CF6, 0, 0, true));
        // 7. Faca (Barra de espaço)
        actionButtons.add(new TouchButton("KNIFE", "FACA", "SPACE", 0xFFEC4899, KeyEvent.KEYCODE_SPACE, 0));
        // 8. Mapa / GPS (M)
        actionButtons.add(new TouchButton("MAP", "MAPA", "M", 0xFF6366F1, KeyEvent.KEYCODE_M, 0));
        // 9. Inventário (E)
        actionButtons.add(new TouchButton("INV", "INV", "E", 0xFFA855F7, KeyEvent.KEYCODE_E, 0));
        // 10. Chamar Parceiro (Q / V)
        actionButtons.add(new TouchButton("PARTNER", "PARCEIRO", "Q", 0xFF14B8A6, KeyEvent.KEYCODE_Q, 0));
        // 11. Pausa (ESC)
        actionButtons.add(new TouchButton("PAUSE", "PAUSA", "ESC", 0xFF475569, KeyEvent.KEYCODE_ESCAPE, 0));
        // 12. Confirmar / Enter (Enter)
        actionButtons.add(new TouchButton("ENTER", "OK", "ENTER", 0xFF10B981, KeyEvent.KEYCODE_ENTER, 0));

        // Atalhos Rápidos de Armas (1, 2, 3, 4)
        quickSlots.add(new TouchButton("SLOT1", "1", "ARMA", 0xFF38BDF8, KeyEvent.KEYCODE_1, 0));
        quickSlots.add(new TouchButton("SLOT2", "2", "ARMA", 0xFF38BDF8, KeyEvent.KEYCODE_2, 0));
        quickSlots.add(new TouchButton("SLOT3", "3", "ARMA", 0xFF38BDF8, KeyEvent.KEYCODE_3, 0));
        quickSlots.add(new TouchButton("SLOT4", "4", "ARMA", 0xFF38BDF8, KeyEvent.KEYCODE_4, 0));
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        stateCheckHandler.post(stateCheckRunnable);
    }

    @Override
    protected void onDetachedFromWindow() {
        stateCheckHandler.removeCallbacks(stateCheckRunnable);
        super.onDetachedFromWindow();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        layoutButtons(w, h);
    }

    private void layoutButtons(int w, int h) {
        if (w <= 0 || h <= 0) return;

        float s = controlsScale;
        joystickRadius = dp(65) * s;
        joystickKnobRadius = dp(28) * s;

        // Base do joystick: canto inferior esquerdo com margem confortável
        joystickBaseX = dp(105) * s;
        joystickBaseY = h - dp(105) * s;
        joystickKnobX = joystickBaseX;
        joystickKnobY = joystickBaseY;

        // Pílulas de topo (Menu do launcher e Ocultar controles)
        float topY = dp(14);
        btnMenu.bounds.set(w / 2f - dp(56), topY, w / 2f + dp(56), topY + dp(32));
        btnHideControls.bounds.set(w - dp(48), topY, w - dp(14), topY + dp(32));

        // Botão de Pausa (ESC) no topo direito
        findButton("PAUSE").bounds.set(w - dp(100), topY, w - dp(54), topY + dp(32));

        // Botões de Ação no quadrante inferior direito (disposição ergonômica em arco)
        float rightMargin = dp(28) * s;
        float bottomMargin = dp(28) * s;

        // 1. TIRO (Principal, grande)
        float fireR = dp(38) * s;
        float fireCX = w - rightMargin - fireR - dp(45) * s;
        float fireCY = h - bottomMargin - fireR - dp(45) * s;
        findButton("FIRE").bounds.set(fireCX - fireR, fireCY - fireR, fireCX + fireR, fireCY + fireR);

        // 2. MIRA (Ao lado esquerdo do Tiro)
        float aimR = dp(34) * s;
        float aimCX = fireCX - dp(80) * s;
        float aimCY = fireCY - dp(24) * s;
        findButton("AIM").bounds.set(aimCX - aimR, aimCY - aimR, aimCX + aimR, aimCY + aimR);

        // 3. AÇÃO / PEGAR (Acima do Tiro)
        float actR = dp(30) * s;
        float actCX = fireCX + dp(15) * s;
        float actCY = fireCY - dp(85) * s;
        findButton("ACTION").bounds.set(actCX - actR, actCY - actR, actCX + actR, actCY + actR);

        // 4. RECARREGAR (À direita do Tiro)
        float relR = dp(26) * s;
        float relCX = fireCX + dp(65) * s;
        float relCY = fireCY + dp(10) * s;
        findButton("RELOAD").bounds.set(relCX - relR, relCY - relR, relCX + relR, relCY + relR);

        // 5. CORRER / SPRINT (Abaixo da Mira)
        float sprR = dp(28) * s;
        float sprCX = aimCX - dp(20) * s;
        float sprCY = fireCY + dp(55) * s;
        findButton("SPRINT").bounds.set(sprCX - sprR, sprCY - sprR, sprCX + sprR, sprCY + sprR);

        // 6. GIRO 180° (Próximo à corrida)
        float turnR = dp(24) * s;
        float turnCX = sprCX + dp(55) * s;
        float turnCY = sprCY + dp(10) * s;
        findButton("180").bounds.set(turnCX - turnR, turnCY - turnR, turnCX + turnR, turnCY + turnR);

        // 7. FACA (Acima da Mira)
        float knfR = dp(26) * s;
        float knfCX = aimCX - dp(35) * s;
        float knfCY = aimCY - dp(60) * s;
        findButton("KNIFE").bounds.set(knfCX - knfR, knfCY - knfR, knfCX + knfR, knfCY + knfR);

        // 8. INVENTÁRIO (E)
        float invR = dp(26) * s;
        float invCX = actCX - dp(55) * s;
        float invCY = actCY - dp(50) * s;
        findButton("INV").bounds.set(invCX - invR, invCY - invR, invCX + invR, invCY + invR);

        // 9. PARCEIRO (Q)
        float partR = dp(26) * s;
        float partCX = invCX - dp(60) * s;
        float partCY = invCY + dp(8) * s;
        findButton("PARTNER").bounds.set(partCX - partR, partCY - partR, partCX + partR, partCY + partR);

        // 10. MAPA / GPS
        float mapR = dp(24) * s;
        float mapCX = partCX - dp(55) * s;
        float mapCY = partCY + dp(10) * s;
        findButton("MAP").bounds.set(mapCX - mapR, mapCY - mapR, mapCX + mapR, mapCY + mapR);

        // 11. CONFIRMAR / OK
        float entR = dp(24) * s;
        float entCX = actCX + dp(50) * s;
        float entCY = actCY - dp(45) * s;
        findButton("ENTER").bounds.set(entCX - entR, entCY - entR, entCX + entR, entCY + entR);

        // Atalhos Rápidos de Armas (1, 2, 3, 4) - Faixa elegante acima do analógico esquerdo
        float slotW = dp(36) * s;
        float slotH = dp(32) * s;
        float slotY = joystickBaseY - joystickRadius - dp(45) * s;
        float slotStartX = dp(24);
        for (int i = 0; i < quickSlots.size(); i++) {
            float sx = slotStartX + i * (slotW + dp(8) * s);
            quickSlots.get(i).bounds.set(sx, slotY, sx + slotW, slotY + slotH);
        }
    }

    private TouchButton findButton(String id) {
        for (TouchButton b : actionButtons) {
            if (id.equals(b.id)) return b;
        }
        return actionButtons.get(0);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        // Sempre desenha pílula de menu e botão de ocultar no topo
        drawTopPill(canvas, btnMenu);
        drawTopPill(canvas, btnHideControls);

        if (!controlsEnabled) return;

        // Se NÃO estamos em gameplay (estamos em Menus/Inventário/Pausa):
        // Os botões de ação e slots rápidos são OCULTADOS para o usuário clicar com o dedo!
        // O analógico virtual permanece desenhado sutilmente para navegação no menu.
        if (!inGameplay) {
            drawMenuHint(canvas);
            drawVirtualJoystick(canvas, 0.45f); // 45% opacidade no menu
            return;
        }

        // --- MODO GAMEPLAY ---
        // Desenha o Analógico Virtual completo
        drawVirtualJoystick(canvas, 1.0f);

        // Desenha os Slots Rápidos de Armas (1, 2, 3, 4)
        for (TouchButton slot : quickSlots) {
            drawQuickSlot(canvas, slot);
        }

        // Desenha os Botões de Ação Ergonômicos
        for (TouchButton b : actionButtons) {
            drawActionButton(canvas, b);
        }
    }

    private void drawMenuHint(Canvas canvas) {
        // Indicador sutil e moderno no topo informando modo menu
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0x800F172A);
        float hw = getWidth() / 2f;
        RectF r = new RectF(hw - dp(140), dp(14), hw - dp(65), dp(46));
        canvas.drawRoundRect(r, dp(16), dp(16), paint);

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1));
        paint.setColor(0x40FFFFFF);
        canvas.drawRoundRect(r, dp(16), dp(16), paint);

        textPaint.setTextSize(dp(11));
        textPaint.setColor(0xEEFFFFFF);
        canvas.drawText("Toque na tela p/ clicar", r.centerX(), r.centerY() + dp(4), textPaint);
    }

    private void drawVirtualJoystick(Canvas canvas, float opacityMod) {
        float alphaMult = controlsOpacity * opacityMod;
        int baseFillAlpha = (int) (0x33 * alphaMult);
        int baseStrokeAlpha = (int) (0x66 * alphaMult);
        int knobFillAlpha = (int) ((joystickActive ? 0xCC : 0x77) * alphaMult);
        int knobStrokeAlpha = (int) (0xEE * alphaMult);

        // Anel base exterior com efeito glassmorphism
        paint.setStyle(Paint.Style.FILL);
        paint.setColor((baseFillAlpha << 24) | 0x001E293B);
        canvas.drawCircle(joystickBaseX, joystickBaseY, joystickRadius, paint);

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(2));
        paint.setColor((baseStrokeAlpha << 24) | 0x0038BDF8);
        canvas.drawCircle(joystickBaseX, joystickBaseY, joystickRadius, paint);

        // Indicadores direcionais sutis
        paint.setColor((int) (0x40 * alphaMult) << 24 | 0x00FFFFFF);
        paint.setStrokeWidth(dp(1.2f));
        canvas.drawLine(joystickBaseX - joystickRadius + dp(6), joystickBaseY, joystickBaseX - dp(12), joystickBaseY, paint);
        canvas.drawLine(joystickBaseX + dp(12), joystickBaseY, joystickBaseX + joystickRadius - dp(6), joystickBaseY, paint);
        canvas.drawLine(joystickBaseX, joystickBaseY - joystickRadius + dp(6), joystickBaseX, joystickBaseY - dp(12), paint);
        canvas.drawLine(joystickBaseX, joystickBaseY + dp(12), joystickBaseX, joystickBaseY + joystickRadius - dp(6), paint);

        // Knob central metálico com glow
        paint.setStyle(Paint.Style.FILL);
        paint.setColor((knobFillAlpha << 24) | (joystickActive ? 0x000284C7 : 0x001E293B));
        canvas.drawCircle(joystickKnobX, joystickKnobY, joystickKnobRadius, paint);

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(2.2f));
        paint.setColor((knobStrokeAlpha << 24) | 0x00FFFFFF);
        canvas.drawCircle(joystickKnobX, joystickKnobY, joystickKnobRadius, paint);

        // Ponto central do knob
        paint.setStyle(Paint.Style.FILL);
        paint.setColor((int) (0xAA * alphaMult) << 24 | 0x00FFFFFF);
        canvas.drawCircle(joystickKnobX, joystickKnobY, dp(4), paint);
    }

    private void drawTopPill(Canvas canvas, TouchButton b) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(b.isDown ? 0xB3000000 : 0x660F172A);
        canvas.drawRoundRect(b.bounds, dp(16), dp(16), paint);

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1.2f));
        paint.setColor(b.isDown ? Color.WHITE : b.color);
        canvas.drawRoundRect(b.bounds, dp(16), dp(16), paint);

        textPaint.setTextSize(dp(11));
        textPaint.setColor(Color.WHITE);
        float textY = b.bounds.centerY() - ((textPaint.descent() + textPaint.ascent()) / 2);
        canvas.drawText(b.label, b.bounds.centerX(), textY, textPaint);
    }

    private void drawQuickSlot(Canvas canvas, TouchButton b) {
        int fillAlpha = (int) ((b.isDown ? 0xDD : 0x55) * controlsOpacity);
        int strokeAlpha = (int) ((b.isDown ? 0xFF : 0x88) * controlsOpacity);

        paint.setStyle(Paint.Style.FILL);
        paint.setColor((fillAlpha << 24) | (b.color & 0x00FFFFFF));
        canvas.drawRoundRect(b.bounds, dp(8), dp(8), paint);

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1.4f));
        paint.setColor((strokeAlpha << 24) | (b.isDown ? 0x00FFFFFF : b.color));
        canvas.drawRoundRect(b.bounds, dp(8), dp(8), paint);

        textPaint.setTextSize(dp(12));
        textPaint.setColor(Color.WHITE);
        float textY = b.bounds.centerY() - ((textPaint.descent() + textPaint.ascent()) / 2);
        canvas.drawText(b.label, b.bounds.centerX(), textY, textPaint);
    }

    private void drawActionButton(Canvas canvas, TouchButton b) {
        float r = b.bounds.width() / 2f;
        float cx = b.bounds.centerX();
        float cy = b.bounds.centerY();

        int fillAlpha = (int) ((b.isDown ? 0xCC : 0x48) * controlsOpacity);
        int strokeAlpha = (int) ((b.isDown ? 0xFF : 0x88) * controlsOpacity);

        // Fundo circular com visual moderno e translúcido
        paint.setStyle(Paint.Style.FILL);
        paint.setColor((fillAlpha << 24) | (b.color & 0x00FFFFFF));
        canvas.drawCircle(cx, cy, r, paint);

        // Borda destacada com glow sutil quando pressionado
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(b.isDown ? dp(2.8f) : dp(1.6f));
        paint.setColor((strokeAlpha << 24) | (b.isDown ? 0x00FFFFFF : b.color));
        canvas.drawCircle(cx, cy, r, paint);

        // Rótulos de texto
        float baseTextSize = r > dp(32) ? 12.5f : r > dp(25) ? 10.5f : 9.0f;
        textPaint.setTextSize(dp(baseTextSize));
        textPaint.setColor(Color.WHITE);

        if (b.subLabel != null && !b.subLabel.isEmpty()) {
            float textY = cy - dp(2.5f);
            canvas.drawText(b.label, cx, textY, textPaint);

            subTextPaint.setTextSize(dp(Math.max(7.5f, baseTextSize - 3.5f)));
            canvas.drawText(b.subLabel, cx, cy + dp(9f), subTextPaint);
        } else {
            float textY = cy - ((textPaint.descent() + textPaint.ascent()) / 2);
            canvas.drawText(b.label, cx, textY, textPaint);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int actionIndex = event.getActionIndex();
        int pointerId = event.getPointerId(actionIndex);
        float x = event.getX(actionIndex);
        float y = event.getY(actionIndex);

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                handlePointerDown(pointerId, x, y);
                break;

            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); i++) {
                    int pId = event.getPointerId(i);
                    handlePointerMove(pId, event.getX(i), event.getY(i));
                }
                break;

            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_UP:
                handlePointerUp(pointerId);
                break;

            case MotionEvent.ACTION_CANCEL:
                resetAllInputs();
                break;
        }

        invalidate();
        return true;
    }

    private void handlePointerDown(int pointerId, float x, float y) {
        // 1. Checa botões de Topo fixos (Menu e Ocultar)
        if (btnMenu.bounds.contains(x, y)) {
            vibrateClick();
            if (menuToggleListener != null) menuToggleListener.onToggleMenu();
            return;
        }

        if (btnHideControls.bounds.contains(x, y)) {
            vibrateClick();
            controlsEnabled = !controlsEnabled;
            invalidate();
            return;
        }

        if (!controlsEnabled) return;

        // 2. Se ESTAMOS EM MENU (não em gameplay):
        // Permite clique direto com o dedo em qualquer item do menu!
        // O analógico no lado esquerdo inferior continua disponível para navegar.
        if (!inGameplay) {
            // Se o toque for na área do joystick à esquerda, ativa navegação de menu por D-Pad
            if (x < getWidth() * 0.38f && y > getHeight() * 0.45f) {
                if (!joystickActive) {
                    joystickActive = true;
                    joystickPointerId = pointerId;
                    joystickBaseX = x;
                    joystickBaseY = y;
                    joystickKnobX = x;
                    joystickKnobY = y;
                    updateMenuJoystick(0, 0);
                }
                return;
            }

            // Toque em qualquer outro lugar do menu: ENVIA CLIQUE REAL DE MOUSE COM O DEDO!
            menuTouchPointerId = pointerId;
            vibrateClick();
            // Move o cursor para a posição tocada e aperta botão esquerdo
            SDLActivity.onNativeMouse(0, MotionEvent.ACTION_MOVE, x, y, false);
            SDLActivity.onNativeMouse(MotionEvent.BUTTON_PRIMARY, MotionEvent.ACTION_DOWN, x, y, false);
            return;
        }

        // 3. MODO GAMEPLAY:
        // A) Checa botões de ação
        for (TouchButton b : actionButtons) {
            float r = b.bounds.width() / 2f;
            float dx = x - b.bounds.centerX();
            float dy = y - b.bounds.centerY();
            if (dx * dx + dy * dy <= r * r) {
                b.isDown = true;
                b.activePointerId = pointerId;
                vibrateClick();
                dispatchButtonDown(b);
                return;
            }
        }

        // B) Checa atalhos rápidos de armas (1, 2, 3, 4)
        for (TouchButton slot : quickSlots) {
            if (slot.bounds.contains(x, y)) {
                slot.isDown = true;
                slot.activePointerId = pointerId;
                vibrateClick();
                dispatchButtonDown(slot);
                return;
            }
        }

        // C) Lado Esquerdo: Ativa Joystick Virtual (WASD)
        if (x < getWidth() * 0.42f) {
            if (!joystickActive) {
                joystickActive = true;
                joystickPointerId = pointerId;
                joystickBaseX = x;
                joystickBaseY = y;
                joystickKnobX = x;
                joystickKnobY = y;
                updateGameplayJoystick(0, 0);
            }
            return;
        }

        // D) Lado Direito (fora dos botões): Trackpad de rotação da câmera
        if (lookPointerId == -1) {
            lookPointerId = pointerId;
            lastLookX = x;
            lastLookY = y;
        }
    }

    private void handlePointerMove(int pointerId, float x, float y) {
        if (!controlsEnabled) return;

        // Joystick (comportamento varia se está em Menu ou Gameplay)
        if (pointerId == joystickPointerId && joystickActive) {
            float dx = x - joystickBaseX;
            float dy = y - joystickBaseY;
            float dist = (float) Math.sqrt(dx * dx + dy * dy);
            if (dist > joystickRadius) {
                dx = (dx / dist) * joystickRadius;
                dy = (dy / dist) * joystickRadius;
            }
            joystickKnobX = joystickBaseX + dx;
            joystickKnobY = joystickBaseY + dy;

            float nx = dx / joystickRadius;
            float ny = dy / joystickRadius;

            if (inGameplay) {
                updateGameplayJoystick(nx, ny);
            } else {
                updateMenuJoystick(nx, ny);
            }
            return;
        }

        // Toque no menu: arrastar o dedo move o mouse
        if (pointerId == menuTouchPointerId && !inGameplay) {
            SDLActivity.onNativeMouse(MotionEvent.BUTTON_PRIMARY, MotionEvent.ACTION_MOVE, x, y, false);
            return;
        }

        // Câmera no gameplay (trackpad relativo)
        if (pointerId == lookPointerId && inGameplay) {
            float deltaX = x - lastLookX;
            float deltaY = y - lastLookY;
            lastLookX = x;
            lastLookY = y;

            if (Math.abs(deltaX) > 0.05f || Math.abs(deltaY) > 0.05f) {
                SDLActivity.onNativeMouse(0, MotionEvent.ACTION_MOVE, deltaX * 1.6f, deltaY * 1.6f, true);
            }
        }
    }

    private void handlePointerUp(int pointerId) {
        // Soltou toque no menu (solta botão do mouse)
        if (pointerId == menuTouchPointerId) {
            menuTouchPointerId = -1;
            SDLActivity.onNativeMouse(0, MotionEvent.ACTION_UP, 0, 0, false);
        }

        // Soltou botão de ação
        for (TouchButton b : actionButtons) {
            if (b.activePointerId == pointerId) {
                b.isDown = false;
                b.activePointerId = -1;
                dispatchButtonUp(b);
            }
        }

        // Soltou slot rápido
        for (TouchButton slot : quickSlots) {
            if (slot.activePointerId == pointerId) {
                slot.isDown = false;
                slot.activePointerId = -1;
                dispatchButtonUp(slot);
            }
        }

        // Soltou Joystick
        if (pointerId == joystickPointerId) {
            joystickActive = false;
            joystickPointerId = -1;
            joystickKnobX = joystickBaseX;
            joystickKnobY = joystickBaseY;
            if (inGameplay) {
                updateGameplayJoystick(0, 0);
            } else {
                updateMenuJoystick(0, 0);
            }
        }

        // Soltou Trackpad de câmera
        if (pointerId == lookPointerId) {
            lookPointerId = -1;
        }
    }

    private void resetAllInputs() {
        if (menuTouchPointerId != -1) {
            menuTouchPointerId = -1;
            SDLActivity.onNativeMouse(0, MotionEvent.ACTION_UP, 0, 0, false);
        }
        for (TouchButton b : actionButtons) {
            if (b.isDown) {
                b.isDown = false;
                b.activePointerId = -1;
                dispatchButtonUp(b);
            }
        }
        for (TouchButton slot : quickSlots) {
            if (slot.isDown) {
                slot.isDown = false;
                slot.activePointerId = -1;
                dispatchButtonUp(slot);
            }
        }
        resetGameplayInputs();
        joystickActive = false;
        joystickPointerId = -1;
        lookPointerId = -1;
    }

    private void resetGameplayInputs() {
        syncKey(KeyEvent.KEYCODE_W, keyW, false);
        syncKey(KeyEvent.KEYCODE_S, keyS, false);
        syncKey(KeyEvent.KEYCODE_A, keyA, false);
        syncKey(KeyEvent.KEYCODE_D, keyD, false);
        keyW = keyA = keyS = keyD = false;

        if (activeMenuDpadKey != 0) {
            SDLActivity.onNativeKeyUp(activeMenuDpadKey);
            activeMenuDpadKey = 0;
        }
    }

    /** Atualiza movimento do personagem no gameplay via WASD */
    private void updateGameplayJoystick(float nx, float ny) {
        float deadzone = 0.22f;
        boolean newW = ny < -deadzone;
        boolean newS = ny > deadzone;
        boolean newA = nx < -deadzone;
        boolean newD = nx > deadzone;

        syncKey(KeyEvent.KEYCODE_W, keyW, newW);
        syncKey(KeyEvent.KEYCODE_S, keyS, newS);
        syncKey(KeyEvent.KEYCODE_A, keyA, newA);
        syncKey(KeyEvent.KEYCODE_D, keyD, newD);

        keyW = newW;
        keyS = newS;
        keyA = newA;
        keyD = newD;
    }

    /** Navegação precisa nos menus usando D-Pad com repetição */
    private void updateMenuJoystick(float nx, float ny) {
        float threshold = 0.38f;
        int targetKey = 0;

        // Prioriza o eixo com maior deflexão
        if (Math.abs(ny) >= Math.abs(nx)) {
            if (ny < -threshold) targetKey = KeyEvent.KEYCODE_DPAD_UP;
            else if (ny > threshold) targetKey = KeyEvent.KEYCODE_DPAD_DOWN;
        } else {
            if (nx < -threshold) targetKey = KeyEvent.KEYCODE_DPAD_LEFT;
            else if (nx > threshold) targetKey = KeyEvent.KEYCODE_DPAD_RIGHT;
        }

        if (targetKey != activeMenuDpadKey) {
            if (activeMenuDpadKey != 0) {
                SDLActivity.onNativeKeyUp(activeMenuDpadKey);
            }
            activeMenuDpadKey = targetKey;
            if (activeMenuDpadKey != 0) {
                vibrateClick();
                SDLActivity.onNativeKeyDown(activeMenuDpadKey);
                lastDpadRepeatTime = SystemClock.uptimeMillis() + (DPAD_INITIAL_DELAY_MS - DPAD_REPEAT_INTERVAL_MS);
            }
        }
    }

    private void syncKey(int keycode, boolean oldState, boolean newState) {
        if (!oldState && newState) {
            SDLActivity.onNativeKeyDown(keycode);
        } else if (oldState && !newState) {
            SDLActivity.onNativeKeyUp(keycode);
        }
    }

    private void dispatchButtonDown(TouchButton b) {
        if (b.isQuickTurn) {
            // Giro 180° no RE5 PC: Tecla S + Espaço juntas
            SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_S);
            SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_SPACE);
            return;
        }

        if (b.mouseButton == 1) {
            SDLActivity.onNativeMouse(MotionEvent.BUTTON_PRIMARY, MotionEvent.ACTION_DOWN, 0, 0, false);
        } else if (b.mouseButton == 3) {
            SDLActivity.onNativeMouse(MotionEvent.BUTTON_SECONDARY, MotionEvent.ACTION_DOWN, 0, 0, false);
        } else if (b.keycode > 0) {
            SDLActivity.onNativeKeyDown(b.keycode);
        }
    }

    private void dispatchButtonUp(TouchButton b) {
        if (b.isQuickTurn) {
            SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_SPACE);
            SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_S);
            return;
        }

        if (b.mouseButton > 0) {
            // Para soltar o mouse no SDL, passa 0 como estado do botão no ACTION_UP!
            SDLActivity.onNativeMouse(0, MotionEvent.ACTION_UP, 0, 0, false);
        } else if (b.keycode > 0) {
            SDLActivity.onNativeKeyUp(b.keycode);
        }
    }

    private void vibrateClick() {
        try {
            if (vibrator != null && vibrator.hasVibrator()) {
                if (Build.VERSION.SDK_INT >= 26) {
                    vibrator.vibrate(VibrationEffect.createPredefined(VibrationEffect.EFFECT_CLICK));
                } else {
                    vibrator.vibrate(18);
                }
            } else {
                performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            }
        } catch (Exception ignored) {}
    }

    private float dp(float value) {
        return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics());
    }
}
