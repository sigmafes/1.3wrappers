package app.gdps13.port;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/** Descargas HTTP(S) para el núcleo nativo (canciones de Newgrounds). */
public class Net {
    public static byte[] fetch(String url, String post, String userAgent, int[] status) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setInstanceFollowRedirects(true);
            c.setConnectTimeout(20000);
            c.setReadTimeout(120000);
            c.setRequestProperty("User-Agent", userAgent);
            if (post != null && !post.isEmpty()) {
                c.setDoOutput(true);
                c.setRequestMethod("POST");
                c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded");
                OutputStream o = c.getOutputStream();
                o.write(post.getBytes("UTF-8"));
                o.close();
            }
            status[0] = c.getResponseCode();
            InputStream in = status[0] >= 400 ? c.getErrorStream() : c.getInputStream();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            if (in != null) {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                in.close();
            }
            return out.toByteArray();
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
