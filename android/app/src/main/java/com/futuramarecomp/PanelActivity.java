package com.futuramarecomp;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

// A small 2D panel for the launcher's questions and messages (platform/sdl/Launcher.cpp). The game's own activity
// is an immersive VR app, so dialogs inside it are never shown on Quest; a separate activity opens as a panel.
// The native side starts it and polls `result`: -1 = waiting, 0 = second button / closed, 1 = first button
// (with action "files": 1 only when all-files access is on afterwards).
public class PanelActivity extends Activity {
    public static volatile int result = -1;

    private TextView text;
    private Button first;
    private boolean waitingForAccess;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable poll = new Runnable() {
        @Override public void run() {
            if (!waitingForAccess) return;
            if (hasAccess()) done(1);
            else handler.postDelayed(this, 500);
        }
    };

    static boolean hasAccess() {
        return Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        setTitle("Futurama");
        final Intent in = getIntent();
        final String action = in.getStringExtra("action");
        final String b2 = in.getStringExtra("button2");

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(48, 40, 48, 40);
        text = new TextView(this);
        text.setText(in.getStringExtra("text"));
        text.setTextSize(20);
        root.addView(text);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.END);
        row.setPadding(0, 32, 0, 0);
        first = new Button(this);
        first.setText(in.getStringExtra("button1"));
        first.setOnClickListener(v -> {
            if (!"files".equals(action)) { done(1); return; }
            if (waitingForAccess) { done(hasAccess() ? 1 : 0); return; }   // ("Continue")
            waitingForAccess = true;
            openAccessPage();
            text.setText("Turn on \"Allow access to manage all files\" for Futurama in the settings page, then come "
                         + "back here. The game continues by itself once access is on.");
            first.setText("Continue");
            handler.postDelayed(poll, 500);
        });
        row.addView(first);
        if (b2 != null) {
            Button second = new Button(this);
            second.setText(b2);
            second.setOnClickListener(v -> done(0));
            row.addView(second);
        }
        root.addView(row);
        setContentView(root);
    }

    private void openAccessPage() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                     Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (Exception e2) {
                done(0);
            }
        }
    }

    @Override protected void onResume() {
        super.onResume();
        if (waitingForAccess && hasAccess()) done(1);
    }

    private void done(int r) {
        waitingForAccess = false;
        handler.removeCallbacks(poll);
        result = r;
        finish();
    }

    @Override protected void onDestroy() {
        handler.removeCallbacks(poll);
        if (result == -1) result = 0;   // (closed without an answer)
        super.onDestroy();
    }
}
