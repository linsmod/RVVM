package com.rvvm.android;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ListView;
import android.widget.Toast;

import java.io.IOException;
import java.util.ArrayList;

/**
 * Launcher activity: app grid list.
 *
 * <p>Tapping an app name launches it as a new Android activity (GuestActivity)
 * with {@link Intent#FLAG_ACTIVITY_NEW_DOCUMENT} and {@link
 * Intent#FLAG_ACTIVITY_MULTIPLE_TASK}. The app's {@link
 * Intent#CATEGORY_DEFAULT} (set to GuestActivity) gives each app its own window,
 * so the system Recents/Back stack works per application. One GuestActivity
 * can be visible at a time on the screen, but multiple instances exist in
 * background state (suspended/ended), and switching between them is the
 * responsibility of the system - not the RVVM host.
 *
 * <p>The console is never visible here: all input to the launcher is from its
 * own 1x1 invisible EditText (managed in the hosting MainActivity), so the
 * console keyboard is not needed. The console and its controls stay on the
 * screen while the apps are running, and each guest displays its own
 * SurfaceView over the console.
 */
public class SimpleLauncherActivity extends Activity {

    private static final String TAG = "RVVM-SimpleLauncher";
    private ListView appListView;
    private Button homeButton, recentAppsButton, backButton;
    private ArrayAdapter<String> appAdapter;

    /** The apps to launch are the *.exe files in assets - exactly what MainActivity
      * reads into guestApps[]. This avoids duplication. */
    private String[] guestApps;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // The UI is minimal: an app grid (list) and the three-button taskbar at the
        // bottom. Everything else (the console, TTY keyboard) stays on the screen
        // inside MainActivity - not visible in this Activity.

        // Inflate the custom launcher layout.
        setContentView(R.layout.activity_simple_launcher);

        // Find and wire the three-button taskbar buttons.
        homeButton = findViewById(R.id.homeButton);
        recentAppsButton = findViewById(R.id.recentAppsButton);
        backButton = findViewById(R.id.backButton);

        // The three-button actions are no-ops: home goes to the launcher (no-op here),
        // recent opens a system-like recents list (implemented later), back returns
        // to the launcher (no-op here). In a real implementation they would be
        // handled by the system. For now they just show a Toast to confirm.
        homeButton.setOnClickListener(v -> Toast.makeText(this, "Home", Toast.LENGTH_SHORT).show());
        recentAppsButton.setOnClickListener(v -> showRecentApps());
        backButton.setOnClickListener(v -> finish());

        // Debug support: adb can launch a guest directly by index, e.g.
        //   adb shell am start -n com.rvvm.android/.SimpleLauncherActivity --ei guest_app_index 0
        // after_guest_exit is passed through to GuestActivity (e.g. "stay").
        int index = getIntent().getIntExtra("guest_app_index", -1);

        // Load the guest apps from assets (exact same list as MainActivity).
        loadGuestApps();

        if (index >= 0 && index < guestApps.length) {
            Log.i(TAG, "Direct launch by index " + index + ": " + guestApps[index]);
            launchGuestApp(guestApps[index],
                    getIntent().getStringExtra(GuestActivity.EXTRA_AFTER_GUEST_EXIT));
            finish();
            return;
        }

        // Populate the app list view. Tapping an app starts it.
        appListView = findViewById(R.id.appListView);
        appAdapter = new ArrayAdapter<>(this, R.layout.item_app_name, guestApps);
        appListView.setAdapter(appAdapter);
        appListView.setOnItemClickListener((parent, view, position, id) ->
                launchGuestApp(guestApps[position], null));
        // Long-press: open in MainActivity's floating window-card mode instead
        // of the dedicated GuestActivity.
        appListView.setOnItemLongClickListener((parent, view, position, id) -> {
            showLaunchModeMenu(view, guestApps[position]);
            return true;
        });
    }

    /**
     * Long-press popup: launch the app in MainActivity's floating window-card
     * mode (MainActivity reads EXTRA_GUEST_APP and starts that guest inside
     * its draggable GlWindowCard workspace).
     */
    private void showLaunchModeMenu(View anchor, String appName) {
        android.widget.PopupMenu menu = new android.widget.PopupMenu(this, anchor);
        // Stay-open mode: GuestActivity does not finish when the guest exits,
        // keeping the last screen and console visible.
        menu.getMenu().add("打开（退出时保留tty）").setOnMenuItemClickListener(item -> {
            launchGuestApp(appName, GuestActivity.AFTER_GUEST_EXIT_STAY);
            return true;
        });
        menu.getMenu().add("打开（窗口模式）").setOnMenuItemClickListener(item -> {
            Intent intent = new Intent(this, MainActivity.class);
            intent.putExtra(MainActivity.EXTRA_GUEST_APP, appName);
            startActivity(intent);
            return true;
        });
        menu.show();
    }

    /** Load the guest apps from assets. */
    private void loadGuestApps() {
        try {
            String[] assets = getAssets().list("");
            ArrayList<String> exeList = new ArrayList<>();
            if (assets != null) {
                for (String name : assets) {
                    if (name.endsWith(".exe")) {
                        exeList.add(name);
                    }
                }
            }
            java.util.Collections.sort(exeList);
            guestApps = exeList.toArray(new String[0]);
        } catch (IOException e) {
            Log.e(TAG, "Failed to list assets", e);
            guestApps = new String[]{ "test_game_activity.exe" };
        }
    }

    /** Launch the given guest app using GuestActivity (default exit behavior). */
    private void launchGuestApp(String appName) {
        launchGuestApp(appName, null);
    }

    /** Launch the given guest app using GuestActivity.
     * @param afterGuestExit value for {@link GuestActivity#EXTRA_AFTER_GUEST_EXIT},
     *        null/"finish" closes the activity on guest exit, "stay" keeps it open. */
    private void launchGuestApp(String appName, String afterGuestExit) {
        Intent intent = new Intent(this, GuestActivity.class);
        intent.setAction(appName);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_DOCUMENT | Intent.FLAG_ACTIVITY_MULTIPLE_TASK);
        intent.putExtra(GuestActivity.EXTRA_APP_NAME, appName);
        if (afterGuestExit != null) {
            intent.putExtra(GuestActivity.EXTRA_AFTER_GUEST_EXIT, afterGuestExit);
        }
        startActivity(intent);
    }

    /** Show the recent apps list (to be implemented). */
    private void showRecentApps() {
        // TODO: Implement recent apps list using ActivityManager.getAppTasks()
        Toast.makeText(this, "Recent apps", Toast.LENGTH_SHORT).show();
    }
}
