package cz.oris.mobileaudio;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.IBinder;

import java.util.ArrayList;
import java.util.List;

public final class MediaNotificationService extends Service {
    public static final String ACTION_UPDATE = "cz.oris.mobileaudio.MEDIA_UPDATE";
    public static final String ACTION_HIDE = "cz.oris.mobileaudio.MEDIA_HIDE";
    public static final String ACTION_COMMAND = "cz.oris.mobileaudio.MEDIA_COMMAND";
    public static final String ACTION_APP_COMMAND = "cz.oris.mobileaudio.APP_MEDIA_COMMAND";

    public static final String COMMAND_PREVIOUS = "previous";
    public static final String COMMAND_TOGGLE = "toggle";
    public static final String COMMAND_NEXT = "next";
    public static final String COMMAND_STOP = "stop";

    private static final String EXTRA_COMMAND = "command";
    private static final String EXTRA_SOURCE = "source";
    private static final String EXTRA_TITLE = "title";
    private static final String EXTRA_SUBTITLE = "subtitle";
    private static final String EXTRA_PLAYING = "playing";
    private static final String EXTRA_NAVIGATION = "navigation";
    private static final String EXTRA_STATIONS = "stations";
    private static final String EXTRA_CURRENT_STATION = "current_station";

    public static final String SOURCE_MOBILE = "mobile";
    public static final String SOURCE_REMOTE = "remote";
    public static final String NAVIGATION_TRACK = "track";
    public static final String NAVIGATION_STATION = "station";

    private static final String CHANNEL_ID = "oris_media_playback";
    private static final int NOTIFICATION_ID = 3201;

    private String source = SOURCE_REMOTE;
    private String title = "ORIScore radio";
    private String subtitle = "Přehrávač";
    private boolean playing = true;
    private String navigation = NAVIGATION_TRACK;
    private String stations = "";
    private int currentStation = -1;

    public static void show(Context context, String source, String title, String subtitle,
                            boolean playing, String navigation, String stations, int currentStation) {
        Intent intent = new Intent(context, MediaNotificationService.class);
        intent.setAction(ACTION_UPDATE);
        intent.putExtra(EXTRA_SOURCE, source);
        intent.putExtra(EXTRA_TITLE, title);
        intent.putExtra(EXTRA_SUBTITLE, subtitle);
        intent.putExtra(EXTRA_PLAYING, playing);
        intent.putExtra(EXTRA_NAVIGATION, navigation);
        intent.putExtra(EXTRA_STATIONS, stations);
        intent.putExtra(EXTRA_CURRENT_STATION, currentStation);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) context.startForegroundService(intent);
        else context.startService(intent);
    }

    public static void hide(Context context) {
        Intent intent = new Intent(context, MediaNotificationService.class);
        intent.setAction(ACTION_HIDE);
        try { context.startService(intent); }
        catch (Exception ignored) {}
    }

    @Override
    public void onCreate() {
        super.onCreate();
        createChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            startForeground(NOTIFICATION_ID, buildNotification());
            return START_NOT_STICKY;
        }

        String action = intent.getAction();
        if (ACTION_HIDE.equals(action)) {
            stopForeground(true);
            stopSelf();
            return START_NOT_STICKY;
        }
        if (ACTION_COMMAND.equals(action)) {
            handleCommand(intent.getStringExtra(EXTRA_COMMAND));
            return START_NOT_STICKY;
        }

        source = safe(intent.getStringExtra(EXTRA_SOURCE), SOURCE_REMOTE);
        title = safe(intent.getStringExtra(EXTRA_TITLE), "ORIScore radio");
        subtitle = safe(intent.getStringExtra(EXTRA_SUBTITLE), "Přehrávač");
        playing = intent.getBooleanExtra(EXTRA_PLAYING, true);
        navigation = safe(intent.getStringExtra(EXTRA_NAVIGATION), NAVIGATION_TRACK);
        stations = safe(intent.getStringExtra(EXTRA_STATIONS), "");
        currentStation = intent.getIntExtra(EXTRA_CURRENT_STATION, -1);
        startForeground(NOTIFICATION_ID, buildNotification());
        return START_NOT_STICKY;
    }

    private void handleCommand(String command) {
        if (command == null) return;
        if (SOURCE_MOBILE.equals(source)) {
            Intent broadcast = new Intent(ACTION_APP_COMMAND);
            broadcast.setPackage(getPackageName());
            broadcast.putExtra(EXTRA_COMMAND, command);
            sendBroadcast(broadcast);
            if (COMMAND_TOGGLE.equals(command)) {
                playing = !playing;
                updateNotification();
            } else if (COMMAND_STOP.equals(command)) {
                stopForeground(true);
                stopSelf();
            }
            return;
        }

        SharedPreferences prefs = getSharedPreferences("oris_audio", MODE_PRIVATE);
        String host = prefs.getString("radio_host", "192.168.1.177");
        String user = prefs.getString("web_user", "admin");
        String password = prefs.getString("web_pass", "admin");
        OrisApi api = new OrisApi(this, host, user, password);

        if (COMMAND_PREVIOUS.equals(command) || COMMAND_NEXT.equals(command)) {
            if (NAVIGATION_STATION.equals(navigation)) {
                int nextStation = relativeStation(COMMAND_NEXT.equals(command) ? 1 : -1);
                if (nextStation >= 0) {
                    currentStation = nextStation;
                    api.post("/radio/play?i=" + nextStation, (text, error) -> {});
                }
            } else {
                api.post(COMMAND_NEXT.equals(command) ? "/audio/next" : "/audio/prev", (text, error) -> {});
            }
        } else if (COMMAND_TOGGLE.equals(command)) {
            playing = !playing;
            api.post("/audio/toggle_pause", (text, error) -> {});
            updateNotification();
        } else if (COMMAND_STOP.equals(command)) {
            api.post("/audio/stop_ajax", (text, error) -> {});
            stopForeground(true);
            stopSelf();
        }
    }

    private int relativeStation(int delta) {
        List<Integer> values = new ArrayList<>();
        for (String part : stations.split(",")) {
            try { values.add(Integer.parseInt(part.trim())); }
            catch (Exception ignored) {}
        }
        if (values.isEmpty()) return -1;
        int position = values.indexOf(currentStation);
        if (position < 0) position = 0;
        position = (position + delta + values.size()) % values.size();
        return values.get(position);
    }

    private void updateNotification() {
        NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (manager != null) manager.notify(NOTIFICATION_ID, buildNotification());
    }

    private Notification buildNotification() {
        Intent open = new Intent(this, MainActivity.class);
        open.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent contentIntent = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | immutableFlag());

        Notification.Action previous = new Notification.Action.Builder(
                android.R.drawable.ic_media_previous, "Předchozí", commandIntent(COMMAND_PREVIOUS, 1)).build();
        Notification.Action toggle = new Notification.Action.Builder(
                playing ? android.R.drawable.ic_media_pause : android.R.drawable.ic_media_play,
                playing ? "Pauza" : "Přehrát", commandIntent(COMMAND_TOGGLE, 2)).build();
        Notification.Action next = new Notification.Action.Builder(
                android.R.drawable.ic_media_next, "Další", commandIntent(COMMAND_NEXT, 3)).build();
        Notification.Action stop = new Notification.Action.Builder(
                android.R.drawable.ic_menu_close_clear_cancel, "Stop", commandIntent(COMMAND_STOP, 4)).build();

        Notification.MediaStyle mediaStyle = new Notification.MediaStyle()
                .setShowActionsInCompactView(0, 1, 2);

        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        return builder
                .setSmallIcon(R.drawable.ic_stat_oris)
                .setContentTitle(title)
                .setContentText(subtitle)
                .setSubText(SOURCE_MOBILE.equals(source) ? "Stream z mobilu" : "ORIS rádio")
                .setContentIntent(contentIntent)
                .setCategory(Notification.CATEGORY_TRANSPORT)
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setStyle(mediaStyle)
                .addAction(previous)
                .addAction(toggle)
                .addAction(next)
                .addAction(stop)
                .build();
    }

    private PendingIntent commandIntent(String command, int requestCode) {
        Intent intent = new Intent(this, MediaNotificationService.class);
        intent.setAction(ACTION_COMMAND);
        intent.putExtra(EXTRA_COMMAND, command);
        return PendingIntent.getService(this, requestCode, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | immutableFlag());
    }

    private void createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID, "Ovládání přehrávání", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Stanice a skladby přehrávané přes ORIScore radio");
        channel.setShowBadge(false);
        NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (manager != null) manager.createNotificationChannel(channel);
    }

    private static int immutableFlag() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.M ? PendingIntent.FLAG_IMMUTABLE : 0;
    }

    private static String safe(String value, String fallback) {
        return value == null || value.trim().isEmpty() ? fallback : value;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
