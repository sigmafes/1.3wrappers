package app.gdps13.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;

/** Arranque: pide el permiso de almacenamiento si hace falta y abre el juego. */
public class LauncherActivity extends Activity {
    static final int REQ_FILES = 77;
    boolean asked = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        start();
    }

    /** Android moderno: pide "Acceso a todos los archivos" (necesario para guardar en External). */
    void start() {
        if (!asked) {
            asked = true;
            try {
                if (Build.VERSION.SDK_INT >= 30) {
                    if (!android.os.Environment.isExternalStorageManager()) {
                        new AlertDialog.Builder(this)
                                .setTitle("Storage permission")
                                .setMessage("To use the \"External\" save location, the game needs access to your files. Enable the permission on the next screen, then go back.")
                                .setPositiveButton("Allow", (d, w) -> {
                                    try {
                                        startActivityForResult(new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                                android.net.Uri.parse("package:" + getPackageName())), REQ_FILES);
                                    } catch (Exception e) {
                                        startActivityForResult(new Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION), REQ_FILES);
                                    }
                                })
                                .setNegativeButton("Not now", (d, w) -> start())
                                .setCancelable(false)
                                .show();
                        return;
                    }
                } else if (Build.VERSION.SDK_INT >= 23
                        && checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                    requestPermissions(new String[] { android.Manifest.permission.READ_EXTERNAL_STORAGE,
                            android.Manifest.permission.WRITE_EXTERNAL_STORAGE }, REQ_FILES);
                    return;
                }
            } catch (Exception ignored) { }
        }
        startActivity(new Intent(this, GDActivity.class));
        finish();
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == REQ_FILES) start();
    }

    @Override
    public void onRequestPermissionsResult(int req, String[] p, int[] r) {
        super.onRequestPermissionsResult(req, p, r);
        if (req == REQ_FILES) start();
    }
}
