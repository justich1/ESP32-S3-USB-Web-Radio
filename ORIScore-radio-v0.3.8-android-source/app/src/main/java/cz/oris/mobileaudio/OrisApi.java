package cz.oris.mobileaudio;

import android.content.ContentResolver;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Base64;
import android.util.LruCache;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;

public final class OrisApi {
    public interface JsonCallback { void done(JSONObject data, Exception error); }
    public interface ArrayCallback { void done(JSONArray data, Exception error); }
    public interface TextCallback { void done(String text, Exception error); }
    public interface BitmapCallback { void done(Bitmap bitmap, Exception error); }

    private static final long LOGO_CACHE_MAX_AGE_MS = 30L * 24L * 60L * 60L * 1000L;

    private final Context context;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final File logoCacheDir;
    private final LruCache<String, Bitmap> memoryLogoCache;
    private final Object logoLock = new Object();
    private final Map<String, List<BitmapCallback>> pendingLogoRequests = new HashMap<>();

    private volatile String host;
    private volatile String user;
    private volatile String password;

    public OrisApi(Context context, String host, String user, String password) {
        this.context = context.getApplicationContext();
        this.logoCacheDir = new File(this.context.getCacheDir(), "oris_station_logos");
        if (!logoCacheDir.exists()) logoCacheDir.mkdirs();
        int maxMemoryKb = (int) (Runtime.getRuntime().maxMemory() / 1024L);
        int cacheKb = Math.max(2048, Math.min(12288, maxMemoryKb / 16));
        this.memoryLogoCache = new LruCache<String, Bitmap>(cacheKb) {
            @Override
            protected int sizeOf(String key, Bitmap bitmap) {
                return Math.max(1, bitmap.getByteCount() / 1024);
            }
        };
        configure(host, user, password);
    }

    public void configure(String host, String user, String password) {
        String value = host == null ? "" : host.trim();
        value = value.replace("http://", "").replace("https://", "");
        int slash = value.indexOf('/');
        if (slash >= 0) value = value.substring(0, slash);
        int colon = value.lastIndexOf(':');
        if (colon > 0 && value.indexOf(':') == colon) value = value.substring(0, colon);
        this.host = value.isEmpty() ? "192.168.1.177" : value;
        this.user = user == null || user.trim().isEmpty() ? "admin" : user.trim();
        this.password = password == null ? "" : password;
    }

    public String getHost() { return host; }

    public String absolute(String path) {
        if (path == null || path.isEmpty()) return "http://" + host + "/";
        if (path.startsWith("http://") || path.startsWith("https://")) return path;
        return "http://" + host + (path.startsWith("/") ? path : "/" + path);
    }

    public void getJson(String path, JsonCallback callback) {
        request("GET", path, null, "application/x-www-form-urlencoded", (text, error) -> {
            if (error != null) callback.done(null, error);
            else {
                try { callback.done(new JSONObject(text), null); }
                catch (Exception e) { callback.done(null, e); }
            }
        });
    }

    public void getArray(String path, ArrayCallback callback) {
        request("GET", path, null, "application/x-www-form-urlencoded", (text, error) -> {
            if (error != null) callback.done(null, error);
            else {
                try { callback.done(new JSONArray(text), null); }
                catch (Exception e) { callback.done(null, e); }
            }
        });
    }

    public void getText(String path, TextCallback callback) {
        request("GET", path, null, "application/x-www-form-urlencoded", callback);
    }

    public void post(String path, TextCallback callback) {
        request("POST", path, new byte[0], "application/x-www-form-urlencoded", callback);
    }

    public void postForm(String path, Map<String, String> values, TextCallback callback) {
        StringBuilder body = new StringBuilder();
        try {
            for (Map.Entry<String, String> entry : values.entrySet()) {
                if (body.length() > 0) body.append('&');
                body.append(URLEncoder.encode(entry.getKey(), "UTF-8"));
                body.append('=');
                body.append(URLEncoder.encode(entry.getValue() == null ? "" : entry.getValue(), "UTF-8"));
            }
        } catch (Exception e) {
            callback.done(null, e);
            return;
        }
        request("POST", path, body.toString().getBytes(StandardCharsets.UTF_8),
                "application/x-www-form-urlencoded", callback);
    }

    public void deleteByGet(String path, TextCallback callback) { getText(path, callback); }

    private void request(String method, String path, byte[] body, String contentType, TextCallback callback) {
        new Thread(() -> {
            HttpURLConnection connection = null;
            try {
                connection = open(path, method);
                connection.setRequestProperty("Accept", "application/json, text/plain, */*");
                if (body != null) {
                    connection.setDoOutput(true);
                    connection.setRequestProperty("Content-Type", contentType);
                    connection.setFixedLengthStreamingMode(body.length);
                    try (OutputStream out = connection.getOutputStream()) { out.write(body); }
                }
                int code = connection.getResponseCode();
                InputStream input = code >= 200 && code < 400
                        ? connection.getInputStream() : connection.getErrorStream();
                String text = readText(input);
                if (code < 200 || code >= 400) throw new RuntimeException(
                        (text == null || text.trim().isEmpty()) ? "HTTP " + code : text.trim());
                String result = text == null ? "" : text;
                main.post(() -> callback.done(result, null));
            } catch (Exception e) {
                main.post(() -> callback.done(null, e));
            } finally {
                if (connection != null) connection.disconnect();
            }
        }, "OrisApi").start();
    }

    public void loadBitmap(String path, BitmapCallback callback) {
        final String key = absolute(path);
        Bitmap memory = memoryLogoCache.get(key);
        if (memory != null && !memory.isRecycled()) {
            main.post(() -> callback.done(memory, null));
            return;
        }

        File diskFile = logoCacheFile(key);
        if (diskFile.isFile() && System.currentTimeMillis() - diskFile.lastModified() <= LOGO_CACHE_MAX_AGE_MS) {
            Bitmap diskBitmap = BitmapFactory.decodeFile(diskFile.getAbsolutePath());
            if (diskBitmap != null) {
                memoryLogoCache.put(key, diskBitmap);
                main.post(() -> callback.done(diskBitmap, null));
                return;
            }
            diskFile.delete();
        }

        synchronized (logoLock) {
            List<BitmapCallback> pending = pendingLogoRequests.get(key);
            if (pending != null) {
                pending.add(callback);
                return;
            }
            pending = new ArrayList<>();
            pending.add(callback);
            pendingLogoRequests.put(key, pending);
        }

        new Thread(() -> {
            HttpURLConnection connection = null;
            Bitmap bitmap = null;
            Exception failure = null;
            try {
                connection = open(path, "GET");
                connection.setRequestProperty("Accept", "image/*");
                int code = connection.getResponseCode();
                if (code < 200 || code >= 400) throw new RuntimeException("HTTP " + code);
                try (InputStream input = connection.getInputStream()) {
                    bitmap = BitmapFactory.decodeStream(input);
                }
                if (bitmap == null) throw new RuntimeException("Obrázek nelze načíst");
                memoryLogoCache.put(key, bitmap);
                writeBitmapCache(diskFile, bitmap);
            } catch (Exception e) {
                failure = e;
            } finally {
                if (connection != null) connection.disconnect();
            }

            final Bitmap result = bitmap;
            final Exception error = failure;
            final List<BitmapCallback> callbacks;
            synchronized (logoLock) {
                callbacks = pendingLogoRequests.remove(key);
            }
            if (callbacks != null) {
                main.post(() -> {
                    for (BitmapCallback pending : callbacks) pending.done(result, error);
                });
            }
        }, "OrisImage").start();
    }

    public void clearBitmapCache() {
        memoryLogoCache.evictAll();
        File[] files = logoCacheDir.listFiles();
        if (files != null) {
            for (File file : files) file.delete();
        }
    }

    public void clearBitmapCache(String path) {
        String key = absolute(path);
        memoryLogoCache.remove(key);
        logoCacheFile(key).delete();
    }

    private File logoCacheFile(String key) {
        return new File(logoCacheDir, sha256(key) + ".png");
    }

    private static String sha256(String value) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] bytes = digest.digest(value.getBytes(StandardCharsets.UTF_8));
            StringBuilder out = new StringBuilder(bytes.length * 2);
            for (byte b : bytes) out.append(String.format(Locale.US, "%02x", b & 0xFF));
            return out.toString();
        } catch (Exception e) {
            return Integer.toHexString(value.hashCode());
        }
    }

    private static void writeBitmapCache(File file, Bitmap bitmap) {
        File temp = new File(file.getParentFile(), file.getName() + ".tmp");
        try (FileOutputStream output = new FileOutputStream(temp)) {
            if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, output)) return;
            output.flush();
            if (file.exists()) file.delete();
            temp.renameTo(file);
        } catch (Exception ignored) {
            temp.delete();
        }
    }

    public void upload(Uri uri, String fieldName, String path, TextCallback callback) {
        new Thread(() -> {
            HttpURLConnection connection = null;
            try {
                String boundary = "----ORIS" + UUID.randomUUID().toString().replace("-", "");
                connection = open(path, "POST");
                connection.setDoOutput(true);
                connection.setChunkedStreamingMode(16 * 1024);
                connection.setRequestProperty("Content-Type", "multipart/form-data; boundary=" + boundary);

                ContentResolver resolver = context.getContentResolver();
                String fileName = "upload.bin";
                String type = resolver.getType(uri);
                if (type == null) type = "application/octet-stream";
                String last = uri.getLastPathSegment();
                if (last != null && !last.trim().isEmpty()) fileName = last.replace('"', '_');

                try (OutputStream out = connection.getOutputStream();
                     InputStream in = resolver.openInputStream(uri)) {
                    if (in == null) throw new RuntimeException("Soubor nelze otevřít");
                    String head = "--" + boundary + "\r\n"
                            + "Content-Disposition: form-data; name=\"" + fieldName + "\"; filename=\"" + fileName + "\"\r\n"
                            + "Content-Type: " + type + "\r\n\r\n";
                    out.write(head.getBytes(StandardCharsets.UTF_8));
                    byte[] buffer = new byte[16 * 1024];
                    int read;
                    while ((read = in.read(buffer)) >= 0) out.write(buffer, 0, read);
                    out.write(("\r\n--" + boundary + "--\r\n").getBytes(StandardCharsets.UTF_8));
                }

                int code = connection.getResponseCode();
                InputStream input = code >= 200 && code < 400
                        ? connection.getInputStream() : connection.getErrorStream();
                String text = readText(input);
                if (code < 200 || code >= 400) throw new RuntimeException(
                        (text == null || text.trim().isEmpty()) ? "HTTP " + code : text.trim());
                String result = text == null ? "Hotovo" : text;
                main.post(() -> callback.done(result, null));
            } catch (Exception e) {
                main.post(() -> callback.done(null, e));
            } finally {
                if (connection != null) connection.disconnect();
            }
        }, "OrisUpload").start();
    }

    public void download(String path, Uri target, TextCallback callback) {
        new Thread(() -> {
            HttpURLConnection connection = null;
            try {
                connection = open(path, "GET");
                int code = connection.getResponseCode();
                if (code < 200 || code >= 400) {
                    String message = readText(connection.getErrorStream());
                    throw new RuntimeException(message == null || message.trim().isEmpty()
                            ? "HTTP " + code : message.trim());
                }
                try (InputStream in = connection.getInputStream();
                     OutputStream out = context.getContentResolver().openOutputStream(target, "w")) {
                    if (out == null) throw new RuntimeException("Cílový soubor nelze otevřít");
                    byte[] buffer = new byte[32 * 1024];
                    int read;
                    while ((read = in.read(buffer)) >= 0) out.write(buffer, 0, read);
                    out.flush();
                }
                main.post(() -> callback.done("Staženo", null));
            } catch (Exception e) {
                main.post(() -> callback.done(null, e));
            } finally {
                if (connection != null) connection.disconnect();
            }
        }, "OrisDownload").start();
    }

    private HttpURLConnection open(String path, String method) throws Exception {
        URL url = new URL(absolute(path));
        HttpURLConnection connection = (HttpURLConnection) url.openConnection();
        connection.setConnectTimeout(5000);
        connection.setReadTimeout(10000);
        connection.setUseCaches(false);
        connection.setInstanceFollowRedirects(false);
        connection.setRequestMethod(method);
        String auth = user + ":" + password;
        connection.setRequestProperty("Authorization", "Basic " + Base64.encodeToString(
                auth.getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP));
        return connection;
    }

    private static String readText(InputStream input) throws Exception {
        if (input == null) return "";
        try (InputStream in = input; ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            int read;
            while ((read = in.read(buffer)) >= 0) out.write(buffer, 0, read);
            return out.toString(StandardCharsets.UTF_8.name());
        }
    }

    public static Map<String, String> form() { return new LinkedHashMap<>(); }
}
