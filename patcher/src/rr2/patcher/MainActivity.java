package rr2.patcher;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.view.View;
import android.widget.*;

import java.io.*;
import java.util.Arrays;

/** Pick your own Reckless Racing 2 APK and OBB, patch them into one 64-bit APK, install. */
public class MainActivity extends Activity {
    TextView status;
    static final String GAME = "com.polarbit.RecklessRacing2";
    static final int SAVE = 3;
    Button[] buttons = new Button[4];  // 1. APK, 2. OBB, 3. patch and install, save (shown if install fails)
    Uri[] picked = new Uri[2];

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        l.setPadding(pad, pad, pad, pad);
        String[] labels = {"1. Choose game APK (v1.0.4 .apk or .xapk)", "2. Choose game data (.obb, or a zip/.xapk with it)",
                           "3. Patch and install", "Save patched APK instead"};
        for (int i = 0; i < 4; i++) {
            int which = i;
            buttons[i] = new Button(this);
            buttons[i].setText(labels[i]);
            buttons[i].setOnClickListener(v -> {
                if (which < 2) {  // opens where the last pick was; the grant is kept across launches
                    Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                    if (picked[which] != null) pick.putExtra(DocumentsContract.EXTRA_INITIAL_URI, picked[which]);
                    startActivityForResult(pick, which);
                }
                else if (which == 2) patch();
                else startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                        .setType("application/vnd.android.package-archive").putExtra(Intent.EXTRA_TITLE, "RR2-arm64.apk"), SAVE);
            });
            l.addView(buttons[i]);
        }
        buttons[2].setEnabled(false);
        buttons[SAVE].setVisibility(View.GONE);
        status = new TextView(this);
        status.setPadding(0, pad, 0, 0);
        status.setTextIsSelectable(true);
        l.addView(status);
        ScrollView s = new ScrollView(this);
        s.addView(l);
        setContentView(s);
        log("Reckless Racing 2 Revived, by Blivii.\nPatches your own copy of the game to run on 64-bit phones. No game files are included in this app.");
        for (int i = 0; i < 2; i++) {  // the files picked last time, if they're still there
            String saved = prefs().getString("picked" + i, null);
            if (saved != null && !use(i, Uri.parse(saved))) prefs().edit().remove("picked" + i).apply();
        }
    }

    SharedPreferences prefs() { return getSharedPreferences("picks", MODE_PRIVATE); }

    /** Shows a picked file on its button; false if it can no longer be read (moved, deleted). */
    boolean use(int i, Uri uri) {
        String name = null;
        try (Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) name = c.getString(c.getColumnIndexOrThrow(OpenableColumns.DISPLAY_NAME));
        } catch (Exception ignored) {}
        if (name == null) return false;
        picked[i] = uri;
        buttons[i].setText((i + 1) + ". " + name);
        buttons[2].setEnabled(picked[0] != null && picked[1] != null);
        return true;
    }

    /** Keeps read access to a newly picked file across launches, and forgets the one it replaces. */
    void remember(int i, Uri uri) {
        int flags = Intent.FLAG_GRANT_READ_URI_PERMISSION;
        try {
            if (picked[i] != null && !picked[i].equals(uri) && !picked[i].equals(picked[1 - i]))
                getContentResolver().releasePersistableUriPermission(picked[i], flags);
            getContentResolver().takePersistableUriPermission(uri, flags);
            prefs().edit().putString("picked" + i, uri.toString()).apply();
        } catch (Exception e) {  // a picker that can't grant lasting access: works for this session only
            prefs().edit().remove("picked" + i).apply();
        }
    }

    void log(String m) { runOnUiThread(() -> status.append(m + "\n\n")); }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        if (res != RESULT_OK || data == null) return;
        if (req == SAVE) { save(data.getData()); return; }
        Uri uri = data.getData();
        remember(req, uri);
        if (!use(req, uri)) log("Couldn't read that file.");
    }

    void setBusy(boolean busy) {
        runOnUiThread(() -> {
            for (Button b : buttons) b.setEnabled(!busy);
            if (busy) buttons[SAVE].setVisibility(View.GONE);
            if (!busy) buttons[2].setEnabled(picked[0] != null && picked[1] != null);
        });
    }

    void patch() {
        setBusy(true);
        status.setText("");
        warnIfOtherSigner();
        new Thread(() -> {
            File in = new File(getCacheDir(), "original.apk"), obb = new File(getCacheDir(), "main.obb"),
                 out = new File(getCacheDir(), "patched.apk");
            try {
                log("Reading files...");
                copy(picked[0], in);
                copy(picked[1], obb);
                log("Patching...");
                try (InputStream so = getAssets().open("libRecklessRacing2.so");
                     InputStream k = getAssets().open("key.pk8"); InputStream c = getAssets().open("cert.der")) {
                    Patcher.patch(in, obb, so, out, Patcher.loadKey(k), Patcher.loadCert(c), this::log);
                }
                log("Installing...");
                install(out);
            } catch (Exception e) {
                log("Failed: " + e.getMessage());
                setBusy(false);
            } finally {
                in.delete();
                obb.delete();
            }
        }).start();
    }

    /** An installed copy signed with another key (the original game) blocks the install. */
    void warnIfOtherSigner() {
        try (InputStream c = getAssets().open("cert.der")) {
            byte[] ours = Patcher.bytes(c);
            PackageInfo p = getPackageManager().getPackageInfo(GAME, PackageManager.GET_SIGNING_CERTIFICATES);
            for (Signature sig : p.signingInfo.getApkContentsSigners()) if (Arrays.equals(sig.toByteArray(), ours)) return;
            log("Warning: Reckless Racing 2 is already installed with a different signature (probably the original "
                    + "game). Android won't install the patched copy over it. Uninstall it first, but note that this "
                    + "deletes its saves.");
        } catch (Exception notInstalled) {}
    }

    /** Fallback when Android blocks the install: keep the APK so it can be installed another way. */
    void save(Uri to) {
        new Thread(() -> {
            try (InputStream i = new FileInputStream(new File(getCacheDir(), "patched.apk"));
                 OutputStream o = getContentResolver().openOutputStream(to)) {
                Patcher.pipe(i, o);
                log("Saved. Install it with your file manager, or from a PC with: adb install RR2-arm64.apk");
            } catch (Exception e) {
                log("Save failed: " + e.getMessage());
            }
        }).start();
    }

    void copy(Uri from, File to) throws IOException {
        try (InputStream s = getContentResolver().openInputStream(from); OutputStream o = new FileOutputStream(to)) {
            Patcher.pipe(s, o);
        }
    }

    void install(File apk) throws IOException {
        PackageInstaller pi = getPackageManager().getPackageInstaller();
        int id = pi.createSession(new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL));
        try (PackageInstaller.Session s = pi.openSession(id)) {
            try (InputStream i = new FileInputStream(apk); OutputStream o = s.openWrite("base.apk", 0, apk.length())) {
                Patcher.pipe(i, o);
                s.fsync(o);
            }
            Intent cb = new Intent(this, MainActivity.class).setAction("rr2.patcher.INSTALLED");
            s.commit(PendingIntent.getActivity(this, 0, cb,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE).getIntentSender());
        }
    }

    @Override protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        if (!"rr2.patcher.INSTALLED".equals(i.getAction())) return;
        int st = i.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (st == PackageInstaller.STATUS_PENDING_USER_ACTION) { startActivity((Intent) i.getParcelableExtra(Intent.EXTRA_INTENT)); return; }
        setBusy(false);
        if (st == PackageInstaller.STATUS_SUCCESS) {
            new File(getCacheDir(), "patched.apk").delete();
            log("Installed. You can delete the APK and OBB you picked; the game no longer needs them.");
            return;
        }
        String why = st == PackageInstaller.STATUS_FAILURE_CONFLICT
                ? "Another copy of the game (probably the original) is installed with a different signature. Uninstall it and try again; uninstalling deletes its saves."
                : st == PackageInstaller.STATUS_FAILURE_BLOCKED
                ? "Android blocked the install (device policy or developer verification). Save the patched APK and install it with your file manager or adb."
                : st == PackageInstaller.STATUS_FAILURE_ABORTED ? "Install cancelled."
                : st == PackageInstaller.STATUS_FAILURE_STORAGE ? "Not enough free storage."
                : "You can save the patched APK and install it another way.";
        log("Install failed: " + i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) + "\n" + why);
        buttons[SAVE].setVisibility(View.VISIBLE);
    }
}
