package com.re5.recompiled;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.Toast;

/**
 * Editor do layout dos controles na tela, aberto pelo launcher: a arte do jogo ao fundo e o
 * {@link TouchControlsView} em modo de edição (arrastar move, pinça muda o tamanho; barra com
 * Opções, Restaurar e Salvar). O mesmo layout vale no jogo.
 */
public class ControlsEditorActivity extends Activity {

    private TouchControlsView touch;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        L.init(this);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        FrameLayout root = new FrameLayout(this);
        ImageView bg = new ImageView(this);
        bg.setImageResource(R.drawable.re5_hero);
        bg.setScaleType(ImageView.ScaleType.CENTER_CROP);
        root.addView(bg, new FrameLayout.LayoutParams(-1, -1));
        touch = new TouchControlsView(this);
        ControlsOptions.apply(this, touch);
        touch.setControlsEnabled(true);
        touch.setEditMode(true);
        touch.setEditorListener(new TouchControlsView.EditorListener() {
            @Override
            public void onEditorOptions() {
                ControlsOptions.show(ControlsEditorActivity.this, () -> ControlsOptions.apply(ControlsEditorActivity.this, touch));
            }

            @Override
            public void onEditorClose(boolean saved) {
                if (saved) Toast.makeText(ControlsEditorActivity.this, L.t("Layout salvo"), Toast.LENGTH_SHORT).show();
                finish();
            }
        });
        root.addView(touch, new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);
        immersive();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) immersive();
    }

    private void immersive() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }
}
