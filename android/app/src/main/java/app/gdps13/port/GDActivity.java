package app.gdps13.port;

import android.os.Bundle;
import android.system.Os;

import org.libsdl.app.SDLActivity;

import java.io.File;

/** Actividad del port: SDL abre la ventana y llama a SDL_main de libmain.so. */
public class GDActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        try {
            // Registro en Descargas si se puede escribir ahí (más fácil de encontrar); si no, en la carpeta de la app.
            File dl = new File(android.os.Environment.getExternalStoragePublicDirectory(android.os.Environment.DIRECTORY_DOWNLOADS), "13gdps-log.txt");
            boolean canDl = false;
            try { canDl = dl.exists() ? dl.canWrite() : dl.createNewFile(); } catch (Exception ignored) { }
            Os.setenv("GD_LOGFILE", canDl ? dl.getAbsolutePath() : new File(getExternalFilesDir(null), "log.txt").getAbsolutePath(), true);
        } catch (Exception ignored) {
        }
        super.onCreate(savedInstanceState);
        highestRefreshRate();
        sustainedPerformance();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "SDL2_mixer", "main" };
    }

    /** Rendimiento sostenido: evita que el sistema baje la velocidad del procesador a mitad de un nivel. */
    void sustainedPerformance() {
        if (android.os.Build.VERSION.SDK_INT < 24) return;
        try {
            android.os.PowerManager pm = (android.os.PowerManager) getSystemService(POWER_SERVICE);
            if (pm != null && pm.isSustainedPerformanceModeSupported()) getWindow().setSustainedPerformanceMode(true);
        } catch (Exception ignored) { }
    }

    /** Pide la frecuencia de pantalla más alta (90/120 Hz) para que el FPS Bypass se vea de verdad. */
    void highestRefreshRate() {
        if (android.os.Build.VERSION.SDK_INT < 23) return;
        try {
            android.view.Display d = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode cur = d.getMode(), best = cur;
            for (android.view.Display.Mode m : d.getSupportedModes())
                if (m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight()
                        && m.getRefreshRate() > best.getRefreshRate()) best = m;
            android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredDisplayModeId = best.getModeId();
            getWindow().setAttributes(lp);
        } catch (Exception ignored) { }
    }
}
