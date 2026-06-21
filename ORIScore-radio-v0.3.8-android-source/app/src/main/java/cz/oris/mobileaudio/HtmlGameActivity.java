package cz.oris.mobileaudio;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.util.Base64;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.FilterInputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Spouští HTML soubory uložené v rádiu. Virtuální webový kořen zajišťuje,
 * že relativní CSS, JavaScript, obrázky, zvuky a další soubory fungují i přes
 * query endpoint /raw?disk=...&f=....
 */
public final class HtmlGameActivity extends Activity {
    private static final String VIRTUAL_HOST = "oris-radio.local";

    private WebView webView;
    private SharedPreferences preferences;
    private String radioHost;
    private String webUser;
    private String webPass;
    private String disk;
    private String initialPath;
    private String gameTitle;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        preferences = getSharedPreferences("oris_audio", MODE_PRIVATE);
        String address = getIntent().getStringExtra("radio_address");
        if (address == null || address.trim().isEmpty()) {
            address = preferences.getString("radio_address", "192.168.1.177:8928");
        }
        radioHost = extractHost(address);
        webUser = preferences.getString("web_user", "admin");
        webPass = preferences.getString("web_pass", "admin");
        disk = safeValue(getIntent().getStringExtra("disk"), "usb");
        initialPath = normalizeRemotePath(safeValue(getIntent().getStringExtra("path"), "/index.html"));
        gameTitle = safeValue(getIntent().getStringExtra("title"), fileName(initialPath));

        View content = buildUi();
        setContentView(content);
        applySystemBarInsets(content);
        loadRemotePath(initialPath);
    }

    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.BLACK);

        LinearLayout toolbar = new LinearLayout(this);
        toolbar.setOrientation(LinearLayout.HORIZONTAL);
        toolbar.setGravity(Gravity.CENTER_VERTICAL);
        toolbar.setPadding(dp(4), dp(3), dp(4), dp(3));
        toolbar.setBackgroundColor(Color.rgb(21, 27, 37));

        Button back = toolbarButton("‹");
        back.setContentDescription("Zpět");
        back.setOnClickListener(v -> {
            if (webView != null && webView.canGoBack()) webView.goBack();
            else finish();
        });
        toolbar.addView(back, fixed(dp(48)));

        Button reload = toolbarButton("Obnovit");
        reload.setOnClickListener(v -> {
            if (webView != null) webView.reload();
        });
        toolbar.addView(reload, fixed(dp(88)));

        TextView title = new TextView(this);
        title.setText(gameTitle);
        title.setTextColor(Color.WHITE);
        title.setTextSize(15);
        title.setSingleLine(true);
        title.setGravity(Gravity.CENTER);
        title.setPadding(dp(6), 0, dp(6), 0);
        toolbar.addView(title, new LinearLayout.LayoutParams(0, dp(48), 1f));

        Button close = toolbarButton("Zavřít");
        close.setOnClickListener(v -> finish());
        toolbar.addView(close, fixed(dp(82)));

        root.addView(toolbar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        webView = new WebView(this);
        webView.setBackgroundColor(Color.BLACK);
        configureWebView(webView);
        root.addView(webView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        return root;
    }

    private void configureWebView(WebView view) {
        WebSettings settings = view.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setDatabaseEnabled(true);
        settings.setLoadsImagesAutomatically(true);
        settings.setUseWideViewPort(true);
        settings.setLoadWithOverviewMode(true);
        settings.setBuiltInZoomControls(true);
        settings.setDisplayZoomControls(false);
        settings.setMediaPlaybackRequiresUserGesture(false);
        settings.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setSupportMultipleWindows(false);

        view.setWebChromeClient(new WebChromeClient());
        view.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebView webView, WebResourceRequest request) {
                Uri uri = request.getUrl();
                if (uri == null || !VIRTUAL_HOST.equalsIgnoreCase(uri.getHost())) return null;
                return loadRadioResource(request);
            }

            @Override
            public boolean shouldOverrideUrlLoading(WebView webView, WebResourceRequest request) {
                Uri uri = request.getUrl();
                if (uri == null) return false;
                String scheme = uri.getScheme();
                if ("http".equalsIgnoreCase(scheme) || "https".equalsIgnoreCase(scheme) ||
                        "javascript".equalsIgnoreCase(scheme) || "about".equalsIgnoreCase(scheme) ||
                        "data".equalsIgnoreCase(scheme) || "blob".equalsIgnoreCase(scheme)) {
                    return false;
                }
                return true;
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (request != null && request.isForMainFrame()) {
                    CharSequence description = error == null ? "Neznámá chyba" : error.getDescription();
                    Toast.makeText(HtmlGameActivity.this,
                            "HTML nelze otevřít: " + description, Toast.LENGTH_LONG).show();
                }
            }
        });
    }

    private WebResourceResponse loadRadioResource(WebResourceRequest request) {
        HttpURLConnection connection = null;
        try {
            String method = request.getMethod();
            if (method == null || (!method.equalsIgnoreCase("GET") && !method.equalsIgnoreCase("HEAD"))) {
                return errorResponse(405, "Method Not Allowed", "Tato minihra používá nepodporovanou HTTP metodu.");
            }

            String remotePath = normalizeRemotePath(request.getUrl().getPath());
            String endpoint = "http://" + radioHost + "/raw?disk=" + encode(disk) + "&f=" + encode(remotePath);
            connection = (HttpURLConnection) new URL(endpoint).openConnection();
            connection.setConnectTimeout(6000);
            connection.setReadTimeout(20000);
            connection.setUseCaches(false);
            connection.setInstanceFollowRedirects(true);
            connection.setRequestMethod(method.toUpperCase(Locale.ROOT));
            connection.setRequestProperty("Authorization", basicAuth(webUser, webPass));

            Map<String, String> requestHeaders = request.getRequestHeaders();
            copyRequestHeader(requestHeaders, connection, "Accept");
            copyRequestHeader(requestHeaders, connection, "Accept-Language");
            copyRequestHeader(requestHeaders, connection, "Range");
            copyRequestHeader(requestHeaders, connection, "User-Agent");

            int status = connection.getResponseCode();
            InputStream source = status >= 200 && status < 400
                    ? connection.getInputStream() : connection.getErrorStream();
            if (source == null) {
                String reason = safeReason(connection.getResponseMessage());
                connection.disconnect();
                return errorResponse(status, reason, "Soubor nelze načíst.");
            }

            String contentType = connection.getContentType();
            String mime = mimeFrom(contentType, remotePath);
            String encoding = charsetFrom(contentType, mime);
            Map<String, String> headers = flattenHeaders(connection.getHeaderFields());
            String reason = safeReason(connection.getResponseMessage());
            HttpURLConnection openConnection = connection;
            InputStream managed = new FilterInputStream(source) {
                @Override
                public void close() throws java.io.IOException {
                    try {
                        super.close();
                    } finally {
                        openConnection.disconnect();
                    }
                }
            };
            connection = null;
            return new WebResourceResponse(mime, encoding, status, reason, headers, managed);
        } catch (Exception e) {
            if (connection != null) connection.disconnect();
            return errorResponse(500, "Load Error", "Soubor nelze načíst: " + safeMessage(e));
        }
    }

    private void loadRemotePath(String remotePath) {
        Uri virtualUri = new Uri.Builder()
                .scheme("https")
                .authority(VIRTUAL_HOST)
                .path(normalizeRemotePath(remotePath))
                .build();
        webView.loadUrl(virtualUri.toString());
    }

    private WebResourceResponse errorResponse(int status, String reason, String message) {
        int safeStatus = Math.max(400, Math.min(599, status));
        String escaped = htmlEscape(message == null ? "Chyba načítání" : message);
        String html = "<!doctype html><html><head><meta charset=\"utf-8\">"
                + "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                + "<style>body{background:#0b0e13;color:#edf3fb;font-family:sans-serif;padding:24px}"
                + "h2{color:#d84a4a}</style></head><body><h2>Soubor nelze otevřít</h2><p>"
                + escaped + "</p></body></html>";
        InputStream input = new java.io.ByteArrayInputStream(html.getBytes(StandardCharsets.UTF_8));
        return new WebResourceResponse("text/html", "UTF-8", safeStatus,
                safeReason(reason), new HashMap<>(), input);
    }

    @Override
    public void onBackPressed() {
        if (webView != null && webView.canGoBack()) webView.goBack();
        else super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        if (webView != null) {
            webView.stopLoading();
            webView.loadUrl("about:blank");
            webView.clearHistory();
            webView.removeAllViews();
            webView.destroy();
            webView = null;
        }
        super.onDestroy();
    }

    private void applySystemBarInsets(View content) {
        if (Build.VERSION.SDK_INT < 35) return;
        content.setOnApplyWindowInsetsListener((view, insets) -> {
            android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars());
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            return insets;
        });
        content.requestApplyInsets();
    }

    private Button toolbarButton(String text) {
        Button button = new Button(this);
        button.setText(text);
        button.setTextColor(Color.WHITE);
        button.setAllCaps(false);
        button.setBackgroundColor(Color.TRANSPARENT);
        button.setPadding(dp(4), 0, dp(4), 0);
        return button;
    }

    private LinearLayout.LayoutParams fixed(int width) {
        return new LinearLayout.LayoutParams(width, dp(48));
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static void copyRequestHeader(Map<String, String> headers, HttpURLConnection connection, String name) {
        if (headers == null) return;
        String value = headers.get(name);
        if (value == null) {
            for (Map.Entry<String, String> entry : headers.entrySet()) {
                if (name.equalsIgnoreCase(entry.getKey())) {
                    value = entry.getValue();
                    break;
                }
            }
        }
        if (value != null && !value.trim().isEmpty()) connection.setRequestProperty(name, value);
    }

    private static Map<String, String> flattenHeaders(Map<String, List<String>> source) {
        Map<String, String> result = new HashMap<>();
        if (source == null) return result;
        for (Map.Entry<String, List<String>> entry : source.entrySet()) {
            String key = entry.getKey();
            if (key == null || entry.getValue() == null || entry.getValue().isEmpty()) continue;
            // Typ obsahu určujeme podle skutečné přípony souboru. /raw může vracet
            // text/plain nebo attachment, což by HTML zobrazilo jako zdrojový text.
            if ("Content-Type".equalsIgnoreCase(key) ||
                    "Content-Disposition".equalsIgnoreCase(key)) continue;
            result.put(key, join(entry.getValue()));
        }
        return result;
    }

    private static String join(List<String> values) {
        StringBuilder out = new StringBuilder();
        for (String value : values) {
            if (value == null) continue;
            if (out.length() > 0) out.append(", ");
            out.append(value);
        }
        return out.toString();
    }

    private static String mimeFrom(String contentType, String path) {
        String inferred = mimeFromExtension(extension(path));
        if (inferred != null) return inferred;
        if (contentType != null && !contentType.trim().isEmpty()) {
            int semi = contentType.indexOf(';');
            String mime = (semi >= 0 ? contentType.substring(0, semi) : contentType).trim();
            if (!mime.isEmpty()) return mime;
        }
        return "application/octet-stream";
    }

    private static String mimeFromExtension(String ext) {
        switch (ext) {
            case "html": case "htm": return "text/html";
            case "css": return "text/css";
            case "js": case "mjs": return "application/javascript";
            case "json": return "application/json";
            case "xml": return "application/xml";
            case "svg": return "image/svg+xml";
            case "png": return "image/png";
            case "jpg": case "jpeg": return "image/jpeg";
            case "gif": return "image/gif";
            case "webp": return "image/webp";
            case "ico": return "image/x-icon";
            case "mp3": return "audio/mpeg";
            case "wav": return "audio/wav";
            case "ogg": return "audio/ogg";
            case "m4a": return "audio/mp4";
            case "aac": return "audio/aac";
            case "flac": return "audio/flac";
            case "mp4": return "video/mp4";
            case "webm": return "video/webm";
            case "woff": return "font/woff";
            case "woff2": return "font/woff2";
            case "ttf": return "font/ttf";
            case "otf": return "font/otf";
            default: return null;
        }
    }

    private static String charsetFrom(String contentType, String mime) {
        if (contentType != null) {
            String lower = contentType.toLowerCase(Locale.ROOT);
            int charset = lower.indexOf("charset=");
            if (charset >= 0) {
                String value = contentType.substring(charset + 8).trim();
                int semi = value.indexOf(';');
                if (semi >= 0) value = value.substring(0, semi);
                value = value.replace("\"", "").trim();
                if (!value.isEmpty()) return value;
            }
        }
        if (mime.startsWith("text/") || mime.contains("javascript") || mime.contains("json") || mime.contains("xml")) {
            return "UTF-8";
        }
        return null;
    }

    private static String normalizeRemotePath(String path) {
        String value = path == null ? "/" : path.replace('\\', '/').trim();
        if (!value.startsWith("/")) value = "/" + value;
        String[] parts = value.split("/");
        java.util.ArrayList<String> clean = new java.util.ArrayList<>();
        for (String part : parts) {
            if (part.isEmpty() || part.equals(".")) continue;
            if (part.equals("..")) {
                if (!clean.isEmpty()) clean.remove(clean.size() - 1);
            } else {
                clean.add(part);
            }
        }
        StringBuilder result = new StringBuilder("/");
        for (int i = 0; i < clean.size(); i++) {
            if (i > 0) result.append('/');
            result.append(clean.get(i));
        }
        return result.toString();
    }

    private static String extractHost(String address) {
        String value = address == null ? "" : address.trim();
        value = value.replace("ws://", "").replace("wss://", "")
                .replace("http://", "").replace("https://", "");
        int slash = value.indexOf('/');
        if (slash >= 0) value = value.substring(0, slash);
        int colon = value.lastIndexOf(':');
        if (colon > 0 && value.indexOf(':') == colon) value = value.substring(0, colon);
        return value.isEmpty() ? "192.168.1.177" : value;
    }

    private static String encode(String value) throws Exception {
        return URLEncoder.encode(value == null ? "" : value, StandardCharsets.UTF_8.name());
    }

    private static String basicAuth(String user, String password) {
        String credentials = safeValue(user, "admin") + ":" + safeValue(password, "");
        return "Basic " + Base64.encodeToString(credentials.getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP);
    }

    private static String extension(String path) {
        String name = fileName(path).toLowerCase(Locale.ROOT);
        int dot = name.lastIndexOf('.');
        return dot >= 0 && dot + 1 < name.length() ? name.substring(dot + 1) : "";
    }

    private static String fileName(String path) {
        if (path == null || path.isEmpty()) return "HTML minihra";
        int slash = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
        String name = slash >= 0 ? path.substring(slash + 1) : path;
        return name.isEmpty() ? "HTML minihra" : name;
    }

    private static String safeValue(String value, String fallback) {
        return value == null || value.trim().isEmpty() ? fallback : value.trim();
    }

    private static String safeReason(String reason) {
        return reason == null || reason.trim().isEmpty() ? "Error" : reason.trim();
    }

    private static String safeMessage(Exception error) {
        if (error == null) return "Neznámá chyba";
        String message = error.getMessage();
        return message == null || message.trim().isEmpty() ? error.getClass().getSimpleName() : message.trim();
    }

    private static String htmlEscape(String value) {
        return value.replace("&", "&amp;")
                .replace("<", "&lt;")
                .replace(">", "&gt;")
                .replace("\"", "&quot;");
    }
}
