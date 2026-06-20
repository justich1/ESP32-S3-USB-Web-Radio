package cz.oris.mobileaudio;

import android.util.Base64;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

public final class RadioHttpClient {
    public interface Callback {
        void onResult(boolean ok, String message);
    }

    private RadioHttpClient() {}

    public static void setVolume(
            String host,
            String user,
            String password,
            int volume,
            Callback callback
    ) {
        Thread thread = new Thread(() -> {
            HttpURLConnection connection = null;
            try {
                int safeVolume = Math.max(0, Math.min(100, volume));
                URL url = new URL("http://" + host + "/audio/volume?v=" + safeVolume);
                connection = (HttpURLConnection) url.openConnection();
                connection.setConnectTimeout(2500);
                connection.setReadTimeout(2500);
                connection.setRequestMethod("GET");
                String credentials = user + ":" + password;
                String auth = Base64.encodeToString(
                        credentials.getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP);
                connection.setRequestProperty("Authorization", "Basic " + auth);
                int code = connection.getResponseCode();
                InputStream input = code >= 200 && code < 300
                        ? connection.getInputStream() : connection.getErrorStream();
                if (input != null) {
                    byte[] buffer = new byte[256];
                    while (input.read(buffer) >= 0) {
                    }
                    input.close();
                }
                if (callback != null) {
                    callback.onResult(code >= 200 && code < 300,
                            "HTTP " + code);
                }
            } catch (Exception e) {
                if (callback != null) callback.onResult(false,
                        e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage());
            } finally {
                if (connection != null) connection.disconnect();
            }
        }, "RadioHttpVolume");
        thread.start();
    }
}
