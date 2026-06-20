package cz.oris.mobileaudio;

import android.app.Activity;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.net.InetAddress;
import java.util.ArrayList;
import java.util.LinkedList;
import java.util.Locale;

public final class MainActivity extends Activity {
    private static final int PICK_AUDIO = 1001;
    private static final String SERVICE_TYPE = "_sendspin._tcp.";
    private static final int DEFAULT_SENDSPIN_PORT = 8928;

    private final ArrayList<RadioDevice> radios = new ArrayList<>();
    private final ArrayList<Uri> playlistUris = new ArrayList<>();
    private final ArrayList<String> playlistNames = new ArrayList<>();
    private final LinkedList<NsdServiceInfo> resolveQueue = new LinkedList<>();

    private NsdManager nsdManager;
    private NsdManager.DiscoveryListener discoveryListener;
    private boolean discoveryRunning;
    private boolean resolving;
    private WifiManager.MulticastLock multicastLock;

    private ArrayAdapter<RadioDevice> radioAdapter;
    private ArrayAdapter<String> playlistAdapter;
    private Spinner radioSpinner;
    private EditText addressEdit;
    private TextView statusText;
    private TextView trackText;
    private TextView progressText;
    private ProgressBar progressBar;
    private ListView playlistView;
    private SeekBar volumeSeek;
    private Button connectButton;
    private Button pauseButton;

    private SendspinConnection connection;
    private AudioStreamer streamer;
    private int selectedTrackIndex;
    private SharedPreferences preferences;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        preferences = getSharedPreferences("oris_audio", MODE_PRIVATE);
        nsdManager = (NsdManager) getSystemService(Context.NSD_SERVICE);
        acquireMulticastLock();

        connection = new SendspinConnection(new SendspinConnection.Listener() {
            @Override
            public void onStatus(String message) {
                runOnUiThread(() -> setStatus(message));
            }

            @Override
            public void onReady(String radioName) {
                runOnUiThread(() -> {
                    connectButton.setText("Odpojit");
                    setStatus("Připojeno k: " + radioName);
                });
            }

            @Override
            public void onDisconnected(String reason) {
                runOnUiThread(() -> {
                    connectButton.setText("Připojit");
                    setStatus(reason);
                    streamer.stop();
                });
            }

            @Override
            public void onRadioVolume(int volume) {
                runOnUiThread(() -> volumeSeek.setProgress(volume));
            }
        });

        streamer = new AudioStreamer(this, connection, new AudioStreamer.Listener() {
            @Override
            public void onStatus(String message) {
                runOnUiThread(() -> setStatus(message));
            }

            @Override
            public void onTrackChanged(int index, String name) {
                runOnUiThread(() -> {
                    selectedTrackIndex = index;
                    trackText.setText(name);
                    playlistView.setItemChecked(index, true);
                    pauseButton.setText("Pauza");
                });
            }

            @Override
            public void onProgress(long positionMs, long durationMs) {
                runOnUiThread(() -> updateProgress(positionMs, durationMs));
            }

            @Override
            public void onStopped() {
                runOnUiThread(() -> {
                    pauseButton.setText("Pauza");
                    updateProgress(0, 0);
                });
            }
        });

        setContentView(buildUi());
        startDiscovery();
    }

    private View buildUi() {
        int padding = dp(16);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(padding, padding, padding, padding);

        TextView title = new TextView(this);
        title.setText("ORIS Mobile Audio");
        title.setTextSize(26);
        title.setTextColor(Color.rgb(20, 20, 20));
        title.setGravity(Gravity.CENTER_HORIZONTAL);
        content.addView(title, matchWrap());

        TextView subtitle = new TextView(this);
        subtitle.setText("Přehrávání skladeb z telefonu přímo do rádia přes Wi‑Fi");
        subtitle.setTextSize(15);
        subtitle.setGravity(Gravity.CENTER_HORIZONTAL);
        subtitle.setPadding(0, dp(4), 0, dp(16));
        content.addView(subtitle, matchWrap());

        statusText = new TextView(this);
        statusText.setText("Hledám rádio…");
        statusText.setTextSize(16);
        statusText.setPadding(dp(12), dp(10), dp(12), dp(10));
        statusText.setBackgroundColor(Color.rgb(235, 235, 235));
        content.addView(statusText, matchWrap());

        content.addView(label("Nalezená rádia"), matchWrap());

        radioSpinner = new Spinner(this);
        radioAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, radios);
        radioSpinner.setAdapter(radioAdapter);
        content.addView(radioSpinner, matchWrap());

        LinearLayout scanRow = horizontalRow();
        Button scanButton = button("Hledat znovu");
        scanButton.setOnClickListener(v -> startDiscovery());
        scanRow.addView(scanButton, weighted());

        Button useSelectedButton = button("Použít nalezené");
        useSelectedButton.setOnClickListener(v -> useSelectedRadio());
        scanRow.addView(useSelectedButton, weighted());
        content.addView(scanRow, matchWrap());

        content.addView(label("Ruční adresa rádia"), matchWrap());

        addressEdit = new EditText(this);
        addressEdit.setSingleLine(true);
        addressEdit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        addressEdit.setHint("např. 192.168.1.177 nebo 192.168.1.177:8928");
        addressEdit.setText(preferences.getString("radio_address", ""));
        content.addView(addressEdit, matchWrap());

        connectButton = button("Připojit");
        connectButton.setOnClickListener(v -> toggleConnection());
        content.addView(connectButton, matchWrap());

        content.addView(label("Hlasitost rádia"), matchWrap());

        volumeSeek = new SeekBar(this);
        volumeSeek.setMax(100);
        volumeSeek.setProgress(50);
        volumeSeek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (fromUser && connection.isProtocolReady()) connection.sendVolume(progress);
            }

            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });
        content.addView(volumeSeek, matchWrap());

        Button chooseButton = button("Vybrat skladby z telefonu");
        chooseButton.setOnClickListener(v -> chooseAudioFiles());
        content.addView(chooseButton, matchWrap());

        trackText = new TextView(this);
        trackText.setText("Není vybraná skladba");
        trackText.setTextSize(18);
        trackText.setGravity(Gravity.CENTER_HORIZONTAL);
        trackText.setPadding(0, dp(16), 0, dp(8));
        content.addView(trackText, matchWrap());

        progressText = new TextView(this);
        progressText.setText("00:00 / 00:00");
        progressText.setGravity(Gravity.CENTER_HORIZONTAL);
        content.addView(progressText, matchWrap());

        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setMax(1000);
        content.addView(progressBar, matchWrap());

        LinearLayout transport = horizontalRow();
        Button previousButton = button("◀◀");
        previousButton.setOnClickListener(v -> streamer.previous());
        transport.addView(previousButton, weighted());

        Button playButton = button("Přehrát");
        playButton.setOnClickListener(v -> playSelected());
        transport.addView(playButton, weighted());

        pauseButton = button("Pauza");
        pauseButton.setOnClickListener(v -> {
            streamer.pauseOrResume();
            pauseButton.setText(streamer.isPaused() ? "Pokračovat" : "Pauza");
        });
        transport.addView(pauseButton, weighted());

        Button nextButton = button("▶▶");
        nextButton.setOnClickListener(v -> streamer.next());
        transport.addView(nextButton, weighted());
        content.addView(transport, matchWrap());

        Button stopButton = button("Stop");
        stopButton.setOnClickListener(v -> streamer.stop());
        content.addView(stopButton, matchWrap());

        content.addView(label("Playlist"), matchWrap());

        playlistView = new ListView(this);
        playlistView.setChoiceMode(ListView.CHOICE_MODE_SINGLE);
        playlistAdapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_single_choice, playlistNames);
        playlistView.setAdapter(playlistAdapter);
        playlistView.setOnItemClickListener((parent, view, position, id) -> {
            selectedTrackIndex = position;
            trackText.setText(playlistNames.get(position));
        });
        content.addView(playlistView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(260)));

        TextView note = new TextView(this);
        note.setText("První verze podporuje běžné audio soubory, které umí dekódovat Android. "
                + "Telefon i rádio musí být ve stejné Wi‑Fi. Během přehrávání nech aplikaci otevřenou.");
        note.setTextSize(13);
        note.setPadding(0, dp(12), 0, dp(16));
        content.addView(note, matchWrap());

        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        return scroll;
    }

    private void chooseAudioFiles() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("audio/*");
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(intent, PICK_AUDIO);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_AUDIO || resultCode != RESULT_OK || data == null) return;

        playlistUris.clear();
        playlistNames.clear();

        ClipData clipData = data.getClipData();
        if (clipData != null) {
            for (int i = 0; i < clipData.getItemCount(); i++) {
                addAudioUri(clipData.getItemAt(i).getUri(), data.getFlags());
            }
        } else if (data.getData() != null) {
            addAudioUri(data.getData(), data.getFlags());
        }

        playlistAdapter.notifyDataSetChanged();
        selectedTrackIndex = 0;
        if (!playlistNames.isEmpty()) {
            trackText.setText(playlistNames.get(0));
            playlistView.setItemChecked(0, true);
            setStatus("Vybráno skladeb: " + playlistNames.size());
        }
    }

    private void addAudioUri(Uri uri, int flags) {
        if (uri == null) return;
        try {
            int takeFlags = flags & Intent.FLAG_GRANT_READ_URI_PERMISSION;
            getContentResolver().takePersistableUriPermission(uri, takeFlags);
        } catch (Exception ignored) {
        }
        playlistUris.add(uri);
        playlistNames.add(displayName(uri));
    }

    private String displayName(Uri uri) {
        Cursor cursor = null;
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

    private void playSelected() {
        if (playlistUris.isEmpty()) {
            toast("Nejdřív vyber skladby");
            return;
        }
        if (!connection.isProtocolReady()) {
            toast("Nejdřív připoj rádio");
            return;
        }
        streamer.play(playlistUris, playlistNames, selectedTrackIndex);
    }

    private void toggleConnection() {
        if (connection.isConnected()) {
            streamer.stop();
            connection.close();
            connectButton.setText("Připojit");
            return;
        }

        String address = addressEdit.getText().toString().trim();
        if (address.isEmpty()) {
            useSelectedRadio();
            address = addressEdit.getText().toString().trim();
        }
        if (address.isEmpty()) {
            toast("Vyber nalezené rádio nebo zadej jeho IP adresu");
            return;
        }

        HostPort hostPort = parseAddress(address);
        preferences.edit().putString("radio_address", address).apply();
        connection.connectAsync(hostPort.host, hostPort.port);
    }

    private void useSelectedRadio() {
        if (radios.isEmpty()) {
            toast("Zatím nebylo nalezeno žádné rádio");
            return;
        }
        RadioDevice selected = (RadioDevice) radioSpinner.getSelectedItem();
        if (selected == null) selected = radios.get(0);
        addressEdit.setText(selected.host + ":" + selected.port);
    }

    private HostPort parseAddress(String address) {
        String value = address.trim()
                .replace("ws://", "")
                .replace("http://", "")
                .replace("/sendspin", "");
        int slash = value.indexOf('/');
        if (slash >= 0) value = value.substring(0, slash);

        String host = value;
        int port = DEFAULT_SENDSPIN_PORT;
        int colon = value.lastIndexOf(':');
        if (colon > 0 && value.indexOf(':') == colon) {
            host = value.substring(0, colon);
            try {
                port = Integer.parseInt(value.substring(colon + 1));
            } catch (NumberFormatException ignored) {
                port = DEFAULT_SENDSPIN_PORT;
            }
        }
        return new HostPort(host, port);
    }

    private void startDiscovery() {
        stopDiscovery();
        radios.clear();
        radioAdapter.notifyDataSetChanged();
        resolveQueue.clear();
        resolving = false;

        discoveryListener = new NsdManager.DiscoveryListener() {
            @Override
            public void onDiscoveryStarted(String serviceType) {
                runOnUiThread(() -> setStatus("Hledám rádio v síti…"));
            }

            @Override
            public void onServiceFound(NsdServiceInfo serviceInfo) {
                String type = serviceInfo.getServiceType();
                if (type == null || !type.toLowerCase(Locale.ROOT).contains("_sendspin._tcp")) return;
                synchronized (resolveQueue) {
                    resolveQueue.add(serviceInfo);
                }
                resolveNext();
            }

            @Override
            public void onServiceLost(NsdServiceInfo serviceInfo) {
                String name = serviceInfo.getServiceName();
                runOnUiThread(() -> {
                    for (int i = radios.size() - 1; i >= 0; i--) {
                        if (radios.get(i).name.equals(name)) radios.remove(i);
                    }
                    radioAdapter.notifyDataSetChanged();
                });
            }

            @Override
            public void onDiscoveryStopped(String serviceType) {
                discoveryRunning = false;
            }

            @Override
            public void onStartDiscoveryFailed(String serviceType, int errorCode) {
                discoveryRunning = false;
                runOnUiThread(() -> setStatus("Hledání rádia selhalo: " + errorCode));
            }

            @Override
            public void onStopDiscoveryFailed(String serviceType, int errorCode) {
                discoveryRunning = false;
            }
        };

        try {
            nsdManager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discoveryListener);
            discoveryRunning = true;
        } catch (Exception e) {
            setStatus("Hledání rádia nelze spustit: " + e.getMessage());
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
                        synchronized (resolveQueue) {
                            resolving = false;
                        }
                        resolveNext();
                    }

                    @Override
                    public void onServiceResolved(NsdServiceInfo serviceInfo) {
                        InetAddress hostAddress = serviceInfo.getHost();
                        if (hostAddress != null) {
                            RadioDevice device = new RadioDevice(
                                    serviceInfo.getServiceName(),
                                    hostAddress.getHostAddress(),
                                    serviceInfo.getPort() > 0 ? serviceInfo.getPort() : DEFAULT_SENDSPIN_PORT);
                            runOnUiThread(() -> addOrReplaceRadio(device));
                        }
                        synchronized (resolveQueue) {
                            resolving = false;
                        }
                        resolveNext();
                    }
                });
            } catch (Exception e) {
                resolving = false;
                resolveNext();
            }
        }
    }

    private void addOrReplaceRadio(RadioDevice device) {
        for (int i = 0; i < radios.size(); i++) {
            RadioDevice existing = radios.get(i);
            if (existing.name.equals(device.name) || existing.host.equals(device.host)) {
                radios.set(i, device);
                radioAdapter.notifyDataSetChanged();
                return;
            }
        }
        radios.add(device);
        radioAdapter.notifyDataSetChanged();
        setStatus("Nalezeno rádio: " + device.name);
        if (addressEdit.getText().toString().trim().isEmpty()) {
            addressEdit.setText(device.host + ":" + device.port);
        }
    }

    private void stopDiscovery() {
        if (!discoveryRunning || discoveryListener == null || nsdManager == null) return;
        try {
            nsdManager.stopServiceDiscovery(discoveryListener);
        } catch (Exception ignored) {
        }
        discoveryRunning = false;
    }

    private void acquireMulticastLock() {
        try {
            WifiManager manager = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            multicastLock = manager.createMulticastLock("oris-sendspin-discovery");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        } catch (Exception ignored) {
        }
    }

    private void updateProgress(long positionMs, long durationMs) {
        progressText.setText(formatTime(positionMs) + " / " + formatTime(durationMs));
        if (durationMs > 0) {
            progressBar.setProgress((int) Math.min(1000L, positionMs * 1000L / durationMs));
        } else {
            progressBar.setProgress(0);
        }
    }

    private static String formatTime(long millis) {
        long totalSeconds = Math.max(0L, millis / 1000L);
        long minutes = totalSeconds / 60L;
        long seconds = totalSeconds % 60L;
        return String.format(Locale.getDefault(), "%02d:%02d", minutes, seconds);
    }

    private void setStatus(String text) {
        if (statusText != null) statusText.setText(text);
    }

    private void toast(String text) {
        Toast.makeText(this, text, Toast.LENGTH_SHORT).show();
    }

    private TextView label(String text) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(16);
        view.setPadding(0, dp(14), 0, dp(4));
        return view;
    }

    private Button button(String text) {
        Button button = new Button(this);
        button.setText(text);
        button.setAllCaps(false);
        return button;
    }

    private LinearLayout horizontalRow() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        return row;
    }

    private LinearLayout.LayoutParams weighted() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        params.setMargins(dp(2), dp(2), dp(2), dp(2));
        return params;
    }

    private LinearLayout.LayoutParams matchWrap() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.setMargins(0, dp(3), 0, dp(3));
        return params;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onDestroy() {
        stopDiscovery();
        streamer.stop();
        connection.close();
        try {
            if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
        } catch (Exception ignored) {
        }
        super.onDestroy();
    }

    private static final class RadioDevice {
        final String name;
        final String host;
        final int port;

        RadioDevice(String name, String host, int port) {
            this.name = name == null || name.trim().isEmpty() ? "ORIS rádio" : name;
            this.host = host;
            this.port = port;
        }

        @Override
        public String toString() {
            return name + " — " + host + ":" + port;
        }
    }

    private static final class HostPort {
        final String host;
        final int port;

        HostPort(String host, int port) {
            this.host = host;
            this.port = port;
        }
    }
}
