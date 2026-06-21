package cz.oris.mobileaudio;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.media.projection.MediaProjectionManager;
import android.net.Uri;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.provider.DocumentsContract;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.view.MotionEvent;
import android.view.Window;
import android.view.WindowManager;
import android.view.WindowInsets;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.GridLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.PopupMenu;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.net.InetAddress;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.LinkedList;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public final class MainActivity extends Activity {
    private static final int PICK_AUDIO = 2001;
    private static final int PICK_STATION_LOGO = 2002;
    private static final int REQUEST_SYSTEM_AUDIO = 2003;
    private static final int REQUEST_RECORD_AUDIO = 2004;
    private static final int PICK_FILE_UPLOAD = 2005;
    private static final int CREATE_FILE_DOWNLOAD = 2006;
    private static final int REQUEST_NOTIFICATIONS = 2007;
    private static final int PICK_AUDIO_FOLDER = 2008;

    private static final String SERVICE_TYPE = "_sendspin._tcp.";
    private static final int DEFAULT_SENDSPIN_PORT = 8928;

    private static final int BG = Color.rgb(11, 14, 19);
    private static final int BG_SOFT = Color.rgb(17, 23, 34);
    private static final int PANEL = Color.rgb(21, 27, 37);
    private static final int PANEL_2 = Color.rgb(27, 36, 48);
    private static final int PANEL_3 = Color.rgb(34, 45, 58);
    private static final int LINE = Color.rgb(44, 57, 72);
    private static final int TEXT = Color.rgb(237, 243, 251);
    private static final int MUTED = Color.rgb(151, 166, 184);
    private static final int ACCENT = Color.rgb(77, 141, 255);
    private static final int ACCENT_2 = Color.rgb(114, 167, 255);
    private static final int DANGER = Color.rgb(216, 74, 74);
    private static final int OK = Color.rgb(98, 216, 140);
    private static final int WARN = Color.rgb(242, 199, 92);
    private static final String[] EQ_BAND_LABELS = {
            "31 Hz", "62 Hz", "125 Hz", "250 Hz", "500 Hz",
            "1 kHz", "2 kHz", "4 kHz", "8 kHz", "16 kHz"
    };

    private enum Page {
        PLAYER("Přehrávač"),
        RADIO("Internetová rádia"),
        PHONE_MUSIC("Hudba v mobilu"),
        SYSTEM_AUDIO("Stream zvuku"),
        FILES("Soubory rádia"),
        SETTINGS("Nastavení");

        final String title;
        Page(String title) { this.title = title; }
    }

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ArrayList<Uri> playlistUris = new ArrayList<>();
    private final ArrayList<String> playlistNames = new ArrayList<>();
    private final ArrayList<JSONObject> cachedStations = new ArrayList<>();
    private final LinkedList<NsdServiceInfo> resolveQueue = new LinkedList<>();

    private SharedPreferences preferences;
    private OrisApi api;
    private SendspinConnection sendspin;
    private AudioStreamer streamer;
    private MediaProjectionManager mediaProjectionManager;

    private LinearLayout root;
    private FrameLayout pageHost;
    private TextView pageTitle;
    private TextView headerConnection;
    private TextView bottomStatus;
    private Page currentPage = Page.PLAYER;

    private String radioHost;
    private int sendspinPort;
    private String webUser;
    private String webPassword;

    private JSONObject lastStatus;
    private JSONObject lastConfig;
    private int selectedStationIndex = -1;
    private int selectedTrackIndex = 0;
    private boolean pendingLocalPlay;
    private int sendspinRetryCount;

    private ImageView playerCover;
    private TextView playerMode;
    private TextView playerMainTitle;
    private TextView playerSubtitle;
    private TextView playerDetail;
    private TextView playerBattery;
    private ProgressBar playerBatteryBar;
    private Button playerPlayPause;
    private SeekBar playerVolume;
    private TextView playerVolumePercent;
    private GridLayout playerStationGrid;
    private String lastStationGridSignature = "";

    private LinearLayout radioEditorList;
    private final ArrayList<EditText> radioNameEditors = new ArrayList<>();
    private final ArrayList<EditText> radioUrlEditors = new ArrayList<>();
    private int pendingLogoIndex = -1;

    private ArrayAdapter<String> phonePlaylistAdapter;
    private ListView phonePlaylistView;
    private TextView phoneConnectionStatus;
    private TextView phoneTrackTitle;
    private TextView phoneProgress;
    private ProgressBar phoneProgressBar;
    private Button phonePauseButton;
    private CheckBox phoneRepeatCheck;
    private CheckBox phoneShuffleCheck;
    private boolean phoneRepeatEnabled;
    private boolean phoneShuffleEnabled;

    private TextView systemAudioStatus;
    private Button systemAudioStart;
    private Button systemAudioStop;
    private boolean statusReceiverRegistered;
    private boolean mediaCommandReceiverRegistered;
    private boolean mobilePlaylistActive;
    private String mobilePlaylistTitle = "";

    private String filesDisk = "ffat";
    private String filesPath = "/";
    private LinearLayout filesList;
    private TextView filesPathLabel;
    private final ArrayList<Uri> pendingUploadUris = new ArrayList<>();
    private int pendingUploadIndex = 0;
    private String pendingDownloadDisk = "";
    private String pendingDownloadPath = "";
    private String pendingDownloadName = "";

    private LinearLayout karaokeList;
    private Spinner karaokeDiskSpinner;

    private EditText settingsHost;
    private EditText settingsPort;
    private EditText settingsUser;
    private EditText settingsPassword;
    private TextView settingsTestResult;

    private Spinner apModeSpinner;
    private EditText apSsidInput;
    private EditText apPasswordInput;
    private EditText mdnsInput;
    private CheckBox ftpEnabledCheck;
    private EditText ftpUserInput;
    private EditText ftpPasswordInput;
    private Spinner ftpDiskSpinner;
    private CheckBox rgbEnabledCheck;
    private SeekBar configVolumeSeek;
    private TextView configVolumeValue;
    private CheckBox eqEnabledCheck;
    private CheckBox eqAutoHeadroomCheck;
    private SeekBar eqPreampSeek;
    private SeekBar outputGainSeek;
    private SeekBar volumeCurveSeek;
    private TextView eqPreampValue;
    private TextView outputGainValue;
    private TextView volumeCurveValue;
    private final SeekBar[] eqBandSeeks = new SeekBar[10];
    private final TextView[] eqBandValues = new TextView[10];
    private CheckBox batteryEnabledCheck;
    private EditText batteryDividerInput;
    private EditText batteryCalibrationInput;
    private EditText defaultRadioNameInput;
    private EditText defaultRadioUrlInput;
    private CheckBox smartSpeakerEnabledCheck;
    private LinearLayout savedWifiList;
    private Spinner wifiScanSpinner;
    private EditText wifiSsidInput;
    private EditText wifiPasswordInput;
    private TextView wifiScanStatus;
    private final ArrayList<JSONObject> scannedWifiNetworks = new ArrayList<>();

    private NsdManager nsdManager;
    private NsdManager.DiscoveryListener discoveryListener;
    private WifiManager.MulticastLock multicastLock;
    private boolean discoveryRunning;
    private boolean resolving;

    private final BroadcastReceiver mediaCommandReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (!MediaNotificationService.ACTION_APP_COMMAND.equals(intent.getAction())) return;
            String command = intent.getStringExtra("command");
            if (MediaNotificationService.COMMAND_PREVIOUS.equals(command)) {
                streamer.previous();
            } else if (MediaNotificationService.COMMAND_NEXT.equals(command)) {
                streamer.next();
            } else if (MediaNotificationService.COMMAND_TOGGLE.equals(command)) {
                streamer.pauseOrResume();
                if (phonePauseButton != null) {
                    phonePauseButton.setText(streamer.isPaused() ? "Pokračovat" : "Pauza");
                }
                updateMobileMediaNotification();
            } else if (MediaNotificationService.COMMAND_STOP.equals(command)) {
                streamer.stop();
            }
        }
    };

    private final BroadcastReceiver systemAudioStatusReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (!SystemAudioCaptureService.ACTION_STATUS.equals(intent.getAction())) return;
            String text = intent.getStringExtra(SystemAudioCaptureService.EXTRA_STATUS);
            boolean running = intent.getBooleanExtra(SystemAudioCaptureService.EXTRA_RUNNING, false);
            if (systemAudioStatus != null && text != null) systemAudioStatus.setText(text);
            if (systemAudioStart != null) systemAudioStart.setEnabled(!running);
            if (systemAudioStop != null) systemAudioStop.setEnabled(running);
            setBottomStatus(text == null ? "" : text, running ? OK : MUTED);
        }
    };

    private final Runnable statusTicker = new Runnable() {
        @Override
        public void run() {
            refreshStatus(false);
            mainHandler.postDelayed(this, 1200L);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        preferences = getSharedPreferences("oris_audio", MODE_PRIVATE);
        loadSettings();
        api = new OrisApi(this, radioHost, webUser, webPassword);
        mediaProjectionManager = (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        nsdManager = (NsdManager) getSystemService(Context.NSD_SERVICE);
        acquireMulticastLock();
        initSendspin();

        View content = buildShell();
        setContentView(content);
        applySystemBarInsets(content);
        registerSystemAudioReceiver();
        registerMediaCommandReceiver();
        requestNotificationPermissionIfNeeded();
        showPage(Page.PLAYER);
        refreshConfig(true);
        mainHandler.post(statusTicker);
    }

    private void loadSettings() {
        String legacyAddress = preferences.getString("radio_address", "");
        String savedHost = preferences.getString("radio_host", "");
        int savedPort = preferences.getInt("sendspin_port", DEFAULT_SENDSPIN_PORT);

        if (savedHost == null || savedHost.trim().isEmpty()) {
            String legacy = legacyAddress == null ? "" : legacyAddress.trim();
            legacy = legacy.replace("ws://", "").replace("http://", "").replace("/sendspin", "");
            int slash = legacy.indexOf('/');
            if (slash >= 0) legacy = legacy.substring(0, slash);
            int colon = legacy.lastIndexOf(':');
            if (colon > 0 && legacy.indexOf(':') == colon) {
                try { savedPort = Integer.parseInt(legacy.substring(colon + 1)); }
                catch (Exception ignored) {}
                legacy = legacy.substring(0, colon);
            }
            savedHost = legacy;
        }

        radioHost = cleanHost(savedHost);
        sendspinPort = savedPort > 0 && savedPort < 65536 ? savedPort : DEFAULT_SENDSPIN_PORT;
        webUser = preferences.getString("web_user", "admin");
        webPassword = preferences.getString("web_pass", "admin");
        phoneRepeatEnabled = preferences.getBoolean("phone_repeat", false);
        phoneShuffleEnabled = preferences.getBoolean("phone_shuffle", false);

        preferences.edit()
                .putString("radio_host", radioHost)
                .putInt("sendspin_port", sendspinPort)
                .putString("web_user", webUser)
                .putString("web_pass", webPassword)
                .apply();
    }

    private void saveSettings(String host, int port, String user, String password) {
        radioHost = cleanHost(host);
        sendspinPort = port > 0 && port < 65536 ? port : DEFAULT_SENDSPIN_PORT;
        webUser = user == null || user.trim().isEmpty() ? "admin" : user.trim();
        webPassword = password == null ? "" : password;
        preferences.edit()
                .putString("radio_host", radioHost)
                .putInt("sendspin_port", sendspinPort)
                .putString("web_user", webUser)
                .putString("web_pass", webPassword)
                .apply();
        api.configure(radioHost, webUser, webPassword);
    }

    private void initSendspin() {
        sendspin = new SendspinConnection(new SendspinConnection.Listener() {
            @Override
            public void onStatus(String message) {
                runOnUiThread(() -> {
                    if (phoneConnectionStatus != null) phoneConnectionStatus.setText(message);
                    setBottomStatus(message, MUTED);
                });
            }

            @Override
            public void onReady(String radioName) {
                runOnUiThread(() -> {
                    sendspinRetryCount = 0;
                    if (phoneConnectionStatus != null) phoneConnectionStatus.setText("Připojeno: " + radioName);
                    setBottomStatus("Mobilní audio připojeno k " + radioName, OK);
                    if (pendingLocalPlay) {
                        pendingLocalPlay = false;
                        streamer.play(playlistUris, playlistNames, selectedTrackIndex, phoneRepeatEnabled, phoneShuffleEnabled);
                    }
                });
            }

            @Override
            public void onDisconnected(String reason) {
                runOnUiThread(() -> {
                    if (phoneConnectionStatus != null) phoneConnectionStatus.setText(reason);
                    setBottomStatus(reason, DANGER);
                    if (pendingLocalPlay && sendspinRetryCount < 2) {
                        sendspinRetryCount++;
                        mainHandler.postDelayed(() -> sendspin.connectAsync(radioHost, sendspinPort), 900L);
                    } else {
                        pendingLocalPlay = false;
                    }
                });
            }

            @Override
            public void onRadioVolume(int volume) {
                runOnUiThread(() -> {
                    if (playerVolume != null && !playerVolume.isPressed()) playerVolume.setProgress(volume);
                    updatePlayerVolumePercent(volume);
                });
            }
        });

        streamer = new AudioStreamer(this, sendspin, new AudioStreamer.Listener() {
            @Override
            public void onStatus(String message) {
                runOnUiThread(() -> setBottomStatus(message, MUTED));
            }

            @Override
            public void onTrackChanged(int index, String name) {
                runOnUiThread(() -> {
                    selectedTrackIndex = index;
                    mobilePlaylistActive = true;
                    mobilePlaylistTitle = name == null ? "Skladba z mobilu" : name;
                    if (phoneTrackTitle != null) phoneTrackTitle.setText(name);
                    if (phonePlaylistView != null) phonePlaylistView.setItemChecked(index, true);
                    if (phonePauseButton != null) phonePauseButton.setText("Pauza");
                    updateMobileMediaNotification();
                });
            }

            @Override
            public void onProgress(long positionMs, long durationMs) {
                runOnUiThread(() -> {
                    if (phoneProgress != null) {
                        phoneProgress.setText(formatTime(positionMs) + " / " + formatTime(durationMs));
                    }
                    if (phoneProgressBar != null) {
                        phoneProgressBar.setProgress(durationMs > 0
                                ? (int) Math.min(1000L, positionMs * 1000L / durationMs)
                                : 0);
                    }
                });
            }

            @Override
            public void onStopped() {
                runOnUiThread(() -> {
                    mobilePlaylistActive = false;
                    mobilePlaylistTitle = "";
                    MediaNotificationService.hide(MainActivity.this);
                    if (phonePauseButton != null) phonePauseButton.setText("Pauza");
                    if (phoneProgress != null) phoneProgress.setText("00:00 / 00:00");
                    if (phoneProgressBar != null) phoneProgressBar.setProgress(0);
                });
            }
        });
    }

    private View buildShell() {
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BG);

        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(8), dp(7), dp(8), dp(7));
        header.setBackgroundColor(Color.rgb(13, 17, 24));

        Button menu = textButton("☰", PANEL_2);
        menu.setTextSize(22);
        menu.setOnClickListener(this::showMenu);
        header.addView(menu, new LinearLayout.LayoutParams(dp(52), dp(48)));

        LinearLayout titleBox = new LinearLayout(this);
        titleBox.setOrientation(LinearLayout.VERTICAL);
        titleBox.setPadding(dp(10), 0, 0, 0);
        TextView brand = text("ORIScore radio", 12, ACCENT_2, true);
        pageTitle = text("Přehrávač", 19, TEXT, true);
        titleBox.addView(brand);
        titleBox.addView(pageTitle);
        header.addView(titleBox, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        headerConnection = text("● offline", 12, MUTED, true);
        headerConnection.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        header.addView(headerConnection, new LinearLayout.LayoutParams(dp(100), dp(48)));

        root.addView(header, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        View line = new View(this);
        line.setBackgroundColor(LINE);
        root.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1)));

        pageHost = new FrameLayout(this);
        pageHost.setBackgroundColor(BG);
        root.addView(pageHost, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        bottomStatus = text("Připraveno", 11, MUTED, false);
        bottomStatus.setSingleLine(true);
        bottomStatus.setPadding(dp(12), dp(7), dp(12), dp(7));
        bottomStatus.setBackgroundColor(Color.rgb(10, 13, 18));
        root.addView(bottomStatus, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return root;
    }

    private static final int MENU_OPEN_RADIO_WEB = 1000;

    private void showMenu(View anchor) {
        PopupMenu popup = new PopupMenu(this, anchor);
        int id = 1;
        for (Page page : Page.values()) popup.getMenu().add(0, id++, id, page.title);
        popup.getMenu().add(0, MENU_OPEN_RADIO_WEB, MENU_OPEN_RADIO_WEB, "Otevřít web rádia");
        popup.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == MENU_OPEN_RADIO_WEB) {
                openRadioWeb();
                return true;
            }
            int index = item.getItemId() - 1;
            if (index >= 0 && index < Page.values().length) showPage(Page.values()[index]);
            return true;
        });
        popup.show();
    }

    private void openRadioWeb() {
        persistConnectionFields();
        String host = cleanHost(radioHost);
        if (host.isEmpty()) {
            toast("Nejdřív nastav IP adresu rádia.");
            showPage(Page.SETTINGS);
            return;
        }
        Intent intent = new Intent(this, ControlActivity.class);
        intent.putExtra("radio_address", host);
        startActivity(intent);
    }

    private void applySystemBarInsets(View content) {
        // Android 15+ vynucuje edge-to-edge. Obsah proto posuneme pod horní
        // stavovou lištu a nad spodní navigační lištu, místo aby je překrýval.
        if (Build.VERSION.SDK_INT < 35) return;
        content.setOnApplyWindowInsetsListener((view, insets) -> {
            android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars());
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            return insets;
        });
        content.requestApplyInsets();
    }

    private void showPage(Page page) {
        if (currentPage == Page.SETTINGS && page != Page.SETTINGS) persistConnectionFields();
        currentPage = page;
        pageTitle.setText(page.title);
        pageHost.removeAllViews();
        View view;
        switch (page) {
            case RADIO: view = buildRadioPage(); break;
            case PHONE_MUSIC: view = buildPhoneMusicPage(); break;
            case SYSTEM_AUDIO: view = buildSystemAudioPage(); break;
            case FILES: view = buildFilesPage(); break;
            case SETTINGS: view = buildSettingsPage(); break;
            case PLAYER:
            default: view = buildPlayerPage(); break;
        }
        pageHost.addView(view, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private View buildPlayerPage() {
        LinearLayout body = verticalBody();

        LinearLayout playerCard = card();
        playerCard.setGravity(Gravity.CENTER_HORIZONTAL);

        playerMode = pill("PŘIPRAVENO", ACCENT);
        playerCard.addView(playerMode, wrapCenter());

        playerCover = new ImageView(this);
        playerCover.setScaleType(ImageView.ScaleType.CENTER_CROP);
        playerCover.setImageResource(android.R.drawable.ic_media_play);
        playerCover.setBackground(round(PANEL_3, dp(18), LINE, dp(1)));
        playerCover.setPadding(dp(20), dp(20), dp(20), dp(20));
        LinearLayout.LayoutParams coverParams = new LinearLayout.LayoutParams(dp(210), dp(210));
        coverParams.gravity = Gravity.CENTER_HORIZONTAL;
        coverParams.setMargins(0, dp(12), 0, dp(14));
        playerCard.addView(playerCover, coverParams);

        TextView eyebrow = text("ORIScore radio", 12, ACCENT_2, true);
        eyebrow.setGravity(Gravity.CENTER_HORIZONTAL);
        playerCard.addView(eyebrow, matchWrap());

        playerMainTitle = text("Přehrávač připraven", 25, TEXT, true);
        playerMainTitle.setGravity(Gravity.CENTER_HORIZONTAL);
        playerMainTitle.setPadding(dp(4), dp(4), dp(4), 0);
        playerCard.addView(playerMainTitle, matchWrap());

        playerSubtitle = text("Vyber stanici a spusť přehrávání.", 14, MUTED, false);
        playerSubtitle.setGravity(Gravity.CENTER_HORIZONTAL);
        playerSubtitle.setPadding(dp(4), dp(4), dp(4), 0);
        playerCard.addView(playerSubtitle, matchWrap());

        playerDetail = text("Načítám stav…", 13, TEXT, false);
        playerDetail.setGravity(Gravity.CENTER_HORIZONTAL);
        playerDetail.setPadding(dp(10), dp(10), dp(10), dp(10));
        playerDetail.setBackground(round(PANEL_2, dp(11), LINE, dp(1)));
        LinearLayout.LayoutParams detailParams = matchWrap();
        detailParams.setMargins(0, dp(12), 0, dp(8));
        playerCard.addView(playerDetail, detailParams);

        LinearLayout batteryPanel = new LinearLayout(this);
        batteryPanel.setOrientation(LinearLayout.VERTICAL);
        batteryPanel.setPadding(dp(12), dp(9), dp(12), dp(9));
        batteryPanel.setBackground(round(PANEL_2, dp(11), LINE, dp(1)));
        LinearLayout.LayoutParams batteryPanelParams = matchWrapWithMargins(12, 2, 12, 6);
        playerCard.addView(batteryPanel, batteryPanelParams);

        playerBattery = text("🔋 Baterie: --", 14, TEXT, true);
        playerBattery.setGravity(Gravity.CENTER_HORIZONTAL);
        batteryPanel.addView(playerBattery, matchWrap());

        playerBatteryBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        playerBatteryBar.setMax(100);
        playerBatteryBar.setProgress(0);
        LinearLayout.LayoutParams batteryBarParams = matchWrapWithMargins(12, 6, 12, 0);
        batteryPanel.addView(playerBatteryBar, batteryBarParams);

        LinearLayout controls = horizontalRow();
        Button previous = circleButton("⏮", PANEL_3, 58);
        previous.setOnClickListener(v -> handleMainPrevious());
        controls.addView(previous);

        playerPlayPause = circleButton("▶", ACCENT, 72);
        playerPlayPause.setTextSize(25);
        playerPlayPause.setOnClickListener(v -> togglePlayer());
        LinearLayout.LayoutParams big = new LinearLayout.LayoutParams(dp(72), dp(72));
        big.setMargins(dp(12), 0, dp(12), 0);
        controls.addView(playerPlayPause, big);

        Button next = circleButton("⏭", PANEL_3, 58);
        next.setOnClickListener(v -> handleMainNext());
        controls.addView(next);

        Button stop = circleButton("■", DANGER, 58);
        LinearLayout.LayoutParams stopParams = new LinearLayout.LayoutParams(dp(58), dp(58));
        stopParams.setMargins(dp(12), 0, 0, 0);
        controls.addView(stop, stopParams);
        stop.setOnClickListener(v -> audioPost("/audio/stop_ajax"));

        LinearLayout.LayoutParams controlsParams = wrapCenter();
        controlsParams.setMargins(0, dp(14), 0, dp(10));
        playerCard.addView(controls, controlsParams);

        LinearLayout volumeHeader = horizontalRow();
        TextView volLabel = text("HLASITOST", 12, MUTED, true);
        volumeHeader.addView(volLabel, weighted());
        int initialVolume = lastConfig == null ? 50 : lastConfig.optInt("audioVolume", 50);
        playerVolumePercent = text(initialVolume + " %", 16, TEXT, true);
        playerVolumePercent.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        volumeHeader.addView(playerVolumePercent, weighted());
        playerCard.addView(volumeHeader, matchWrap());

        LinearLayout volumeControls = horizontalRow();
        Button volumeDown = textButton("−", PANEL_3);
        volumeDown.setTextSize(22);
        volumeDown.setOnClickListener(v -> changePlayerVolume(-1));
        volumeControls.addView(volumeDown, new LinearLayout.LayoutParams(dp(52), dp(48)));

        playerVolume = new SeekBar(this);
        playerVolume.setMax(100);
        playerVolume.setProgress(initialVolume);
        playerVolume.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                updatePlayerVolumePercent(progress);
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) { setRadioVolume(seekBar.getProgress()); }
        });
        LinearLayout.LayoutParams sliderParams = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        sliderParams.setMargins(dp(8), 0, dp(8), 0);
        volumeControls.addView(playerVolume, sliderParams);

        Button volumeUp = textButton("+", PANEL_3);
        volumeUp.setTextSize(22);
        volumeUp.setOnClickListener(v -> changePlayerVolume(1));
        volumeControls.addView(volumeUp, new LinearLayout.LayoutParams(dp(52), dp(48)));
        playerCard.addView(volumeControls, matchWrapWithMargins(0, 4, 0, 0));

        body.addView(playerCard, matchWrapWithMargins(0, 0, 0, 14));

        LinearLayout stationCard = card();
        TextView stationsTitle = text("Stanice", 20, TEXT, true);
        stationCard.addView(stationsTitle, matchWrap());
        TextView stationsNote = text("Jedním klepnutím přepneš rádio.", 13, MUTED, false);
        stationCard.addView(stationsNote, matchWrap());

        playerStationGrid = new GridLayout(this);
        playerStationGrid.setColumnCount(2);
        playerStationGrid.setUseDefaultMargins(true);
        stationCard.addView(playerStationGrid, matchWrap());
        body.addView(stationCard, matchWrap());

        refreshStatus(true);
        refreshConfig(true);
        return scroll(body);
    }

    private View buildRadioPage() {
        LinearLayout body = verticalBody();
        TextView intro = text("Stanice, MP3 streamy a loga. Vše je uložené přímo v rádiu.", 14, MUTED, false);
        body.addView(intro, matchWrapWithMargins(0, 0, 0, 10));

        LinearLayout actions = horizontalRow();
        Button refresh = textButton("Obnovit", PANEL_3);
        refresh.setOnClickListener(v -> loadRadioEditor());
        actions.addView(refresh, weighted());
        Button save = textButton("Uložit stanice", ACCENT);
        save.setOnClickListener(v -> saveRadioStations());
        actions.addView(save, weighted());
        body.addView(actions, matchWrapWithMargins(0, 0, 0, 10));

        radioEditorList = new LinearLayout(this);
        radioEditorList.setOrientation(LinearLayout.VERTICAL);
        body.addView(radioEditorList, matchWrap());
        loadRadioEditor();
        return scroll(body);
    }

    private void loadRadioEditor() {
        if (radioEditorList == null) return;
        radioEditorList.removeAllViews();
        radioEditorList.addView(loading("Načítám stanice…"), matchWrap());
        api.getJson("/api/config.json", (data, error) -> {
            if (radioEditorList == null) return;
            radioEditorList.removeAllViews();
            if (error != null) {
                radioEditorList.addView(errorBox(error.getMessage()), matchWrap());
                return;
            }
            lastConfig = data;
            JSONArray stations = data.optJSONArray("radioStations");
            cachedStations.clear();
            radioNameEditors.clear();
            radioUrlEditors.clear();
            if (stations == null) return;
            for (int i = 0; i < stations.length(); i++) {
                JSONObject station = stations.optJSONObject(i);
                if (station == null) continue;
                cachedStations.add(station);
                radioEditorList.addView(buildStationEditor(station), matchWrapWithMargins(0, 0, 0, 10));
            }
        });
    }

    private View buildStationEditor(JSONObject station) {
        int index = station.optInt("index", radioNameEditors.size());
        LinearLayout box = card();

        LinearLayout top = horizontalRow();
        ImageView logo = new ImageView(this);
        logo.setScaleType(ImageView.ScaleType.CENTER_CROP);
        logo.setImageResource(android.R.drawable.ic_media_play);
        logo.setBackground(round(PANEL_3, dp(12), LINE, dp(1)));
        top.addView(logo, new LinearLayout.LayoutParams(dp(82), dp(82)));

        LinearLayout info = new LinearLayout(this);
        info.setOrientation(LinearLayout.VERTICAL);
        info.setPadding(dp(12), 0, 0, 0);
        TextView number = text("STANICE " + (index + 1), 12, ACCENT_2, true);
        info.addView(number, matchWrap());
        TextView current = text(station.optString("name", "Stanice " + (index + 1)), 18, TEXT, true);
        info.addView(current, matchWrap());
        top.addView(info, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        box.addView(top, matchWrap());

        loadLogo(logo, station.optString("logo", ""));

        EditText name = input("Název stanice", station.optString("name", ""), false);
        radioNameEditors.add(name);
        box.addView(name, matchWrapWithMargins(0, 10, 0, 0));

        EditText url = input("HTTP MP3 URL", station.optString("url", ""), false);
        radioUrlEditors.add(url);
        box.addView(url, matchWrapWithMargins(0, 8, 0, 0));

        LinearLayout buttons = horizontalRow();
        Button play = textButton("▶ Přehrát", ACCENT);
        play.setEnabled(!station.optString("url", "").isEmpty());
        play.setOnClickListener(v -> playStation(index));
        buttons.addView(play, weighted());

        Button upload = textButton("Logo", PANEL_3);
        upload.setOnClickListener(v -> chooseStationLogo(index));
        buttons.addView(upload, weighted());

        Button delete = textButton("Smazat logo", DANGER);
        delete.setOnClickListener(v -> deleteStationLogo(index));
        buttons.addView(delete, weighted());
        box.addView(buttons, matchWrapWithMargins(0, 10, 0, 0));
        return box;
    }

    private void saveRadioStations() {
        Map<String, String> values = new LinkedHashMap<>();
        int count = Math.min(radioNameEditors.size(), radioUrlEditors.size());
        for (int i = 0; i < count; i++) {
            int index = i < cachedStations.size() ? cachedStations.get(i).optInt("index", i) : i;
            values.put("radio_name_" + index, radioNameEditors.get(i).getText().toString().trim());
            values.put("radio_url_" + index, radioUrlEditors.get(i).getText().toString().trim());
        }
        setBottomStatus("Ukládám stanice…", MUTED);
        api.postForm("/radio/save", values, (text, error) -> {
            if (error != null) setBottomStatus("Uložení selhalo: " + error.getMessage(), DANGER);
            else {
                setBottomStatus(text == null || text.trim().isEmpty() ? "Stanice uloženy" : text.trim(), OK);
                refreshConfig(true);
                loadRadioEditor();
            }
        });
    }

    private void chooseStationLogo(int index) {
        pendingLogoIndex = index;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("image/*");
        startActivityForResult(intent, PICK_STATION_LOGO);
    }

    private void deleteStationLogo(int index) {
        new AlertDialog.Builder(this)
                .setTitle("Smazat logo")
                .setMessage("Opravdu smazat logo stanice " + (index + 1) + "?")
                .setNegativeButton("Ne", null)
                .setPositiveButton("Ano", (dialog, which) -> {
                    Map<String, String> data = new LinkedHashMap<>();
                    data.put("i", String.valueOf(index));
                    api.postForm("/radio/logo/delete", data, (text, error) -> {
                        if (error != null) setBottomStatus(error.getMessage(), DANGER);
                        else {
                            api.clearBitmapCache();
                            setBottomStatus(text, OK);
                            refreshConfig(true);
                            loadRadioEditor();
                        }
                    });
                })
                .show();
    }

    private View buildPhoneMusicPage() {
        LinearLayout body = verticalBody();

        LinearLayout connectionCard = card();
        TextView heading = text("Přímé přehrávání z telefonu", 20, TEXT, true);
        connectionCard.addView(heading, matchWrap());
        phoneConnectionStatus = text(sendspin.isProtocolReady()
                ? "Připojeno přes Sendspin"
                : "Rádio se připojí automaticky při přehrávání.", 13, MUTED, false);
        connectionCard.addView(phoneConnectionStatus, matchWrap());

        LinearLayout connButtons = horizontalRow();
        Button connect = textButton("Připojit", ACCENT);
        connect.setOnClickListener(v -> connectMobileAudio(false));
        connButtons.addView(connect, weighted());
        Button disconnect = textButton("Odpojit", PANEL_3);
        disconnect.setOnClickListener(v -> {
            pendingLocalPlay = false;
            streamer.stop();
            sendspin.close();
        });
        connButtons.addView(disconnect, weighted());
        connectionCard.addView(connButtons, matchWrapWithMargins(0, 10, 0, 0));
        body.addView(connectionCard, matchWrapWithMargins(0, 0, 0, 12));

        LinearLayout player = card();
        LinearLayout importButtons = horizontalRow();
        Button choose = textButton("Vybrat skladby", ACCENT);
        choose.setOnClickListener(v -> chooseAudioFiles());
        importButtons.addView(choose, weighted());
        Button chooseFolder = textButton("Importovat složku", PANEL_3);
        chooseFolder.setOnClickListener(v -> chooseAudioFolder());
        importButtons.addView(chooseFolder, weighted());
        player.addView(importButtons, matchWrap());

        LinearLayout playbackOptions = horizontalRow();
        phoneRepeatCheck = checkBox("Opakovat playlist", phoneRepeatEnabled);
        phoneRepeatCheck.setOnCheckedChangeListener((buttonView, isChecked) -> {
            phoneRepeatEnabled = isChecked;
            preferences.edit().putBoolean("phone_repeat", isChecked).apply();
            streamer.setPlaybackOptions(phoneRepeatEnabled, phoneShuffleEnabled);
        });
        playbackOptions.addView(phoneRepeatCheck, weighted());
        phoneShuffleCheck = checkBox("Náhodné pořadí", phoneShuffleEnabled);
        phoneShuffleCheck.setOnCheckedChangeListener((buttonView, isChecked) -> {
            phoneShuffleEnabled = isChecked;
            preferences.edit().putBoolean("phone_shuffle", isChecked).apply();
            streamer.setPlaybackOptions(phoneRepeatEnabled, phoneShuffleEnabled);
        });
        playbackOptions.addView(phoneShuffleCheck, weighted());
        player.addView(playbackOptions, matchWrapWithMargins(0, 8, 0, 0));

        phoneTrackTitle = text(playlistNames.isEmpty() ? "Není vybraná skladba" : playlistNames.get(Math.max(0, Math.min(selectedTrackIndex, playlistNames.size() - 1))), 20, TEXT, true);
        phoneTrackTitle.setGravity(Gravity.CENTER_HORIZONTAL);
        phoneTrackTitle.setPadding(0, dp(14), 0, dp(6));
        player.addView(phoneTrackTitle, matchWrap());

        phoneProgress = text("00:00 / 00:00", 13, MUTED, false);
        phoneProgress.setGravity(Gravity.CENTER_HORIZONTAL);
        player.addView(phoneProgress, matchWrap());
        phoneProgressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        phoneProgressBar.setMax(1000);
        player.addView(phoneProgressBar, matchWrap());

        LinearLayout controls = horizontalRow();
        Button prev = textButton("⏮", PANEL_3);
        prev.setOnClickListener(v -> streamer.previous());
        controls.addView(prev, weighted());
        Button play = textButton("▶ Přehrát", ACCENT);
        play.setOnClickListener(v -> playLocalSelection());
        controls.addView(play, weighted());
        phonePauseButton = textButton("Pauza", PANEL_3);
        phonePauseButton.setOnClickListener(v -> {
            streamer.pauseOrResume();
            phonePauseButton.setText(streamer.isPaused() ? "Pokračovat" : "Pauza");
            updateMobileMediaNotification();
        });
        controls.addView(phonePauseButton, weighted());
        Button next = textButton("⏭", PANEL_3);
        next.setOnClickListener(v -> streamer.next());
        controls.addView(next, weighted());
        player.addView(controls, matchWrapWithMargins(0, 10, 0, 0));

        Button stop = textButton("■ Stop", DANGER);
        stop.setOnClickListener(v -> streamer.stop());
        player.addView(stop, matchWrapWithMargins(0, 8, 0, 0));
        body.addView(player, matchWrapWithMargins(0, 0, 0, 12));

        LinearLayout listCard = card();
        listCard.addView(text("Playlist", 20, TEXT, true), matchWrap());
        phonePlaylistView = new ListView(this);
        phonePlaylistView.setChoiceMode(ListView.CHOICE_MODE_SINGLE);
        phonePlaylistView.setDividerHeight(dp(1));
        phonePlaylistView.setBackgroundColor(PANEL);
        phonePlaylistView.setVerticalScrollBarEnabled(true);
        phonePlaylistView.setNestedScrollingEnabled(true);
        phonePlaylistView.setOnTouchListener((view, event) -> {
            ViewParent parent = view.getParent();
            if (parent != null) {
                int action = event.getActionMasked();
                boolean insidePlaylist = action != MotionEvent.ACTION_UP
                        && action != MotionEvent.ACTION_CANCEL;
                parent.requestDisallowInterceptTouchEvent(insidePlaylist);
            }
            return false;
        });
        phonePlaylistAdapter = new ArrayAdapter<String>(this, android.R.layout.simple_list_item_single_choice, playlistNames) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                TextView view = (TextView) super.getView(position, convertView, parent);
                view.setTextColor(TEXT);
                view.setTextSize(15);
                view.setPadding(dp(12), dp(10), dp(12), dp(10));
                return view;
            }
        };
        phonePlaylistView.setAdapter(phonePlaylistAdapter);
        phonePlaylistView.setOnItemClickListener((parent, view, position, id) -> {
            selectedTrackIndex = position;
            phoneTrackTitle.setText(playlistNames.get(position));
        });
        if (!playlistNames.isEmpty()) phonePlaylistView.setItemChecked(selectedTrackIndex, true);
        listCard.addView(phonePlaylistView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(300)));
        body.addView(listCard, matchWrap());
        return scroll(body);
    }

    private void playLocalSelection() {
        if (playlistUris.isEmpty()) {
            toast("Nejdřív vyber skladby");
            return;
        }
        if (sendspin.isProtocolReady()) {
            streamer.play(playlistUris, playlistNames, selectedTrackIndex, phoneRepeatEnabled, phoneShuffleEnabled);
            return;
        }
        pendingLocalPlay = true;
        sendspinRetryCount = 0;
        connectMobileAudio(true);
    }

    private void connectMobileAudio(boolean forPlayback) {
        if (forPlayback) pendingLocalPlay = true;
        if (sendspin.isConnected()) sendspin.close();
        if (phoneConnectionStatus != null) phoneConnectionStatus.setText("Připojuji " + radioHost + ":" + sendspinPort + "…");
        sendspin.connectAsync(radioHost, sendspinPort);
    }

    private View buildSystemAudioPage() {
        LinearLayout body = verticalBody();
        LinearLayout card = card();
        card.addView(text("Stream systémového zvuku", 21, TEXT, true), matchWrap());
        card.addView(text(
                "Android zachytí zvuk aplikací, které to dovolují, a pošle ho přes Wi‑Fi přímo do rádia. " +
                        "Po spuštění Android zobrazí potvrzení sdílení.",
                14, MUTED, false), matchWrapWithMargins(0, 6, 0, 12));

        systemAudioStatus = text("Stream je zastavený.", 14, TEXT, false);
        systemAudioStatus.setPadding(dp(12), dp(10), dp(12), dp(10));
        systemAudioStatus.setBackground(round(PANEL_2, dp(10), LINE, dp(1)));
        card.addView(systemAudioStatus, matchWrap());

        systemAudioStart = textButton("Spustit stream zvuku", ACCENT);
        systemAudioStart.setOnClickListener(v -> requestSystemAudioCapture());
        card.addView(systemAudioStart, matchWrapWithMargins(0, 12, 0, 0));

        systemAudioStop = textButton("Zastavit stream", DANGER);
        systemAudioStop.setEnabled(false);
        systemAudioStop.setOnClickListener(v -> stopSystemAudioCapture());
        card.addView(systemAudioStop, matchWrapWithMargins(0, 8, 0, 0));
        body.addView(card, matchWrap());

        LinearLayout note = card();
        note.addView(text("Co Android pustí", 18, TEXT, true), matchWrap());
        note.addView(text(
                "Hudba, prohlížeč, hry a video obvykle fungují. Aplikace může zachycení svého zvuku zakázat; " +
                        "v takovém případě rádio dostane ticho. Aplikace ukazuje živou úroveň zachyceného zvuku.",
                13, MUTED, false), matchWrap());
        body.addView(note, matchWrapWithMargins(0, 12, 0, 0));
        return scroll(body);
    }

    private View buildFilesPage() {
        LinearLayout body = verticalBody();
        LinearLayout toolbar = card();
        toolbar.addView(text("Soubory v rádiu", 20, TEXT, true), matchWrap());

        LinearLayout disks = horizontalRow();
        Button ffat = textButton("Interní FFat", filesDisk.equals("ffat") ? ACCENT : PANEL_3);
        ffat.setOnClickListener(v -> { filesDisk = "ffat"; filesPath = "/"; showPage(Page.FILES); });
        disks.addView(ffat, weighted());
        Button usb = textButton("USB disk", filesDisk.equals("usb0") ? ACCENT : PANEL_3);
        usb.setOnClickListener(v -> { filesDisk = "usb0"; filesPath = "/"; showPage(Page.FILES); });
        disks.addView(usb, weighted());
        toolbar.addView(disks, matchWrapWithMargins(0, 8, 0, 0));

        filesPathLabel = text(filesDisk + ": " + filesPath, 13, ACCENT_2, true);
        toolbar.addView(filesPathLabel, matchWrapWithMargins(0, 10, 0, 0));

        LinearLayout pathActions = horizontalRow();
        Button up = textButton("↑ Nahoru", PANEL_3);
        up.setOnClickListener(v -> { filesPath = parentPath(filesPath); loadFiles(); });
        pathActions.addView(up, weighted());
        Button folderPlay = textButton("▶ Přehrát složku", ACCENT);
        folderPlay.setOnClickListener(v -> api.getText(
                "/audio/play_folder?disk=" + enc(filesDisk) + "&p=" + enc(filesPath),
                (text, error) -> actionResult(text, error)));
        pathActions.addView(folderPlay, weighted());
        Button refresh = textButton("Obnovit", PANEL_3);
        refresh.setOnClickListener(v -> loadFiles());
        pathActions.addView(refresh, weighted());
        toolbar.addView(pathActions, matchWrapWithMargins(0, 8, 0, 0));

        Button uploadFiles = textButton("⬆ Nahrát soubory", ACCENT);
        uploadFiles.setOnClickListener(v -> chooseFilesForUpload());
        toolbar.addView(uploadFiles, matchWrapWithMargins(0, 8, 0, 0));
        body.addView(toolbar, matchWrapWithMargins(0, 0, 0, 10));

        filesList = new LinearLayout(this);
        filesList.setOrientation(LinearLayout.VERTICAL);
        body.addView(filesList, matchWrap());
        loadFiles();
        return scroll(body);
    }

    private void loadFiles() {
        if (filesList == null) return;
        filesPathLabel.setText(filesDisk + ": " + filesPath);
        filesList.removeAllViews();
        filesList.addView(loading("Načítám soubory…"), matchWrap());
        String path = "/api/files.json?disk=" + enc(filesDisk) + "&p=" + enc(filesPath);
        api.getJson(path, (data, error) -> {
            if (filesList == null) return;
            filesList.removeAllViews();
            if (error != null) {
                filesList.addView(errorBox(error.getMessage()), matchWrap());
                return;
            }
            if (!data.optBoolean("available", true)) {
                filesList.addView(errorBox("Disk není dostupný"), matchWrap());
                return;
            }
            filesPath = data.optString("path", filesPath);
            filesPathLabel.setText(filesDisk + ": " + filesPath);
            JSONArray items = data.optJSONArray("items");
            if (items == null || items.length() == 0) {
                filesList.addView(text("Složka je prázdná.", 14, MUTED, false), matchWrap());
                return;
            }
            for (int i = 0; i < items.length(); i++) {
                JSONObject item = items.optJSONObject(i);
                if (item != null) filesList.addView(buildFileRow(item), matchWrapWithMargins(0, 0, 0, 6));
            }
        });
    }

    private View buildFileRow(JSONObject item) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12), dp(10), dp(10), dp(10));
        row.setBackground(round(PANEL, dp(11), LINE, dp(1)));

        boolean dir = item.optBoolean("dir", false);
        String name = item.optString("name", "");
        String path = item.optString("path", "");
        String ext = item.optString("ext", "").toLowerCase(Locale.ROOT);

        TextView icon = text(dir ? "📁" : fileIcon(ext), 23, TEXT, false);
        row.addView(icon, new LinearLayout.LayoutParams(dp(42), dp(42)));

        LinearLayout info = new LinearLayout(this);
        info.setOrientation(LinearLayout.VERTICAL);
        info.addView(text(name, 15, TEXT, true), matchWrap());
        if (!dir) {
            String detail = niceBytes(item.optLong("size", 0));
            if (!ext.isEmpty()) detail += " · " + ext.toUpperCase(Locale.ROOT);
            info.addView(text(detail, 11, MUTED, false), matchWrap());
        }
        info.setOnClickListener(v -> {
            if (dir) {
                filesPath = path;
                loadFiles();
            } else {
                openFilePreview(item);
            }
        });
        row.addView(info, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        Button action = textButton("Akce", PANEL_3);
        action.setOnClickListener(v -> showFileActions(action, item));
        row.addView(action, new LinearLayout.LayoutParams(dp(94), dp(46)));
        return row;
    }

    private void showFileActions(View anchor, JSONObject item) {
        boolean dir = item.optBoolean("dir", false);
        String ext = item.optString("ext", "").toLowerCase(Locale.ROOT);
        PopupMenu menu = new PopupMenu(this, anchor);
        menu.getMenu().add(0, 1, 1, dir ? "Otevřít složku" :
                (isHtmlFileExt(ext) ? "Spustit HTML / minihru" : "Otevřít / náhled"));
        if (!dir && isAudioFileExt(ext)) menu.getMenu().add(0, 2, 2, "Přehrát v rádiu");
        if (!dir && isTextFileExt(ext)) menu.getMenu().add(0, 3, 3, "Editovat");
        if (!dir) menu.getMenu().add(0, 4, 4, "Stáhnout do telefonu");
        menu.getMenu().add(0, 5, 5, "Přejmenovat");
        menu.getMenu().add(0, 6, 6, dir ? "Smazat složku" : "Smazat soubor");
        menu.setOnMenuItemClickListener(menuItem -> {
            switch (menuItem.getItemId()) {
                case 1:
                    if (dir) {
                        filesPath = item.optString("path", "/");
                        loadFiles();
                    } else {
                        openFilePreview(item);
                    }
                    return true;
                case 2:
                    playFileOnRadio(item.optString("path", ""));
                    return true;
                case 3:
                    editTextFile(item.optString("name", "Soubor"), item.optString("path", ""));
                    return true;
                case 4:
                    beginFileDownload(item);
                    return true;
                case 5:
                    renameFileOrFolder(item);
                    return true;
                case 6:
                    confirmDeleteFileOrFolder(item);
                    return true;
                default:
                    return false;
            }
        });
        menu.show();
    }

    private void openFilePreview(JSONObject item) {
        String name = item.optString("name", "Soubor");
        String path = item.optString("path", "");
        String ext = item.optString("ext", "").toLowerCase(Locale.ROOT);
        String rawPath = "/raw?disk=" + enc(filesDisk) + "&f=" + enc(path);

        if (isHtmlFileExt(ext)) {
            openHtmlFile(name, path);
            return;
        }

        if (isTextFileExt(ext)) {
            setBottomStatus("Načítám " + name + "…", MUTED);
            api.getText(rawPath, (content, error) -> {
                if (error != null) {
                    setBottomStatus("Soubor nelze otevřít: " + error.getMessage(), DANGER);
                    return;
                }
                showTextPreviewDialog(name, path, content);
            });
            return;
        }

        if (isImageFileExt(ext)) {
            setBottomStatus("Načítám obrázek…", MUTED);
            api.loadBitmap(rawPath, (bitmap, error) -> {
                if (error != null || bitmap == null) {
                    setBottomStatus("Obrázek nelze otevřít", DANGER);
                    return;
                }
                ImageView image = new ImageView(this);
                image.setAdjustViewBounds(true);
                image.setScaleType(ImageView.ScaleType.FIT_CENTER);
                image.setImageBitmap(bitmap);
                image.setPadding(dp(8), dp(8), dp(8), dp(8));
                ScrollView scroll = new ScrollView(this);
                scroll.setBackgroundColor(BG);
                scroll.addView(image, new ScrollView.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
                new AlertDialog.Builder(this)
                        .setTitle(name)
                        .setView(scroll)
                        .setNegativeButton("Zavřít", null)
                        .show();
                setBottomStatus(path, OK);
            });
            return;
        }

        if (isAudioFileExt(ext)) {
            new AlertDialog.Builder(this)
                    .setTitle(name)
                    .setMessage(filesDisk + ": " + path + "\n\nAudio soubor můžeš přehrát přímo v rádiu.")
                    .setNegativeButton("Zavřít", null)
                    .setPositiveButton("Přehrát", (dialog, which) -> playFileOnRadio(path))
                    .show();
            return;
        }

        new AlertDialog.Builder(this)
                .setTitle(name)
                .setMessage(filesDisk + ": " + path + "\n\nVelikost: " + niceBytes(item.optLong("size", 0)) +
                        (ext.isEmpty() ? "" : "\nTyp: " + ext.toUpperCase(Locale.ROOT)))
                .setPositiveButton("Zavřít", null)
                .show();
    }

    private void openHtmlFile(String name, String path) {
        if (path == null || path.trim().isEmpty()) {
            setBottomStatus("HTML soubor nemá platnou cestu", DANGER);
            return;
        }
        Intent intent = new Intent(this, HtmlGameActivity.class);
        intent.putExtra("radio_address", radioHost);
        intent.putExtra("disk", filesDisk);
        intent.putExtra("path", path);
        intent.putExtra("title", name);
        startActivity(intent);
        setBottomStatus("Spouštím " + name + "…", OK);
    }

    private void showTextPreviewDialog(String name, String path, String content) {
        TextView preview = text(content == null ? "" : content, 13, TEXT, false);
        preview.setTypeface(Typeface.MONOSPACE);
        preview.setTextIsSelectable(true);
        preview.setPadding(dp(12), dp(12), dp(12), dp(12));
        preview.setBackgroundColor(BG_SOFT);
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BG_SOFT);
        scroll.addView(preview, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(name)
                .setView(scroll)
                .setNegativeButton("Zavřít", null)
                .setPositiveButton("Editovat", null)
                .create();
        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_POSITIVE)
                .setOnClickListener(v -> {
                    dialog.dismiss();
                    showTextEditorDialog(name, path, content);
                }));
        dialog.show();
        setBottomStatus(filesDisk + ": " + path, OK);
    }

    private void editTextFile(String name, String path) {
        String rawPath = "/raw?disk=" + enc(filesDisk) + "&f=" + enc(path);
        setBottomStatus("Načítám editor…", MUTED);
        api.getText(rawPath, (content, error) -> {
            if (error != null) {
                setBottomStatus("Editor: " + error.getMessage(), DANGER);
                return;
            }
            showTextEditorDialog(name, path, content);
        });
    }

    private void showTextEditorDialog(String name, String path, String content) {
        EditText editor = new EditText(this);
        editor.setText(content == null ? "" : content);
        editor.setTextColor(TEXT);
        editor.setHintTextColor(MUTED);
        editor.setTextSize(13);
        editor.setTypeface(Typeface.MONOSPACE);
        editor.setGravity(Gravity.TOP | Gravity.START);
        editor.setMinLines(16);
        editor.setHorizontallyScrolling(true);
        editor.setInputType(InputType.TYPE_CLASS_TEXT |
                InputType.TYPE_TEXT_FLAG_MULTI_LINE |
                InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        editor.setPadding(dp(12), dp(12), dp(12), dp(12));
        editor.setBackgroundColor(BG_SOFT);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(BG_SOFT);
        scroll.addView(editor, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Editor: " + name)
                .setView(scroll)
                .setNegativeButton("Zrušit", null)
                .setPositiveButton("Uložit", null)
                .create();
        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_POSITIVE)
                .setOnClickListener(v -> {
                    Map<String, String> values = new LinkedHashMap<>();
                    values.put("content", editor.getText().toString());
                    dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
                    setBottomStatus("Ukládám " + name + "…", MUTED);
                    api.postForm("/save?disk=" + enc(filesDisk) + "&f=" + enc(path), values,
                            (result, error) -> {
                                if (error != null) {
                                    dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
                                    setBottomStatus("Uložení selhalo: " + error.getMessage(), DANGER);
                                } else {
                                    dialog.dismiss();
                                    setBottomStatus("Soubor uložen: " + path, OK);
                                    loadFiles();
                                }
                            });
                }));
        dialog.show();
    }

    private void playFileOnRadio(String path) {
        api.getText("/audio/play?disk=" + enc(filesDisk) + "&f=" + enc(path),
                (result, error) -> {
                    actionResult(result, error);
                    if (error == null) mainHandler.postDelayed(() -> refreshStatus(false), 250L);
                });
    }

    private static boolean isHtmlFileExt(String ext) {
        return ext.equals("html") || ext.equals("htm");
    }

    private static boolean isTextFileExt(String ext) {
        return ext.equals("txt") || ext.equals("md") || ext.equals("ini") || ext.equals("cfg") ||
                ext.equals("log") || ext.equals("json") || ext.equals("ock") || ext.equals("xml") ||
                ext.equals("csv") || ext.equals("html") || ext.equals("htm") || ext.equals("css") ||
                ext.equals("js") || ext.equals("ino") || ext.equals("cpp") || ext.equals("h") ||
                ext.equals("hpp") || ext.equals("c") || ext.equals("py") || ext.equals("sh") ||
                ext.equals("bat") || ext.equals("yml") || ext.equals("yaml");
    }

    private static boolean isImageFileExt(String ext) {
        return ext.equals("jpg") || ext.equals("jpeg") || ext.equals("png") || ext.equals("gif") ||
                ext.equals("bmp") || ext.equals("webp");
    }

    private static boolean isAudioFileExt(String ext) {
        return ext.equals("mp3") || ext.equals("wav") || ext.equals("ogg") || ext.equals("m4a") ||
                ext.equals("aac") || ext.equals("flac");
    }

    private static String fileIcon(String ext) {
        if (isHtmlFileExt(ext)) return "▶";
        if (isAudioFileExt(ext)) return "♫";
        if (isImageFileExt(ext)) return "▧";
        if (isTextFileExt(ext)) return "✎";
        return "•";
    }

    private void chooseFilesForUpload() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(intent, PICK_FILE_UPLOAD);
    }

    private void queueSelectedUploads(Intent data) {
        pendingUploadUris.clear();
        pendingUploadIndex = 0;
        ClipData clip = data.getClipData();
        if (clip != null) {
            for (int i = 0; i < clip.getItemCount(); i++) {
                Uri uri = clip.getItemAt(i).getUri();
                if (uri != null) pendingUploadUris.add(uri);
            }
        } else if (data.getData() != null) {
            pendingUploadUris.add(data.getData());
        }
        if (pendingUploadUris.isEmpty()) {
            setBottomStatus("Nebyl vybrán žádný soubor", WARN);
            return;
        }
        uploadNextFile();
    }

    private void uploadNextFile() {
        if (pendingUploadIndex >= pendingUploadUris.size()) {
            int count = pendingUploadUris.size();
            pendingUploadUris.clear();
            pendingUploadIndex = 0;
            setBottomStatus("Nahráno souborů: " + count, OK);
            loadFiles();
            return;
        }
        Uri uri = pendingUploadUris.get(pendingUploadIndex);
        String name = displayName(uri);
        setBottomStatus("Nahrávám " + (pendingUploadIndex + 1) + "/" + pendingUploadUris.size() + ": " + name, MUTED);
        String target = "/upload?disk=" + enc(filesDisk) + "&p=" + enc(filesPath);
        api.upload(uri, "file", target, (result, error) -> {
            if (error != null) {
                setBottomStatus("Nahrání selhalo: " + name + " · " + error.getMessage(), DANGER);
                pendingUploadUris.clear();
                pendingUploadIndex = 0;
                return;
            }
            pendingUploadIndex++;
            uploadNextFile();
        });
    }

    private void beginFileDownload(JSONObject item) {
        pendingDownloadDisk = filesDisk;
        pendingDownloadPath = item.optString("path", "");
        pendingDownloadName = item.optString("name", fileName(pendingDownloadPath));
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/octet-stream");
        intent.putExtra(Intent.EXTRA_TITLE, pendingDownloadName);
        startActivityForResult(intent, CREATE_FILE_DOWNLOAD);
    }

    private void finishFileDownload(Uri target) {
        if (target == null || pendingDownloadPath.isEmpty()) return;
        String source = "/download?disk=" + enc(pendingDownloadDisk) + "&f=" + enc(pendingDownloadPath);
        String name = pendingDownloadName;
        setBottomStatus("Stahuji " + name + "…", MUTED);
        api.download(source, target, (result, error) -> {
            if (error != null) setBottomStatus("Stažení selhalo: " + error.getMessage(), DANGER);
            else setBottomStatus("Staženo: " + name, OK);
            pendingDownloadDisk = "";
            pendingDownloadPath = "";
            pendingDownloadName = "";
        });
    }

    private void renameFileOrFolder(JSONObject item) {
        String oldPath = item.optString("path", "");
        String oldName = item.optString("name", fileName(oldPath));
        EditText nameInput = input("Nový název", oldName, false);
        nameInput.setSelectAllOnFocus(true);
        LinearLayout box = new LinearLayout(this);
        box.setPadding(dp(18), dp(4), dp(18), 0);
        box.addView(nameInput, matchWrap());
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Přejmenovat")
                .setView(box)
                .setNegativeButton("Zrušit", null)
                .setPositiveButton("Přejmenovat", null)
                .create();
        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            String newName = nameInput.getText().toString().trim();
            if (newName.isEmpty() || newName.contains("/") || newName.contains("\\") || newName.contains("..")) {
                nameInput.setError("Neplatný název");
                return;
            }
            Map<String, String> values = new LinkedHashMap<>();
            values.put("disk", filesDisk);
            values.put("p", filesPath);
            values.put("f", oldPath);
            values.put("name", newName);
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
            setBottomStatus("Přejmenovávám " + oldName + "…", MUTED);
            api.postForm("/rename", values, (result, error) -> {
                if (error != null) {
                    dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
                    setBottomStatus("Přejmenování selhalo: " + error.getMessage(), DANGER);
                } else {
                    dialog.dismiss();
                    setBottomStatus("Přejmenováno na " + newName, OK);
                    loadFiles();
                }
            });
        }));
        dialog.show();
    }

    private void confirmDeleteFileOrFolder(JSONObject item) {
        boolean dir = item.optBoolean("dir", false);
        String name = item.optString("name", "Položka");
        String path = item.optString("path", "");
        new AlertDialog.Builder(this)
                .setTitle(dir ? "Smazat složku" : "Smazat soubor")
                .setMessage("Opravdu smazat " + name + (dir ? " včetně jejího obsahu?" : "?"))
                .setNegativeButton("Ne", null)
                .setPositiveButton("Smazat", (dialog, which) -> {
                    setBottomStatus("Mažu " + name + "…", MUTED);
                    String request = "/delete?disk=" + enc(filesDisk) + "&p=" + enc(filesPath) + "&f=" + enc(path);
                    api.getText(request, (result, error) -> {
                        if (error != null) setBottomStatus("Mazání selhalo: " + error.getMessage(), DANGER);
                        else {
                            setBottomStatus("Smazáno: " + name, OK);
                            loadFiles();
                        }
                    });
                })
                .show();
    }

    private View buildKaraokePage() {
        LinearLayout body = verticalBody();
        LinearLayout toolbar = card();
        toolbar.addView(text("Karaoke knihovna", 20, TEXT, true), matchWrap());

        karaokeDiskSpinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(this,
                android.R.layout.simple_spinner_dropdown_item,
                new String[]{"Všechny disky", "USB disk", "Interní FFat"}) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                TextView view = (TextView) super.getView(position, convertView, parent);
                view.setTextColor(TEXT);
                return view;
            }
        };
        karaokeDiskSpinner.setAdapter(adapter);
        toolbar.addView(karaokeDiskSpinner, matchWrapWithMargins(0, 8, 0, 0));

        Button load = textButton("Načíst karaoke", ACCENT);
        load.setOnClickListener(v -> loadKaraoke());
        toolbar.addView(load, matchWrapWithMargins(0, 8, 0, 0));
        body.addView(toolbar, matchWrapWithMargins(0, 0, 0, 10));

        karaokeList = new LinearLayout(this);
        karaokeList.setOrientation(LinearLayout.VERTICAL);
        body.addView(karaokeList, matchWrap());
        loadKaraoke();
        return scroll(body);
    }

    private void loadKaraoke() {
        if (karaokeList == null) return;
        karaokeList.removeAllViews();
        karaokeList.addView(loading("Hledám karaoke soubory…"), matchWrap());
        String disk = "all";
        if (karaokeDiskSpinner != null) {
            int p = karaokeDiskSpinner.getSelectedItemPosition();
            if (p == 1) disk = "usb0";
            else if (p == 2) disk = "ffat";
        }
        api.getArray("/karaoke/list.json?disk=" + enc(disk), (items, error) -> {
            if (karaokeList == null) return;
            karaokeList.removeAllViews();
            if (error != null) {
                karaokeList.addView(errorBox(error.getMessage()), matchWrap());
                return;
            }
            if (items == null || items.length() == 0) {
                karaokeList.addView(text("Žádné karaoke skladby.", 14, MUTED, false), matchWrap());
                return;
            }
            for (int i = 0; i < items.length(); i++) {
                JSONObject item = items.optJSONObject(i);
                if (item != null) karaokeList.addView(buildKaraokeRow(item), matchWrapWithMargins(0, 0, 0, 6));
            }
        });
    }

    private View buildKaraokeRow(JSONObject item) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12), dp(10), dp(10), dp(10));
        row.setBackground(round(PANEL, dp(11), LINE, dp(1)));

        LinearLayout info = new LinearLayout(this);
        info.setOrientation(LinearLayout.VERTICAL);
        String name = item.optString("name", fileName(item.optString("path", "Karaoke")));
        info.addView(text(name, 15, TEXT, true), matchWrap());
        info.addView(text(item.optString("disk", "") + ": " + item.optString("path", ""), 11, MUTED, false), matchWrap());
        row.addView(info, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        Button play = textButton("▶", ACCENT);
        play.setOnClickListener(v -> playKaraoke(item));
        row.addView(play, new LinearLayout.LayoutParams(dp(60), dp(46)));
        return row;
    }

    private void playKaraoke(JSONObject item) {
        String disk = item.optString("disk", "usb0");
        String jsonPath = item.optString("path", "");
        String fallbackTitle = item.optString("name", fileName(jsonPath));
        api.getJson("/raw?disk=" + enc(disk) + "&f=" + enc(jsonPath), (doc, error) -> {
            if (error != null) {
                setBottomStatus("Karaoke JSON: " + error.getMessage(), DANGER);
                return;
            }
            String audio = firstString(doc, "audio", "Audio");
            String title = firstString(doc, "title", "Title");
            if (title.isEmpty()) title = fallbackTitle;
            Map<String, String> values = new LinkedHashMap<>();
            values.put("disk", disk);
            values.put("json", jsonPath);
            values.put("audio", audio);
            values.put("title", title);
            api.postForm("/karaoke/play", values, (text, playError) -> actionResult(text, playError));
        });
    }

    private View buildSettingsPage() {
        LinearLayout body = verticalBody();

        LinearLayout connection = card();
        connection.addView(text("Připojení aplikace", 21, TEXT, true), matchWrap());
        connection.addView(text(
                "Adresa rádia, Sendspin port a přihlašovací údaje se ukládají v telefonu a po dalším spuštění se automaticky obnoví.",
                13, MUTED, false), matchWrapWithMargins(0, 4, 0, 10));

        settingsHost = input("IP adresa nebo mDNS jméno", radioHost, false);
        connection.addView(settingsHost, matchWrap());
        settingsPort = input("Sendspin port", String.valueOf(sendspinPort), false);
        settingsPort.setInputType(InputType.TYPE_CLASS_NUMBER);
        connection.addView(settingsPort, matchWrapWithMargins(0, 8, 0, 0));
        settingsUser = input("Web uživatel", webUser, false);
        connection.addView(settingsUser, matchWrapWithMargins(0, 8, 0, 0));
        settingsPassword = input("Web heslo", webPassword, true);
        connection.addView(settingsPassword, matchWrapWithMargins(0, 8, 0, 0));

        LinearLayout buttons = horizontalRow();
        Button save = textButton("Uložit a otestovat", ACCENT);
        save.setOnClickListener(v -> saveAndTestSettings());
        buttons.addView(save, weighted());
        Button find = textButton("Najít rádio", PANEL_3);
        find.setOnClickListener(v -> startDiscovery());
        buttons.addView(find, weighted());
        connection.addView(buttons, matchWrapWithMargins(0, 10, 0, 0));

        settingsTestResult = text("", 13, MUTED, false);
        settingsTestResult.setPadding(dp(10), dp(8), dp(10), dp(8));
        connection.addView(settingsTestResult, matchWrap());
        body.addView(connection, matchWrapWithMargins(0, 0, 0, 12));

        LinearLayout wifi = card();
        wifi.addView(text("Uložené Wi‑Fi sítě", 20, TEXT, true), matchWrap());
        wifi.addView(text(
                "Rádio se automaticky připojuje k nejsilnější uložené síti. Při vyhledávání musí být internetové rádio zastavené.",
                13, MUTED, false), matchWrapWithMargins(0, 4, 0, 10));

        Button scan = textButton("Vyhledat okolní sítě", PANEL_3);
        scan.setOnClickListener(v -> startWifiScan());
        wifi.addView(scan, matchWrap());
        wifiScanStatus = text("", 12, MUTED, false);
        wifi.addView(wifiScanStatus, matchWrapWithMargins(0, 6, 0, 0));

        wifiScanSpinner = new Spinner(this);
        wifiScanSpinner.setAdapter(darkSpinnerAdapter(new ArrayList<String>() {{ add("Nejdřív spusť vyhledávání"); }}));
        wifiScanSpinner.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(android.widget.AdapterView<?> parent, View view, int position, long id) {
                if (position >= 0 && position < scannedWifiNetworks.size() && wifiSsidInput != null) {
                    wifiSsidInput.setText(scannedWifiNetworks.get(position).optString("ssid", ""));
                }
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        wifi.addView(wifiScanSpinner, matchWrapWithMargins(0, 8, 0, 0));

        wifiSsidInput = input("SSID", "", false);
        wifi.addView(wifiSsidInput, matchWrapWithMargins(0, 8, 0, 0));
        wifiPasswordInput = input("Heslo Wi‑Fi (prázdné = ponechat původní)", "", true);
        wifi.addView(wifiPasswordInput, matchWrapWithMargins(0, 8, 0, 0));
        Button addWifi = textButton("Přidat / aktualizovat a připojit", ACCENT);
        addWifi.setOnClickListener(v -> addOrUpdateWifi());
        wifi.addView(addWifi, matchWrapWithMargins(0, 8, 0, 0));

        savedWifiList = new LinearLayout(this);
        savedWifiList.setOrientation(LinearLayout.VERTICAL);
        wifi.addView(savedWifiList, matchWrapWithMargins(0, 12, 0, 0));
        body.addView(wifi, matchWrapWithMargins(0, 0, 0, 12));

        LinearLayout ap = card();
        ap.addView(text("AP hotspot a mDNS", 20, TEXT, true), matchWrap());
        apModeSpinner = new Spinner(this);
        apModeSpinner.setAdapter(darkSpinnerAdapter(java.util.Arrays.asList(
                "Vždy zapnuto", "Jen bez připojené Wi‑Fi", "Vždy vypnuto")));
        ap.addView(apModeSpinner, matchWrapWithMargins(0, 8, 0, 0));
        apSsidInput = input("AP SSID", "", false);
        ap.addView(apSsidInput, matchWrapWithMargins(0, 8, 0, 0));
        apPasswordInput = input("AP heslo", "", true);
        ap.addView(apPasswordInput, matchWrapWithMargins(0, 8, 0, 0));
        mdnsInput = input("mDNS jméno, např. oris-radio", "", false);
        ap.addView(mdnsInput, matchWrapWithMargins(0, 8, 0, 0));
        ap.addView(text(
                "Heslo hotspotu musí mít alespoň 8 znaků; prázdné znamená otevřený hotspot.",
                12, MUTED, false), matchWrapWithMargins(0, 8, 0, 0));
        body.addView(ap, matchWrapWithMargins(0, 0, 0, 12));

        LinearLayout access = card();
        access.addView(text("Web a FTP", 20, TEXT, true), matchWrap());
        access.addView(text(
                "Webový login používá aplikace i pro API. Po změně se nové údaje automaticky uloží také v telefonu.",
                12, MUTED, false), matchWrapWithMargins(0, 4, 0, 8));
        ftpEnabledCheck = checkBox("Zapnout FTP server", false);
        access.addView(ftpEnabledCheck, matchWrap());
        ftpUserInput = input("FTP uživatel", "", false);
        access.addView(ftpUserInput, matchWrapWithMargins(0, 8, 0, 0));
        ftpPasswordInput = input("FTP heslo", "", true);
        access.addView(ftpPasswordInput, matchWrapWithMargins(0, 8, 0, 0));
        ftpDiskSpinner = new Spinner(this);
        ftpDiskSpinner.setAdapter(darkSpinnerAdapter(java.util.Arrays.asList("Interní FFat", "USB disk")));
        access.addView(ftpDiskSpinner, matchWrapWithMargins(0, 8, 0, 0));
        access.addView(text("FTP port 21, PASV 50009.", 12, MUTED, false), matchWrapWithMargins(0, 6, 0, 0));
        body.addView(access, matchWrapWithMargins(0, 0, 0, 12));

        LinearLayout audio = card();
        audio.addView(text("Zvuk, gain a 10pásmový ekvalizér", 20, TEXT, true), matchWrap());
        rgbEnabledCheck = checkBox("RGB LED podle hudby", false);
        audio.addView(rgbEnabledCheck, matchWrap());

        configVolumeValue = text("Hlasitost: 0 %", 13, TEXT, true);
        audio.addView(configVolumeValue, matchWrapWithMargins(0, 8, 0, 0));
        configVolumeSeek = new SeekBar(this);
        configVolumeSeek.setMax(100);
        configVolumeSeek.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override public void changed(int value, boolean fromUser) {
                configVolumeValue.setText("Hlasitost: " + value + " %");
            }
        });
        audio.addView(configVolumeSeek, matchWrap());

        outputGainValue = text("Maximální výstupní gain: -12 dB", 13, TEXT, true);
        audio.addView(outputGainValue, matchWrapWithMargins(0, 10, 0, 0));
        outputGainSeek = new SeekBar(this);
        outputGainSeek.setMax(40);
        outputGainSeek.setProgress(28);
        outputGainSeek.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override public void changed(int value, boolean fromUser) {
                updateDetailedEqLabels();
            }
        });
        audio.addView(outputGainSeek, matchWrap());
        audio.addView(text(
                "100 % hlasitosti odpovídá tomuto stropu. Pro silný zesilovač začni na -18 až -12 dB.",
                12, MUTED, false), matchWrapWithMargins(0, 0, 0, 6));

        volumeCurveValue = text("Křivka hlasitosti: 1.8×", 13, TEXT, true);
        audio.addView(volumeCurveValue, matchWrapWithMargins(0, 8, 0, 0));
        volumeCurveSeek = new SeekBar(this);
        volumeCurveSeek.setMax(20);
        volumeCurveSeek.setProgress(8);
        volumeCurveSeek.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override public void changed(int value, boolean fromUser) {
                updateDetailedEqLabels();
            }
        });
        audio.addView(volumeCurveSeek, matchWrap());
        audio.addView(text(
                "Vyšší hodnota zjemní hlasitost kolem 10–40 %. Hodnota 1.0 odpovídá původnímu lineárnímu průběhu.",
                12, MUTED, false), matchWrapWithMargins(0, 0, 0, 8));

        eqEnabledCheck = checkBox("Zapnout 10pásmový ekvalizér", true);
        audio.addView(eqEnabledCheck, matchWrap());

        eqAutoHeadroomCheck = checkBox("Automatická rezerva proti clippingu", true);
        audio.addView(eqAutoHeadroomCheck, matchWrap());

        eqPreampValue = text("EQ preamp: 0 dB", 13, TEXT, true);
        audio.addView(eqPreampValue, matchWrapWithMargins(0, 8, 0, 0));
        eqPreampSeek = new SeekBar(this);
        eqPreampSeek.setMax(30);
        eqPreampSeek.setProgress(24);
        eqPreampSeek.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override public void changed(int value, boolean fromUser) {
                updateDetailedEqLabels();
            }
        });
        audio.addView(eqPreampSeek, matchWrap());

        GridLayout presets = new GridLayout(this);
        presets.setColumnCount(2);
        String[] presetNames = {"Rovný", "Basy", "Mluvené slovo", "Loudness"};
        String[] presetIds = {"flat", "bass", "voice", "loudness"};
        for (int i = 0; i < presetNames.length; i++) {
            final String presetId = presetIds[i];
            Button preset = textButton(presetNames[i], PANEL_3);
            preset.setOnClickListener(v -> applyEqPreset(presetId));
            GridLayout.LayoutParams pp = new GridLayout.LayoutParams();
            pp.width = 0;
            pp.columnSpec = GridLayout.spec(GridLayout.UNDEFINED, 1f);
            pp.setMargins(dp(4), dp(4), dp(4), dp(4));
            presets.addView(preset, pp);
        }
        audio.addView(presets, matchWrapWithMargins(0, 8, 0, 4));

        Button safeGain = textButton("Nastavit bezpečný gain (-18 dB)", PANEL_3);
        safeGain.setOnClickListener(v -> {
            if (outputGainSeek != null) outputGainSeek.setProgress(22);
            if (volumeCurveSeek != null) volumeCurveSeek.setProgress(10);
            updateDetailedEqLabels();
        });
        audio.addView(safeGain, matchWrapWithMargins(0, 4, 0, 8));

        for (int i = 0; i < EQ_BAND_LABELS.length; i++) {
            final int band = i;
            eqBandValues[i] = text(EQ_BAND_LABELS[i] + ": 0 dB", 13, TEXT, true);
            audio.addView(eqBandValues[i], matchWrapWithMargins(0, i == 0 ? 8 : 5, 0, 0));
            eqBandSeeks[i] = new SeekBar(this);
            eqBandSeeks[i].setMax(24);
            eqBandSeeks[i].setProgress(12);
            eqBandSeeks[i].setOnSeekBarChangeListener(new SimpleSeekListener() {
                @Override public void changed(int value, boolean fromUser) {
                    if (eqBandValues[band] != null) {
                        eqBandValues[band].setText(EQ_BAND_LABELS[band] + ": " + dbText(value - 12));
                    }
                }
            });
            audio.addView(eqBandSeeks[i], matchWrap());
        }

        Button resetEq = textButton("Vynulovat ekvalizér", PANEL_3);
        resetEq.setOnClickListener(v -> applyEqPreset("flat"));
        audio.addView(resetEq, matchWrapWithMargins(0, 8, 0, 0));
        audio.addView(text(
                "Pásma 31 Hz až 16 kHz, rozsah -12 až +12 dB. Nastavení platí pro rádio, soubory, USB i Sendspin.",
                12, MUTED, false), matchWrapWithMargins(0, 6, 0, 0));
        body.addView(audio, matchWrapWithMargins(0, 0, 0, 12));

        LinearLayout battery = card();
        battery.addView(text("Stav baterie", 20, TEXT, true), matchWrap());
        batteryEnabledCheck = checkBox("Zapnout měření baterie na GPIO1", false);
        battery.addView(batteryEnabledCheck, matchWrap());
        batteryDividerInput = input("Poměr napěťového děliče", "2.000", false);
        batteryDividerInput.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        battery.addView(batteryDividerInput, matchWrapWithMargins(0, 8, 0, 0));
        batteryCalibrationInput = input("Kalibrační násobek", "1.000", false);
        batteryCalibrationInput.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        battery.addView(batteryCalibrationInput, matchWrapWithMargins(0, 8, 0, 0));
        battery.addView(text(
                "Pro dělič 100 kΩ / 100 kΩ nastav poměr 2.000. Dokud dělič není připojený, nech měření vypnuté.",
                12, MUTED, false), matchWrapWithMargins(0, 8, 0, 0));
        body.addView(battery, matchWrapWithMargins(0, 0, 0, 12));

        LinearLayout defaultRadio = card();
        defaultRadio.addView(text("Výchozí internetové rádio", 20, TEXT, true), matchWrap());
        defaultRadioNameInput = input("Název stanice", "", false);
        defaultRadio.addView(defaultRadioNameInput, matchWrapWithMargins(0, 8, 0, 0));
        defaultRadioUrlInput = input("HTTP MP3 URL", "", false);
        defaultRadio.addView(defaultRadioUrlInput, matchWrapWithMargins(0, 8, 0, 0));
        body.addView(defaultRadio, matchWrapWithMargins(0, 0, 0, 12));

        LinearLayout speaker = card();
        speaker.addView(text("Síťový reproduktor", 20, TEXT, true), matchWrap());
        smartSpeakerEnabledCheck = checkBox("Zapnout síťový reproduktor Sendspin", false);
        speaker.addView(smartSpeakerEnabledCheck, matchWrap());
        body.addView(speaker, matchWrapWithMargins(0, 0, 0, 12));

        Button saveRadioConfig = textButton("Uložit konfiguraci rádia", ACCENT);
        saveRadioConfig.setOnClickListener(v -> saveRadioConfiguration());
        body.addView(saveRadioConfig, matchWrapWithMargins(0, 0, 0, 12));

        LinearLayout device = card();
        device.addView(text("Zařízení", 20, TEXT, true), matchWrap());
        Button refresh = textButton("Načíst stav zařízení", PANEL_3);
        refresh.setOnClickListener(v -> loadDeviceSummary(device));
        device.addView(refresh, matchWrapWithMargins(0, 8, 0, 0));
        Button reboot = textButton("Restartovat rádio", DANGER);
        reboot.setOnClickListener(v -> confirmReboot());
        device.addView(reboot, matchWrapWithMargins(0, 8, 0, 0));
        body.addView(device, matchWrap());

        loadRadioConfiguration();
        loadDeviceSummary(device);
        return scroll(body);
    }

    private void saveAndTestSettings() {
        persistConnectionFields();
        if (sendspin.isConnected()) sendspin.close();
        settingsTestResult.setTextColor(MUTED);
        settingsTestResult.setText("Testuji " + radioHost + "…");
        api.getJson("/status.json", (data, error) -> {
            if (error != null) {
                settingsTestResult.setTextColor(DANGER);
                settingsTestResult.setText("Připojení selhalo: " + error.getMessage());
                setBottomStatus("Rádio není dostupné", DANGER);
            } else {
                settingsTestResult.setTextColor(OK);
                settingsTestResult.setText("Připojeno. " + data.optString("audioDetail", "Rádio odpovídá."));
                setBottomStatus("Připojeno k rádiu " + radioHost, OK);
                loadRadioConfiguration();
                refreshConfig(true);
            }
        });
    }

    private void persistConnectionFields() {
        if (settingsHost == null || settingsPort == null || settingsUser == null || settingsPassword == null) return;
        int port;
        try { port = Integer.parseInt(settingsPort.getText().toString().trim()); }
        catch (Exception e) { port = DEFAULT_SENDSPIN_PORT; }
        saveSettings(
                settingsHost.getText().toString(),
                port,
                settingsUser.getText().toString(),
                settingsPassword.getText().toString());
        settingsHost.setText(radioHost);
        settingsPort.setText(String.valueOf(sendspinPort));
    }

    private void loadRadioConfiguration() {
        if (settingsTestResult != null) {
            settingsTestResult.setTextColor(MUTED);
            settingsTestResult.setText("Načítám konfiguraci rádia…");
        }
        api.getJson("/api/config.json", (data, error) -> {
            if (error != null) {
                if (settingsTestResult != null) {
                    settingsTestResult.setTextColor(DANGER);
                    settingsTestResult.setText("Konfiguraci nelze načíst: " + error.getMessage());
                }
                return;
            }
            lastConfig = data;
            if (settingsTestResult != null) {
                settingsTestResult.setTextColor(OK);
                settingsTestResult.setText("Konfigurace načtena z " + radioHost);
            }
            if (apModeSpinner != null) apModeSpinner.setSelection(Math.max(0, Math.min(2, data.optInt("apMode", 1))));
            setEdit(apSsidInput, data.optString("apSsid", ""));
            setEdit(apPasswordInput, data.optString("apPass", ""));
            setEdit(mdnsInput, data.optString("mdnsName", "oris-radio"));
            setEdit(settingsUser, data.optString("webUser", webUser));
            setEdit(settingsPassword, data.optString("webPass", webPassword));
            if (ftpEnabledCheck != null) ftpEnabledCheck.setChecked(data.optBoolean("ftpEnabled", false));
            setEdit(ftpUserInput, data.optString("ftpUser", ""));
            setEdit(ftpPasswordInput, data.optString("ftpPass", ""));
            if (ftpDiskSpinner != null) ftpDiskSpinner.setSelection("usb0".equals(data.optString("ftpDisk", "ffat")) ? 1 : 0);
            if (rgbEnabledCheck != null) rgbEnabledCheck.setChecked(data.optBoolean("rgbEnabled", false));
            if (configVolumeSeek != null) configVolumeSeek.setProgress(data.optInt("audioVolume", 50));
            if (eqEnabledCheck != null) eqEnabledCheck.setChecked(data.optBoolean("audioEqEnabled", true));
            if (eqAutoHeadroomCheck != null) eqAutoHeadroomCheck.setChecked(data.optBoolean("audioEqAutoHeadroom", true));
            if (eqPreampSeek != null) eqPreampSeek.setProgress(Math.max(0, Math.min(30, data.optInt("audioEqPreampDb", 0) + 24)));
            if (outputGainSeek != null) outputGainSeek.setProgress(Math.max(0, Math.min(40, data.optInt("audioOutputGainDb", -12) + 40)));
            if (volumeCurveSeek != null) {
                int curveProgress = (int) Math.round((data.optDouble("audioVolumeCurve", 1.8) - 1.0) * 10.0);
                volumeCurveSeek.setProgress(Math.max(0, Math.min(20, curveProgress)));
            }
            JSONArray eqBands = data.optJSONArray("audioEqBands");
            for (int i = 0; i < eqBandSeeks.length; i++) {
                int db;
                if (eqBands != null && i < eqBands.length()) {
                    db = eqBands.optInt(i, 0);
                } else if (i < 4) {
                    db = data.optInt("audioBassDb", 0);
                } else if (i >= 7) {
                    db = data.optInt("audioTrebleDb", 0);
                } else {
                    db = 0;
                }
                if (eqBandSeeks[i] != null) eqBandSeeks[i].setProgress(Math.max(0, Math.min(24, db + 12)));
            }
            updateDetailedEqLabels();
            if (batteryEnabledCheck != null) batteryEnabledCheck.setChecked(data.optBoolean("batteryEnabled", false));
            setEdit(batteryDividerInput, String.format(Locale.US, "%.3f", data.optDouble("batteryDividerRatio", 2.0)));
            setEdit(batteryCalibrationInput, String.format(Locale.US, "%.3f", data.optDouble("batteryCalibration", 1.0)));
            if (smartSpeakerEnabledCheck != null) smartSpeakerEnabledCheck.setChecked(data.optBoolean("smartSpeakerEnabled", false));
            JSONArray stations = data.optJSONArray("radioStations");
            JSONObject first = stations == null ? null : stations.optJSONObject(0);
            setEdit(defaultRadioNameInput, first == null ? "" : first.optString("name", ""));
            setEdit(defaultRadioUrlInput, first == null ? "" : first.optString("url", ""));
            renderSavedWifi(data.optJSONArray("savedWifi"));
        });
    }

    private void saveRadioConfiguration() {
        if (lastConfig == null) {
            toast("Nejdřív načti konfiguraci rádia");
            loadRadioConfiguration();
            return;
        }
        String desiredUser = valueOf(settingsUser);
        String desiredPassword = valueOf(settingsPassword);
        if (desiredUser.trim().isEmpty() || desiredPassword.isEmpty()) {
            toast("Web uživatel a heslo nesmí být prázdné");
            return;
        }

        Map<String, String> values = new LinkedHashMap<>();
        values.put("ap_mode", String.valueOf(apModeSpinner == null ? 1 : apModeSpinner.getSelectedItemPosition()));
        values.put("ap_ssid", valueOf(apSsidInput));
        values.put("ap_pass", valueOf(apPasswordInput));
        values.put("mdns_name", valueOf(mdnsInput));
        values.put("web_user", desiredUser);
        values.put("web_pass", desiredPassword);
        values.put("ftp_user", valueOf(ftpUserInput));
        values.put("ftp_pass", valueOf(ftpPasswordInput));
        values.put("ftp_disk", ftpDiskSpinner != null && ftpDiskSpinner.getSelectedItemPosition() == 1 ? "usb0" : "ffat");
        if (ftpEnabledCheck != null && ftpEnabledCheck.isChecked()) values.put("ftp_enabled", "1");
        if (rgbEnabledCheck != null && rgbEnabledCheck.isChecked()) values.put("rgb_enabled", "1");
        values.put("audio_volume", String.valueOf(configVolumeSeek == null ? 50 : configVolumeSeek.getProgress()));
        values.put("audio_eq_enabled", eqEnabledCheck != null && eqEnabledCheck.isChecked() ? "1" : "0");
        values.put("audio_eq_auto_headroom", eqAutoHeadroomCheck != null && eqAutoHeadroomCheck.isChecked() ? "1" : "0");
        values.put("audio_eq_preamp_db", String.valueOf(eqPreampSeek == null ? 0 : eqPreampSeek.getProgress() - 24));
        values.put("audio_output_gain_db", String.valueOf(outputGainSeek == null ? -12 : outputGainSeek.getProgress() - 40));
        double curve = volumeCurveSeek == null ? 1.8 : 1.0 + volumeCurveSeek.getProgress() / 10.0;
        values.put("audio_volume_curve", String.format(Locale.US, "%.1f", curve));
        for (int i = 0; i < eqBandSeeks.length; i++) {
            int db = eqBandSeeks[i] == null ? 0 : eqBandSeeks[i].getProgress() - 12;
            values.put("audio_eq_band_" + i, String.valueOf(db));
        }
        if (batteryEnabledCheck != null && batteryEnabledCheck.isChecked()) values.put("battery_enabled", "1");
        values.put("battery_divider_ratio", valueOf(batteryDividerInput));
        values.put("battery_calibration", valueOf(batteryCalibrationInput));
        values.put("radio_name", valueOf(defaultRadioNameInput));
        values.put("radio_url", valueOf(defaultRadioUrlInput));

        setBottomStatus("Ukládám konfiguraci rádia…", MUTED);
        api.postForm("/config/save", values, (text, error) -> {
            if (error != null) {
                setBottomStatus("Konfiguraci nelze uložit: " + error.getMessage(), DANGER);
                return;
            }
            saveSettings(radioHost, sendspinPort, desiredUser, desiredPassword);
            Map<String, String> speaker = new LinkedHashMap<>();
            if (smartSpeakerEnabledCheck != null && smartSpeakerEnabledCheck.isChecked()) speaker.put("enabled", "1");
            api.postForm("/smart-speaker/save", speaker, (speakerText, speakerError) -> {
                if (speakerError != null) {
                    setBottomStatus("Konfigurace uložena, Sendspin: " + speakerError.getMessage(), WARN);
                } else {
                    setBottomStatus("Konfigurace rádia uložena", OK);
                }
                mainHandler.postDelayed(() -> {
                    loadRadioConfiguration();
                    refreshConfig(true);
                    refreshStatus(false);
                }, 900L);
            });
        });
    }

    private void startWifiScan() {
        if (wifiScanStatus != null) {
            wifiScanStatus.setTextColor(MUTED);
            wifiScanStatus.setText("Spouštím vyhledávání…");
        }
        api.post("/wifi/scan/start", (text, error) -> {
            if (error != null) {
                if (wifiScanStatus != null) {
                    wifiScanStatus.setTextColor(DANGER);
                    wifiScanStatus.setText(error.getMessage());
                }
                return;
            }
            pollWifiScan(0);
        });
    }

    private void pollWifiScan(int attempt) {
        if (attempt > 30) {
            if (wifiScanStatus != null) wifiScanStatus.setText("Vyhledávání vypršelo.");
            return;
        }
        api.getJson("/wifi/scan.json", (data, error) -> {
            if (error != null) {
                if (wifiScanStatus != null) {
                    wifiScanStatus.setTextColor(DANGER);
                    wifiScanStatus.setText(error.getMessage());
                }
                return;
            }
            if (data.optBoolean("running", false)) {
                if (wifiScanStatus != null) wifiScanStatus.setText("Vyhledávám…");
                mainHandler.postDelayed(() -> pollWifiScan(attempt + 1), 700L);
                return;
            }
            if (!data.optBoolean("ok", false)) {
                if (wifiScanStatus != null) {
                    wifiScanStatus.setTextColor(DANGER);
                    wifiScanStatus.setText(data.optString("error", "Vyhledávání selhalo"));
                }
                return;
            }
            JSONArray networks = data.optJSONArray("networks");
            scannedWifiNetworks.clear();
            ArrayList<String> labels = new ArrayList<>();
            if (networks != null) {
                for (int i = 0; i < networks.length(); i++) {
                    JSONObject network = networks.optJSONObject(i);
                    if (network == null) continue;
                    scannedWifiNetworks.add(network);
                    labels.add(network.optString("ssid", "") + "  ·  " + network.optInt("rssi", 0) + " dBm" +
                            (network.optBoolean("secure", false) ? "  🔒" : ""));
                }
            }
            if (labels.isEmpty()) labels.add("Žádná síť nenalezena");
            if (wifiScanSpinner != null) wifiScanSpinner.setAdapter(darkSpinnerAdapter(labels));
            if (wifiScanStatus != null) {
                wifiScanStatus.setTextColor(OK);
                wifiScanStatus.setText("Nalezeno sítí: " + scannedWifiNetworks.size());
            }
        });
    }

    private void addOrUpdateWifi() {
        String ssid = valueOf(wifiSsidInput).trim();
        if (ssid.isEmpty()) {
            toast("Zadej SSID");
            return;
        }
        Map<String, String> values = new LinkedHashMap<>();
        values.put("ssid", ssid);
        values.put("pass", valueOf(wifiPasswordInput));
        api.postForm("/wifi/add", values, (text, error) -> {
            if (error != null) setBottomStatus("Wi‑Fi: " + error.getMessage(), DANGER);
            else {
                setBottomStatus("Wi‑Fi uložena, rádio se připojuje", OK);
                if (wifiPasswordInput != null) wifiPasswordInput.setText("");
                mainHandler.postDelayed(this::loadRadioConfiguration, 1500L);
            }
        });
    }

    private void renderSavedWifi(JSONArray saved) {
        if (savedWifiList == null) return;
        savedWifiList.removeAllViews();
        if (saved == null || saved.length() == 0) {
            savedWifiList.addView(text("Zatím není uložená žádná Wi‑Fi.", 13, WARN, false), matchWrap());
            return;
        }
        for (int i = 0; i < saved.length(); i++) {
            JSONObject item = saved.optJSONObject(i);
            if (item == null) continue;
            int index = item.optInt("index", i);
            LinearLayout row = horizontalRow();
            row.setPadding(dp(10), dp(8), dp(8), dp(8));
            row.setBackground(round(PANEL_2, dp(10), LINE, dp(1)));
            TextView label = text(item.optString("ssid", "Wi‑Fi") +
                    (item.optBoolean("connected", false) ? "\nPřipojeno" : "\nUloženo"),
                    14, item.optBoolean("connected", false) ? OK : TEXT, true);
            row.addView(label, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            Button connect = textButton("Připojit", PANEL_3);
            connect.setOnClickListener(v -> wifiIndexAction("/wifi/connect", index, false));
            row.addView(connect, new LinearLayout.LayoutParams(dp(92), dp(44)));
            Button delete = textButton("Smazat", DANGER);
            LinearLayout.LayoutParams deleteParams = new LinearLayout.LayoutParams(dp(82), dp(44));
            deleteParams.setMargins(dp(6), 0, 0, 0);
            row.addView(delete, deleteParams);
            delete.setOnClickListener(v -> new AlertDialog.Builder(this)
                    .setTitle("Smazat Wi‑Fi")
                    .setMessage("Opravdu smazat síť " + item.optString("ssid", "") + "?")
                    .setNegativeButton("Ne", null)
                    .setPositiveButton("Smazat", (dialog, which) -> wifiIndexAction("/wifi/delete", index, true))
                    .show());
            savedWifiList.addView(row, matchWrapWithMargins(0, 0, 0, 6));
        }
    }

    private void wifiIndexAction(String path, int index, boolean deleting) {
        Map<String, String> values = new LinkedHashMap<>();
        values.put("i", String.valueOf(index));
        api.postForm(path, values, (text, error) -> {
            if (error != null) setBottomStatus("Wi‑Fi: " + error.getMessage(), DANGER);
            else {
                setBottomStatus(deleting ? "Wi‑Fi smazána" : "Rádio se připojuje k vybrané Wi‑Fi", OK);
                mainHandler.postDelayed(this::loadRadioConfiguration, 1200L);
            }
        });
    }

    private void loadDeviceSummary(LinearLayout deviceCard) {
        api.getJson("/status.json", (data, error) -> {
            if (error != null) return;
            for (int i = deviceCard.getChildCount() - 1; i >= 0; i--) {
                View child = deviceCard.getChildAt(i);
                if (child.getTag() != null && child.getTag().equals("summary")) deviceCard.removeViewAt(i);
            }
            LinearLayout summary = new LinearLayout(this);
            summary.setTag("summary");
            summary.setOrientation(LinearLayout.VERTICAL);
            summary.setPadding(0, dp(10), 0, 0);
            summary.addView(metric("Wi‑Fi", data.optString("wifiSsid", "offline") + " · " + data.optInt("rssi", 0) + " dBm"));
            summary.addView(metric("Výstup", data.optString("audioOutput", "")));
            summary.addView(metric("USB", data.optString("usb", "")));
            summary.addView(metric("PSRAM", data.optString("psramFree", "")));
            summary.addView(metric("Uptime", data.optString("uptime", "")));
            deviceCard.addView(summary, matchWrap());
        });
    }

    private void confirmReboot() {
        new AlertDialog.Builder(this)
                .setTitle("Restartovat rádio")
                .setMessage("Opravdu restartovat rádio?")
                .setNegativeButton("Ne", null)
                .setPositiveButton("Restartovat", (dialog, which) -> api.post("/reboot", this::actionResult))
                .show();
    }

    private void refreshStatus(boolean showErrors) {
        api.getJson("/status.json", (data, error) -> {
            if (error != null) {
                headerConnection.setText("● offline");
                headerConnection.setTextColor(DANGER);
                if (showErrors) setBottomStatus(error.getMessage(), DANGER);
                return;
            }
            lastStatus = data;
            headerConnection.setText("● online");
            headerConnection.setTextColor(OK);
            updatePlayerFromStatus(data);
        });
    }

    private void refreshConfig(boolean updateVisible) {
        api.getJson("/api/config.json", (data, error) -> {
            if (error != null) {
                if (updateVisible) setBottomStatus("Konfigurace: " + error.getMessage(), DANGER);
                return;
            }
            lastConfig = data;
            cachedStations.clear();
            JSONArray stations = data.optJSONArray("radioStations");
            if (stations != null) {
                for (int i = 0; i < stations.length(); i++) {
                    JSONObject station = stations.optJSONObject(i);
                    if (station != null) cachedStations.add(station);
                }
            }
            if (selectedStationIndex < 0) {
                for (JSONObject station : cachedStations) {
                    if (!station.optString("url", "").isEmpty()) {
                        selectedStationIndex = station.optInt("index", 0);
                        break;
                    }
                }
            }
            if (currentPage == Page.PLAYER && playerStationGrid != null) renderPlayerStations();
            if (playerVolume != null && !playerVolume.isPressed()) {
                int volume = data.optInt("audioVolume", 50);
                playerVolume.setProgress(volume);
                updatePlayerVolumePercent(volume);
            }
        });
    }

    private void updatePlayerFromStatus(JSONObject data) {
        if (playerMainTitle == null) return;
        boolean playing = data.optBoolean("audioPlaying", false);
        boolean paused = data.optBoolean("audioPaused", false);
        String mode = data.optString("audio", "připraveno");
        String title = data.optString("audioTitle", "Přehrávač připraven");
        String detail = data.optString("audioDetail", mode);
        int radioIndex = data.optInt("radioIndex", -1);
        if (radioIndex >= 0) selectedStationIndex = radioIndex;

        String modeText = "PŘIPRAVENO";
        String subtitle = "Vyber stanici a spusť přehrávání.";
        if ("radio".equals(mode)) {
            modeText = paused ? "RÁDIO · PAUZA" : "INTERNETOVÉ RÁDIO";
            subtitle = paused ? "Přehrávání je pozastavené." : "Živý MP3 stream";
        } else if ("karaoke".equals(mode)) {
            modeText = paused ? "KARAOKE · PAUZA" : "KARAOKE";
            subtitle = "Karaoke skladba";
        } else if ("soubor".equals(mode) || "složka".equals(mode)) {
            modeText = paused ? "HUDBA · PAUZA" : ("složka".equals(mode) ? "PLAYLIST" : "HUDBA");
            subtitle = "složka".equals(mode) ? "Přehrávání MP3 ze složky" : "Lokální soubor";
        }

        playerMode.setText(modeText);
        playerMainTitle.setText(title == null || title.trim().isEmpty() ? "Přehrávač připraven" : title);
        playerSubtitle.setText(subtitle);
        playerDetail.setText(detail);
        playerPlayPause.setText(paused || !playing ? "▶" : "Ⅱ");

        if (data.optBoolean("batteryEnabled", false) && data.optBoolean("batteryValid", false)) {
            int battery = Math.max(0, Math.min(100, data.optInt("batteryPercent", 0)));
            playerBattery.setText("🔋 Baterie " + battery + " % · " +
                    String.format(Locale.getDefault(), "%.2f V", data.optDouble("batteryVoltage", 0.0)));
            if (playerBatteryBar != null) {
                playerBatteryBar.setVisibility(View.VISIBLE);
                playerBatteryBar.setProgress(battery);
            }
        } else {
            playerBattery.setText(data.optBoolean("batteryEnabled", false)
                    ? "🔋 Baterie: čekám na měření"
                    : "🔋 Měření baterie vypnuto");
            if (playerBatteryBar != null) {
                playerBatteryBar.setProgress(0);
                playerBatteryBar.setVisibility(data.optBoolean("batteryEnabled", false) ? View.VISIBLE : View.GONE);
            }
        }

        String logo = data.optString("radioLogo", "");
        if (logo.isEmpty() && radioIndex >= 0) {
            JSONObject station = stationByIndex(radioIndex);
            if (station != null) logo = station.optString("logo", "");
        }
        loadLogo(playerCover, logo);
        if (playerStationGrid != null) renderPlayerStations();
        if (!mobilePlaylistActive) {
            if (playing || paused) {
                MediaNotificationService.show(this,
                        MediaNotificationService.SOURCE_REMOTE,
                        title == null || title.trim().isEmpty() ? "ORIS rádio" : title,
                        modeText,
                        playing && !paused,
                        mainPlayerUsesTrackNavigation()
                                ? MediaNotificationService.NAVIGATION_TRACK
                                : MediaNotificationService.NAVIGATION_STATION,
                        availableStationIndices(),
                        selectedStationIndex);
            } else {
                MediaNotificationService.hide(this);
            }
        }
    }

    private void renderPlayerStations() {
        if (playerStationGrid == null) return;
        StringBuilder signatureBuilder = new StringBuilder();
        signatureBuilder.append(selectedStationIndex).append('|');
        signatureBuilder.append(lastStatus == null ? -1 : lastStatus.optInt("radioIndex", -1)).append('|');
        for (JSONObject station : cachedStations) {
            signatureBuilder.append(station.optInt("index", -1)).append(':')
                    .append(station.optString("name", "")).append(':')
                    .append(station.optString("url", "")).append(':')
                    .append(station.optString("logo", "")).append('|');
        }
        String signature = signatureBuilder.toString();
        if (signature.equals(lastStationGridSignature) && playerStationGrid.getChildCount() > 0) return;
        lastStationGridSignature = signature;

        playerStationGrid.removeAllViews();
        int count = 0;
        for (JSONObject station : cachedStations) {
            if (station.optString("url", "").trim().isEmpty()) continue;
            int index = station.optInt("index", count);
            boolean active = lastStatus != null && lastStatus.optInt("radioIndex", -1) == index;
            boolean selected = selectedStationIndex == index;
            LinearLayout tile = new LinearLayout(this);
            tile.setOrientation(LinearLayout.VERTICAL);
            tile.setGravity(Gravity.CENTER_HORIZONTAL);
            tile.setPadding(dp(8), dp(9), dp(8), dp(9));
            tile.setBackground(round(active ? Color.rgb(34, 53, 82) : (selected ? PANEL_3 : PANEL_2),
                    dp(12), active ? ACCENT : LINE, dp(1)));
            tile.setOnClickListener(v -> playStation(index));

            ImageView image = new ImageView(this);
            image.setScaleType(ImageView.ScaleType.CENTER_CROP);
            image.setImageResource(android.R.drawable.ic_media_play);
            image.setBackground(round(PANEL_3, dp(10), LINE, dp(1)));
            tile.addView(image, new LinearLayout.LayoutParams(dp(76), dp(76)));
            loadLogo(image, station.optString("logo", ""));

            TextView name = text(station.optString("name", "Stanice " + (index + 1)), 14, TEXT, true);
            name.setGravity(Gravity.CENTER_HORIZONTAL);
            name.setMaxLines(2);
            tile.addView(name, matchWrapWithMargins(0, 8, 0, 0));
            TextView state = text(active ? "PRÁVĚ HRAJE" : "PŘEPNOUT", 10, active ? ACCENT_2 : MUTED, true);
            state.setGravity(Gravity.CENTER_HORIZONTAL);
            tile.addView(state, matchWrapWithMargins(0, 3, 0, 0));

            GridLayout.LayoutParams params = new GridLayout.LayoutParams();
            params.width = 0;
            params.height = ViewGroup.LayoutParams.WRAP_CONTENT;
            params.columnSpec = GridLayout.spec(GridLayout.UNDEFINED, 1f);
            params.setMargins(dp(4), dp(4), dp(4), dp(4));
            playerStationGrid.addView(tile, params);
            count++;
        }
        if (count == 0) playerStationGrid.addView(text("Nejdřív nastav stanice v menu Internetová rádia.", 13, WARN, false));
    }

    private void togglePlayer() {
        boolean active = lastStatus != null &&
                (lastStatus.optBoolean("audioPlaying", false) || lastStatus.optBoolean("audioPaused", false));
        if (active) audioPost("/audio/toggle_pause");
        else if (selectedStationIndex >= 0) playStation(selectedStationIndex);
        else toast("Není nastavená žádná stanice");
    }

    private void handleMainPrevious() {
        if (mainPlayerUsesTrackNavigation()) {
            audioPost("/audio/prev");
        } else {
            playRelativeStation(-1);
        }
    }

    private void handleMainNext() {
        if (mainPlayerUsesTrackNavigation()) {
            audioPost("/audio/next");
        } else {
            playRelativeStation(1);
        }
    }

    private boolean mainPlayerUsesTrackNavigation() {
        if (lastStatus == null) return false;
        String mode = lastStatus.optString("audio", "");
        return "složka".equals(mode) || lastStatus.optBoolean("smartSpeakerActive", false);
    }

    private void playRelativeStation(int delta) {
        ArrayList<JSONObject> available = new ArrayList<>();
        for (JSONObject station : cachedStations) {
            if (!station.optString("url", "").trim().isEmpty()) available.add(station);
        }
        if (available.isEmpty()) return;
        int current = 0;
        for (int i = 0; i < available.size(); i++) {
            if (available.get(i).optInt("index", -1) == selectedStationIndex) current = i;
        }
        current = (current + delta + available.size()) % available.size();
        playStation(available.get(current).optInt("index", 0));
    }

    private String availableStationIndices() {
        StringBuilder out = new StringBuilder();
        for (JSONObject station : cachedStations) {
            if (station.optString("url", "").trim().isEmpty()) continue;
            if (out.length() > 0) out.append(',');
            out.append(station.optInt("index", 0));
        }
        return out.toString();
    }

    private void updateMobileMediaNotification() {
        if (!mobilePlaylistActive) return;
        MediaNotificationService.show(this,
                MediaNotificationService.SOURCE_MOBILE,
                mobilePlaylistTitle.isEmpty() ? "Skladba z mobilu" : mobilePlaylistTitle,
                "Hudba v mobilu → ORIS rádio",
                !streamer.isPaused(),
                MediaNotificationService.NAVIGATION_TRACK,
                "",
                -1);
    }

    private void changePlayerVolume(int delta) {
        if (playerVolume == null) return;
        int value = Math.max(0, Math.min(100, playerVolume.getProgress() + delta));
        playerVolume.setProgress(value);
        updatePlayerVolumePercent(value);
        setRadioVolume(value);
    }

    private void updatePlayerVolumePercent(int value) {
        if (playerVolumePercent != null) playerVolumePercent.setText(Math.max(0, Math.min(100, value)) + " %");
    }

    private void playStation(int index) {
        selectedStationIndex = index;
        api.post("/radio/play?i=" + index, (text, error) -> {
            actionResult(text, error);
            if (error == null) mainHandler.postDelayed(() -> refreshStatus(false), 250L);
        });
        if (playerStationGrid != null) renderPlayerStations();
    }

    private void audioPost(String path) {
        api.post(path, (text, error) -> {
            actionResult(text, error);
            if (error == null) mainHandler.postDelayed(() -> refreshStatus(false), 180L);
        });
    }

    private void setRadioVolume(int volume) {
        api.getText("/audio/volume?v=" + volume, (text, error) -> actionResult(text, error));
    }

    private void loadLogo(ImageView view, String path) {
        if (view == null) return;
        String normalized = path == null ? "" : path.trim();
        Object oldTag = view.getTag();
        if (normalized.equals(oldTag)) return;
        view.setTag(normalized);
        if (normalized.isEmpty()) {
            view.setImageResource(android.R.drawable.ic_media_play);
            return;
        }
        api.loadBitmap(normalized, (bitmap, error) -> {
            if (view.getWindowToken() == null) return;
            Object currentTag = view.getTag();
            if (!normalized.equals(currentTag)) return;
            if (error == null && bitmap != null) view.setImageBitmap(bitmap);
        });
    }

    private JSONObject stationByIndex(int index) {
        for (JSONObject station : cachedStations) {
            if (station.optInt("index", -1) == index) return station;
        }
        return null;
    }

    private void chooseAudioFiles() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("audio/*");
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(intent, PICK_AUDIO);
    }

    private void chooseAudioFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        startActivityForResult(intent, PICK_AUDIO_FOLDER);
    }

    private void importAudioFolder(Uri treeUri, int resultFlags) {
        if (treeUri == null) return;
        try {
            int takeFlags = resultFlags & Intent.FLAG_GRANT_READ_URI_PERMISSION;
            getContentResolver().takePersistableUriPermission(treeUri, takeFlags);
        } catch (Exception ignored) {
        }

        setBottomStatus("Načítám hudbu ze složky…", MUTED);
        new Thread(() -> {
            ArrayList<AudioFolderEntry> found = new ArrayList<>();
            String error = null;
            try {
                String rootId = DocumentsContract.getTreeDocumentId(treeUri);
                collectAudioDocuments(treeUri, rootId, "", found, 0);
                Collections.sort(found, Comparator.comparing(
                        entry -> entry.name.toLowerCase(Locale.ROOT)));
            } catch (Exception e) {
                error = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
            }

            String finalError = error;
            runOnUiThread(() -> {
                if (finalError != null) {
                    setBottomStatus("Složku se nepodařilo načíst: " + finalError, DANGER);
                    return;
                }
                playlistUris.clear();
                playlistNames.clear();
                for (AudioFolderEntry entry : found) {
                    playlistUris.add(entry.uri);
                    playlistNames.add(entry.name);
                }
                selectedTrackIndex = 0;
                if (phonePlaylistAdapter != null) phonePlaylistAdapter.notifyDataSetChanged();
                if (phonePlaylistView != null) {
                    phonePlaylistView.clearChoices();
                    if (!playlistNames.isEmpty()) phonePlaylistView.setItemChecked(0, true);
                }
                if (phoneTrackTitle != null) phoneTrackTitle.setText(
                        playlistNames.isEmpty() ? "Ve složce nebyla nalezena hudba" : playlistNames.get(0));
                setBottomStatus(playlistNames.isEmpty()
                        ? "Ve vybrané složce nebyly nalezeny zvukové soubory"
                        : "Importováno skladeb: " + playlistNames.size(),
                        playlistNames.isEmpty() ? WARN : OK);
            });
        }, "OrisFolderImport").start();
    }

    private void collectAudioDocuments(
            Uri treeUri,
            String parentDocumentId,
            String relativePath,
            ArrayList<AudioFolderEntry> output,
            int depth
    ) {
        if (depth > 32 || output.size() >= 5000) return;
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocumentId);
        android.database.Cursor cursor = null;
        try {
            cursor = getContentResolver().query(childrenUri,
                    new String[]{
                            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                            DocumentsContract.Document.COLUMN_MIME_TYPE
                    }, null, null, null);
            if (cursor == null) return;
            int idColumn = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DOCUMENT_ID);
            int nameColumn = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME);
            int mimeColumn = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_MIME_TYPE);
            while (cursor.moveToNext() && output.size() < 5000) {
                if (idColumn < 0) continue;
                String documentId = cursor.getString(idColumn);
                String name = nameColumn >= 0 ? cursor.getString(nameColumn) : "Skladba";
                String mime = mimeColumn >= 0 ? cursor.getString(mimeColumn) : "";
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    String childPath = relativePath.isEmpty() ? name : relativePath + " / " + name;
                    collectAudioDocuments(treeUri, documentId, childPath, output, depth + 1);
                } else if (isAudioDocument(name, mime)) {
                    Uri documentUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId);
                    String display = relativePath.isEmpty() ? name : relativePath + " / " + name;
                    output.add(new AudioFolderEntry(documentUri, display));
                }
            }
        } finally {
            if (cursor != null) cursor.close();
        }
    }

    private static boolean isAudioDocument(String name, String mime) {
        if (mime != null && mime.toLowerCase(Locale.ROOT).startsWith("audio/")) return true;
        String lower = name == null ? "" : name.toLowerCase(Locale.ROOT);
        return lower.endsWith(".mp3") || lower.endsWith(".wav") || lower.endsWith(".flac")
                || lower.endsWith(".m4a") || lower.endsWith(".aac") || lower.endsWith(".ogg")
                || lower.endsWith(".opus") || lower.endsWith(".wma") || lower.endsWith(".3gp")
                || lower.endsWith(".amr");
    }

    private static final class AudioFolderEntry {
        final Uri uri;
        final String name;

        AudioFolderEntry(Uri uri, String name) {
            this.uri = uri;
            this.name = name == null || name.trim().isEmpty() ? "Skladba" : name;
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == REQUEST_SYSTEM_AUDIO) {
            if (resultCode != RESULT_OK || data == null) {
                if (systemAudioStatus != null) systemAudioStatus.setText("Sdílení zvuku nebylo povoleno.");
                return;
            }
            startSystemAudioService(resultCode, data);
            return;
        }

        if (requestCode == PICK_FILE_UPLOAD) {
            if (resultCode == RESULT_OK && data != null) queueSelectedUploads(data);
            return;
        }

        if (requestCode == CREATE_FILE_DOWNLOAD) {
            if (resultCode == RESULT_OK && data != null) finishFileDownload(data.getData());
            return;
        }

        if (requestCode == PICK_AUDIO_FOLDER) {
            if (resultCode == RESULT_OK && data != null) {
                importAudioFolder(data.getData(), data.getFlags());
            }
            return;
        }

        if (resultCode != RESULT_OK || data == null) return;

        if (requestCode == PICK_STATION_LOGO) {
            Uri uri = data.getData();
            if (uri == null || pendingLogoIndex < 0) return;
            int index = pendingLogoIndex;
            pendingLogoIndex = -1;
            setBottomStatus("Nahrávám logo…", MUTED);
            api.upload(uri, "logo", "/radio/logo?i=" + index, (text, error) -> {
                actionResult(text, error);
                if (error == null) {
                    api.clearBitmapCache();
                    refreshConfig(true);
                    if (currentPage == Page.RADIO) loadRadioEditor();
                }
            });
            return;
        }

        if (requestCode == PICK_AUDIO) {
            playlistUris.clear();
            playlistNames.clear();
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); i++) addAudioUri(clip.getItemAt(i).getUri(), data.getFlags());
            } else if (data.getData() != null) {
                addAudioUri(data.getData(), data.getFlags());
            }
            selectedTrackIndex = 0;
            if (phonePlaylistAdapter != null) phonePlaylistAdapter.notifyDataSetChanged();
            if (phoneTrackTitle != null) phoneTrackTitle.setText(
                    playlistNames.isEmpty() ? "Není vybraná skladba" : playlistNames.get(0));
            setBottomStatus("Vybráno skladeb: " + playlistNames.size(), OK);
        }
    }

    private void addAudioUri(Uri uri, int flags) {
        if (uri == null) return;
        try {
            int takeFlags = flags & Intent.FLAG_GRANT_READ_URI_PERMISSION;
            getContentResolver().takePersistableUriPermission(uri, takeFlags);
        } catch (Exception ignored) {}
        playlistUris.add(uri);
        playlistNames.add(displayName(uri));
    }

    private String displayName(Uri uri) {
        android.database.Cursor cursor = null;
        try {
            cursor = getContentResolver().query(uri,
                    new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null);
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) return cursor.getString(index);
            }
        } catch (Exception ignored) {
        } finally {
            if (cursor != null) cursor.close();
        }
        String last = uri.getLastPathSegment();
        return last == null ? "Skladba" : last;
    }

    private void requestSystemAudioCapture() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            toast("Stream systémového zvuku vyžaduje Android 10+");
            return;
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, REQUEST_RECORD_AUDIO);
            return;
        }
        pendingLocalPlay = false;
        streamer.stop();
        MediaNotificationService.hide(this);
        sendspin.close();
        startActivityForResult(mediaProjectionManager.createScreenCaptureIntent(), REQUEST_SYSTEM_AUDIO);
    }

    private void startSystemAudioService(int resultCode, Intent projectionData) {
        Intent service = new Intent(this, SystemAudioCaptureService.class);
        service.setAction(SystemAudioCaptureService.ACTION_START);
        service.putExtra(SystemAudioCaptureService.EXTRA_RESULT_CODE, resultCode);
        service.putExtra(SystemAudioCaptureService.EXTRA_PROJECTION_DATA, projectionData);
        service.putExtra(SystemAudioCaptureService.EXTRA_HOST, radioHost);
        service.putExtra(SystemAudioCaptureService.EXTRA_PORT, sendspinPort);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(service);
        else startService(service);
        if (systemAudioStatus != null) systemAudioStatus.setText("Spouštím stream zvuku…");
    }

    private void stopSystemAudioCapture() {
        Intent service = new Intent(this, SystemAudioCaptureService.class);
        service.setAction(SystemAudioCaptureService.ACTION_STOP);
        startService(service);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_RECORD_AUDIO) {
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                requestSystemAudioCapture();
            } else toast("Bez oprávnění nelze zvuk zachytit");
        } else if (requestCode == REQUEST_NOTIFICATIONS &&
                (grantResults.length == 0 || grantResults[0] != PackageManager.PERMISSION_GRANTED)) {
            toast("Bez povolených notifikací nebude ovládání vidět mimo aplikaci");
        }
    }

    private void startDiscovery() {
        if (nsdManager == null) return;
        stopDiscovery();
        resolveQueue.clear();
        resolving = false;
        if (settingsTestResult != null) settingsTestResult.setText("Hledám rádio v síti…");

        discoveryListener = new NsdManager.DiscoveryListener() {
            @Override public void onDiscoveryStarted(String serviceType) { setBottomStatus("Hledám ORIS rádio…", MUTED); }
            @Override public void onServiceLost(NsdServiceInfo serviceInfo) {}
            @Override public void onDiscoveryStopped(String serviceType) { discoveryRunning = false; }
            @Override public void onStartDiscoveryFailed(String serviceType, int errorCode) {
                discoveryRunning = false;
                runOnUiThread(() -> setBottomStatus("Hledání selhalo: " + errorCode, DANGER));
            }
            @Override public void onStopDiscoveryFailed(String serviceType, int errorCode) { discoveryRunning = false; }

            @Override
            public void onServiceFound(NsdServiceInfo serviceInfo) {
                String type = serviceInfo.getServiceType();
                if (type == null || !type.toLowerCase(Locale.ROOT).contains("_sendspin._tcp")) return;
                synchronized (resolveQueue) { resolveQueue.add(serviceInfo); }
                resolveNext();
            }
        };
        try {
            nsdManager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discoveryListener);
            discoveryRunning = true;
            mainHandler.postDelayed(this::stopDiscovery, 8000L);
        } catch (Exception e) {
            setBottomStatus("Hledání nelze spustit: " + e.getMessage(), DANGER);
        }
    }

    private void resolveNext() {
        synchronized (resolveQueue) {
            if (resolving || resolveQueue.isEmpty()) return;
            resolving = true;
            NsdServiceInfo service = resolveQueue.removeFirst();
            try {
                nsdManager.resolveService(service, new NsdManager.ResolveListener() {
                    @Override
                    public void onResolveFailed(NsdServiceInfo serviceInfo, int errorCode) {
                        synchronized (resolveQueue) { resolving = false; }
                        resolveNext();
                    }

                    @Override
                    public void onServiceResolved(NsdServiceInfo serviceInfo) {
                        InetAddress address = serviceInfo.getHost();
                        if (address != null) {
                            runOnUiThread(() -> {
                                String host = address.getHostAddress();
                                int port = serviceInfo.getPort() > 0 ? serviceInfo.getPort() : DEFAULT_SENDSPIN_PORT;
                                if (settingsHost != null) settingsHost.setText(host);
                                if (settingsPort != null) settingsPort.setText(String.valueOf(port));
                                if (settingsTestResult != null) {
                                    settingsTestResult.setTextColor(OK);
                                    settingsTestResult.setText("Nalezeno: " + serviceInfo.getServiceName() + " · " + host + ":" + port);
                                }
                                setBottomStatus("Nalezeno rádio " + host, OK);
                            });
                        }
                        synchronized (resolveQueue) { resolving = false; }
                        resolveNext();
                    }
                });
            } catch (Exception e) {
                resolving = false;
                resolveNext();
            }
        }
    }

    private void stopDiscovery() {
        if (!discoveryRunning || discoveryListener == null || nsdManager == null) return;
        try { nsdManager.stopServiceDiscovery(discoveryListener); }
        catch (Exception ignored) {}
        discoveryRunning = false;
    }

    private void acquireMulticastLock() {
        try {
            WifiManager manager = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            multicastLock = manager.createMulticastLock("oris-radio-discovery");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        } catch (Exception ignored) {}
    }

    private void registerMediaCommandReceiver() {
        IntentFilter filter = new IntentFilter(MediaNotificationService.ACTION_APP_COMMAND);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(mediaCommandReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(mediaCommandReceiver, filter);
        mediaCommandReceiverRegistered = true;
    }

    private void requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= 33 &&
                checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQUEST_NOTIFICATIONS);
        }
    }

    private void registerSystemAudioReceiver() {
        IntentFilter filter = new IntentFilter(SystemAudioCaptureService.ACTION_STATUS);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(systemAudioStatusReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(systemAudioStatusReceiver, filter);
        statusReceiverRegistered = true;
    }

    private void actionResult(String text, Exception error) {
        if (error != null) setBottomStatus(error.getMessage(), DANGER);
        else setBottomStatus(text == null || text.trim().isEmpty() ? "Hotovo" : text.trim(), OK);
    }

    private void setBottomStatus(String text, int color) {
        if (bottomStatus == null) return;
        bottomStatus.setText(text == null ? "" : text);
        bottomStatus.setTextColor(color);
    }

    private static abstract class SimpleSeekListener implements SeekBar.OnSeekBarChangeListener {
        public abstract void changed(int value, boolean fromUser);
        @Override public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) { changed(progress, fromUser); }
        @Override public void onStartTrackingTouch(SeekBar seekBar) {}
        @Override public void onStopTrackingTouch(SeekBar seekBar) {}
    }

    private ArrayAdapter<String> darkSpinnerAdapter(List<String> values) {
        return new ArrayAdapter<String>(this, android.R.layout.simple_spinner_dropdown_item, values) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                TextView view = (TextView) super.getView(position, convertView, parent);
                view.setTextColor(TEXT);
                view.setTextSize(14);
                view.setPadding(dp(12), dp(10), dp(12), dp(10));
                return view;
            }

            @Override
            public View getDropDownView(int position, View convertView, ViewGroup parent) {
                TextView view = (TextView) super.getDropDownView(position, convertView, parent);
                view.setTextColor(TEXT);
                view.setBackgroundColor(PANEL_2);
                view.setPadding(dp(12), dp(12), dp(12), dp(12));
                return view;
            }
        };
    }

    private CheckBox checkBox(String label, boolean checked) {
        CheckBox box = new CheckBox(this);
        box.setText(label);
        box.setTextColor(TEXT);
        box.setChecked(checked);
        box.setButtonTintList(new android.content.res.ColorStateList(
                new int[][]{new int[]{android.R.attr.state_checked}, new int[]{}},
                new int[]{ACCENT, MUTED}));
        box.setPadding(0, dp(4), 0, dp(4));
        return box;
    }

    private static String valueOf(EditText edit) {
        return edit == null ? "" : edit.getText().toString();
    }

    private static void setEdit(EditText edit, String value) {
        if (edit != null) edit.setText(value == null ? "" : value);
    }

    private static String dbText(int value) {
        return (value > 0 ? "+" : "") + value + " dB";
    }

    private void updateDetailedEqLabels() {
        if (eqPreampValue != null && eqPreampSeek != null) {
            eqPreampValue.setText("EQ preamp: " + dbText(eqPreampSeek.getProgress() - 24));
        }
        if (outputGainValue != null && outputGainSeek != null) {
            outputGainValue.setText("Maximální výstupní gain: " + dbText(outputGainSeek.getProgress() - 40));
        }
        if (volumeCurveValue != null && volumeCurveSeek != null) {
            double curve = 1.0 + volumeCurveSeek.getProgress() / 10.0;
            volumeCurveValue.setText(String.format(Locale.US, "Křivka hlasitosti: %.1f×", curve));
        }
        for (int i = 0; i < eqBandSeeks.length; i++) {
            if (eqBandSeeks[i] != null && eqBandValues[i] != null) {
                eqBandValues[i].setText(EQ_BAND_LABELS[i] + ": " + dbText(eqBandSeeks[i].getProgress() - 12));
            }
        }
    }

    private void applyEqPreset(String name) {
        int[] values;
        switch (name) {
            case "bass":
                values = new int[]{5, 5, 4, 3, 1, 0, -1, -1, 0, 1};
                break;
            case "voice":
                values = new int[]{-5, -4, -2, 0, 2, 4, 4, 2, 0, -2};
                break;
            case "loudness":
                values = new int[]{5, 4, 3, 1, -1, -2, -1, 1, 3, 4};
                break;
            case "flat":
            default:
                values = new int[]{0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
                break;
        }
        for (int i = 0; i < eqBandSeeks.length; i++) {
            if (eqBandSeeks[i] != null) eqBandSeeks[i].setProgress(values[i] + 12);
        }
        if ("flat".equals(name) && eqPreampSeek != null) eqPreampSeek.setProgress(24);
        updateDetailedEqLabels();
    }

    private LinearLayout verticalBody() {
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(14), dp(14), dp(14), dp(22));
        body.setBackgroundColor(BG);
        return body;
    }

    private ScrollView scroll(View content) {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(BG);
        scroll.addView(content);
        return scroll;
    }

    private LinearLayout card() {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(15), dp(15), dp(15), dp(15));
        card.setBackground(round(PANEL, dp(14), LINE, dp(1)));
        return card;
    }

    private TextView metric(String name, String value) {
        TextView view = text(name + "\n" + value, 13, TEXT, false);
        view.setPadding(dp(12), dp(10), dp(12), dp(10));
        view.setBackground(round(PANEL_2, dp(10), LINE, dp(1)));
        LinearLayout.LayoutParams p = matchWrapWithMargins(0, 0, 0, 6);
        view.setLayoutParams(p);
        return view;
    }

    private TextView loading(String message) {
        TextView view = text(message, 14, MUTED, false);
        view.setGravity(Gravity.CENTER_HORIZONTAL);
        view.setPadding(dp(12), dp(24), dp(12), dp(24));
        return view;
    }

    private TextView errorBox(String message) {
        TextView view = text(message == null ? "Chyba" : message, 14, Color.rgb(255, 116, 116), false);
        view.setPadding(dp(12), dp(12), dp(12), dp(12));
        view.setBackground(round(Color.rgb(45, 25, 29), dp(10), DANGER, dp(1)));
        return view;
    }

    private TextView pill(String value, int color) {
        TextView view = text(value, 11, Color.WHITE, true);
        view.setGravity(Gravity.CENTER);
        view.setPadding(dp(11), dp(6), dp(11), dp(6));
        view.setBackground(round(color, dp(20), color, 0));
        return view;
    }

    private EditText input(String hint, String value, boolean password) {
        EditText edit = new EditText(this);
        edit.setHint(hint);
        edit.setHintTextColor(MUTED);
        edit.setTextColor(TEXT);
        edit.setText(value == null ? "" : value);
        edit.setSingleLine(true);
        edit.setPadding(dp(12), dp(10), dp(12), dp(10));
        edit.setBackground(round(Color.rgb(16, 22, 31), dp(10), Color.rgb(51, 66, 84), dp(1)));
        if (password) edit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        return edit;
    }

    private TextView text(String value, float size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        if (bold) view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return view;
    }

    private Button textButton(String value, int color) {
        Button button = new Button(this);
        button.setText(value);
        button.setTextColor(Color.WHITE);
        button.setTextSize(13);
        button.setAllCaps(false);
        button.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        button.setPadding(dp(8), 0, dp(8), 0);
        button.setBackground(round(color, dp(10), color, 0));
        return button;
    }

    private Button circleButton(String value, int color, int sizeDp) {
        Button button = textButton(value, color);
        button.setTextSize(19);
        button.setGravity(Gravity.CENTER);
        button.setBackground(round(color, dp(sizeDp / 2f), color, 0));
        button.setLayoutParams(new LinearLayout.LayoutParams(dp(sizeDp), dp(sizeDp)));
        return button;
    }

    private LinearLayout horizontalRow() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        return row;
    }

    private GradientDrawable round(int color, float radius, int strokeColor, int strokeWidth) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(radius);
        if (strokeWidth > 0) drawable.setStroke(strokeWidth, strokeColor);
        return drawable;
    }

    private LinearLayout.LayoutParams weighted() {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, dp(48), 1f);
        p.setMargins(dp(3), dp(3), dp(3), dp(3));
        return p;
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams wrapCenter() {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p.gravity = Gravity.CENTER_HORIZONTAL;
        return p;
    }

    private LinearLayout.LayoutParams matchWrapWithMargins(int left, int top, int right, int bottom) {
        LinearLayout.LayoutParams p = matchWrap();
        p.setMargins(dp(left), dp(top), dp(right), dp(bottom));
        return p;
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static String cleanHost(String value) {
        String host = value == null ? "" : value.trim();
        host = host.replace("http://", "").replace("https://", "");
        int slash = host.indexOf('/');
        if (slash >= 0) host = host.substring(0, slash);
        int colon = host.lastIndexOf(':');
        if (colon > 0 && host.indexOf(':') == colon) host = host.substring(0, colon);
        return host.isEmpty() ? "192.168.1.177" : host;
    }

    private static String enc(String value) {
        try { return URLEncoder.encode(value == null ? "" : value, StandardCharsets.UTF_8.name()); }
        catch (Exception e) { return value == null ? "" : value; }
    }

    private static String parentPath(String path) {
        if (path == null || path.isEmpty() || "/".equals(path)) return "/";
        String value = path.endsWith("/") && path.length() > 1 ? path.substring(0, path.length() - 1) : path;
        int slash = value.lastIndexOf('/');
        return slash <= 0 ? "/" : value.substring(0, slash);
    }

    private static String fileName(String path) {
        if (path == null) return "";
        int slash = path.lastIndexOf('/');
        return slash >= 0 ? path.substring(slash + 1) : path;
    }

    private static String firstString(JSONObject object, String first, String second) {
        String value = object.optString(first, "");
        return value.isEmpty() ? object.optString(second, "") : value;
    }

    private static String niceBytes(long value) {
        if (value < 1024) return value + " B";
        if (value < 1024 * 1024) return String.format(Locale.getDefault(), "%.1f KB", value / 1024.0);
        return String.format(Locale.getDefault(), "%.1f MB", value / 1024.0 / 1024.0);
    }

    private static String formatTime(long millis) {
        long total = Math.max(0, millis / 1000L);
        return String.format(Locale.getDefault(), "%02d:%02d", total / 60L, total % 60L);
    }

    private void toast(String message) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
    }

    @Override
    protected void onPause() {
        if (currentPage == Page.SETTINGS) persistConnectionFields();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        mainHandler.removeCallbacks(statusTicker);
        stopDiscovery();
        if (statusReceiverRegistered) {
            try { unregisterReceiver(systemAudioStatusReceiver); }
            catch (Exception ignored) {}
        }
        if (mediaCommandReceiverRegistered) {
            try { unregisterReceiver(mediaCommandReceiver); }
            catch (Exception ignored) {}
        }
        pendingLocalPlay = false;
        streamer.stop();
        sendspin.close();
        try {
            if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
        } catch (Exception ignored) {}
        super.onDestroy();
    }
}
