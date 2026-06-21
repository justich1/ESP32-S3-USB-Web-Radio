package cz.oris.mobileaudio;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.webkit.HttpAuthHandler;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

public final class ControlActivity extends Activity {
    private static final int FILE_CHOOSER_REQUEST = 2201;

    private WebView webView;
    private ValueCallback<Uri[]> fileCallback;
    private SharedPreferences preferences;
    private String radioHost;
    private String webUser;
    private String webPass;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        preferences = getSharedPreferences("oris_audio", MODE_PRIVATE);

        String address = getIntent().getStringExtra("radio_address");
        if (address == null || address.trim().isEmpty()) {
            address = preferences.getString("radio_address", "192.168.1.177:8928");
        }
        radioHost = extractHost(address);
        webUser = preferences.getString("web_user", "admin");
        webPass = preferences.getString("web_pass", "admin");

        View content = buildUi();
        setContentView(content);
        applySystemBarInsets(content);
        loadHome();
    }

    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.WHITE);

        LinearLayout toolbar = new LinearLayout(this);
        toolbar.setOrientation(LinearLayout.HORIZONTAL);
        toolbar.setGravity(Gravity.CENTER_VERTICAL);
        toolbar.setPadding(dp(4), dp(4), dp(4), dp(4));
        toolbar.setBackgroundColor(Color.rgb(32, 35, 42));

        Button back = toolbarButton("‹");
        back.setOnClickListener(v -> {
            if (webView.canGoBack()) webView.goBack();
            else finish();
        });
        toolbar.addView(back, fixed(dp(48)));

        Button home = toolbarButton("Domů");
        home.setOnClickListener(v -> loadHome());
        toolbar.addView(home, fixed(dp(76)));

        Button reload = toolbarButton("Obnovit");
        reload.setOnClickListener(v -> webView.reload());
        toolbar.addView(reload, fixed(dp(88)));

        TextView title = new TextView(this);
        title.setText("Ovládání rádia");
        title.setTextColor(Color.WHITE);
        title.setTextSize(16);
        title.setGravity(Gravity.CENTER);
        toolbar.addView(title, new LinearLayout.LayoutParams(0, dp(48), 1f));

        Button close = toolbarButton("Zavřít");
        close.setOnClickListener(v -> finish());
        toolbar.addView(close, fixed(dp(82)));

        root.addView(toolbar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        webView = new WebView(this);
        WebSettings settings = webView.getSettings();
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

        webView.setWebViewClient(new WebViewClient() {
            @Override
            public void onReceivedHttpAuthRequest(
                    WebView view,
                    HttpAuthHandler handler,
                    String host,
                    String realm
            ) {
                handler.proceed(webUser, webPass);
            }

            @Override
            public void onReceivedError(
                    WebView view,
                    int errorCode,
                    String description,
                    String failingUrl
            ) {
                Toast.makeText(ControlActivity.this,
                        "Web rádia: " + description, Toast.LENGTH_SHORT).show();
            }
        });

        webView.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onShowFileChooser(
                    WebView webView,
                    ValueCallback<Uri[]> filePathCallback,
                    FileChooserParams fileChooserParams
            ) {
                if (fileCallback != null) fileCallback.onReceiveValue(null);
                fileCallback = filePathCallback;
                try {
                    Intent intent = fileChooserParams.createIntent();
                    startActivityForResult(intent, FILE_CHOOSER_REQUEST);
                    return true;
                } catch (Exception e) {
                    fileCallback = null;
                    Toast.makeText(ControlActivity.this,
                            "Výběr souboru nelze otevřít", Toast.LENGTH_SHORT).show();
                    return false;
                }
            }
        });

        root.addView(webView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        return root;
    }

    private void loadHome() {
        webView.loadUrl("http://" + radioHost + "/");
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == FILE_CHOOSER_REQUEST && fileCallback != null) {
            Uri[] result = WebChromeClient.FileChooserParams.parseResult(resultCode, data);
            fileCallback.onReceiveValue(result);
            fileCallback = null;
        }
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
            webView.destroy();
        }
        if (fileCallback != null) {
            fileCallback.onReceiveValue(null);
            fileCallback = null;
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

    private static String extractHost(String address) {
        String value = address == null ? "" : address.trim();
        value = value.replace("ws://", "").replace("http://", "");
        int slash = value.indexOf('/');
        if (slash >= 0) value = value.substring(0, slash);
        int colon = value.lastIndexOf(':');
        if (colon > 0 && value.indexOf(':') == colon) value = value.substring(0, colon);
        return value.isEmpty() ? "192.168.1.177" : value;
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
}
