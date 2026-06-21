package cz.oris.mobileaudio;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;

import java.util.concurrent.atomic.AtomicBoolean;

public final class SystemAudioCaptureService extends Service {
    public static final String ACTION_START = "cz.oris.mobileaudio.START_SYSTEM_AUDIO";
    public static final String ACTION_STOP = "cz.oris.mobileaudio.STOP_SYSTEM_AUDIO";
    public static final String ACTION_STATUS = "cz.oris.mobileaudio.SYSTEM_AUDIO_STATUS";
    public static final String EXTRA_STATUS = "status";
    public static final String EXTRA_RUNNING = "running";
    public static final String EXTRA_RESULT_CODE = "result_code";
    public static final String EXTRA_PROJECTION_DATA = "projection_data";
    public static final String EXTRA_HOST = "host";
    public static final String EXTRA_PORT = "port";

    private static final String CHANNEL_ID = "oris_mobile_audio_stream";
    private static final int NOTIFICATION_ID = 4102;
    private static final int SAMPLE_RATE = 48_000;
    private static final int CHANNEL_COUNT = 2;
    private static final int BYTES_PER_SAMPLE = 2;
    private static final int BYTES_PER_FRAME = CHANNEL_COUNT * BYTES_PER_SAMPLE;
    private static final int CHUNK_MS = 20;
    private static final int CHUNK_BYTES = SAMPLE_RATE * BYTES_PER_FRAME * CHUNK_MS / 1000;
    private static final long START_BUFFER_US = 750_000L;

    private final AtomicBoolean stopping = new AtomicBoolean(false);
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private MediaProjection mediaProjection;
    private AudioRecord audioRecord;
    private Thread captureThread;
    private SendspinConnection sendspin;
    private Intent projectionData;
    private int projectionResultCode;
    private String radioHost;
    private int radioPort;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) return START_NOT_STICKY;
        String action = intent.getAction();
        if (ACTION_STOP.equals(action)) {
            stopCapture("Stream zvuku telefonu zastaven");
            return START_NOT_STICKY;
        }
        if (!ACTION_START.equals(action)) return START_NOT_STICKY;

        startForeground(NOTIFICATION_ID, buildNotification("Připojuji rádio…"));
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            stopCapture("Stream zvuku vyžaduje Android 10 nebo novější");
            return START_NOT_STICKY;
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            stopCapture("Chybí oprávnění k záznamu zvuku");
            return START_NOT_STICKY;
        }

        projectionResultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0);
        if (Build.VERSION.SDK_INT >= 33) {
            projectionData = intent.getParcelableExtra(EXTRA_PROJECTION_DATA, Intent.class);
        } else {
            projectionData = intent.getParcelableExtra(EXTRA_PROJECTION_DATA);
        }
        radioHost = intent.getStringExtra(EXTRA_HOST);
        radioPort = intent.getIntExtra(EXTRA_PORT, 8928);

        if (projectionData == null || radioHost == null || radioHost.trim().isEmpty()) {
            stopCapture("Chybí údaje pro spuštění streamu");
            return START_NOT_STICKY;
        }

        stopping.set(false);
        connectToRadio();
        return START_NOT_STICKY;
    }

    private void connectToRadio() {
        sendStatus("Připojuji se k rádiu " + radioHost + ":" + radioPort + "…", false);
        sendspin = new SendspinConnection(new SendspinConnection.Listener() {
            @Override
            public void onStatus(String message) {
                sendStatus(message, false);
            }

            @Override
            public void onReady(String radioName) {
                sendStatus("Připojeno k " + radioName + ", spouštím záznam zvuku…", true);
                startProjectionAndCapture();
            }

            @Override
            public void onDisconnected(String reason) {
                if (!stopping.get()) stopCapture(reason);
            }

            @Override
            public void onRadioVolume(int volume) {
            }
        });
        sendspin.connectAsync(radioHost, radioPort);
    }

    private void startProjectionAndCapture() {
        if (stopping.get() || captureThread != null) return;
        try {
            MediaProjectionManager manager =
                    (MediaProjectionManager) getSystemService(MEDIA_PROJECTION_SERVICE);
            mediaProjection = manager.getMediaProjection(projectionResultCode, projectionData);
            if (mediaProjection == null) {
                stopCapture("Android nepovolil zachycení zvuku");
                return;
            }
            mediaProjection.registerCallback(new MediaProjection.Callback() {
                @Override
                public void onStop() {
                    stopCapture("Android ukončil sdílení zvuku");
                }
            }, mainHandler);

            captureThread = new Thread(this::captureLoop, "OrisSystemAudioCapture");
            captureThread.start();
        } catch (Exception e) {
            stopCapture("Záznam zvuku nelze spustit: " + safeMessage(e));
        }
    }

    private void captureLoop() {
        try {
            AudioPlaybackCaptureConfiguration captureConfig =
                    new AudioPlaybackCaptureConfiguration.Builder(mediaProjection)
                            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                            .addMatchingUsage(AudioAttributes.USAGE_GAME)
                            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                            .build();

            AudioFormat format = new AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                    .build();

            int minimum = AudioRecord.getMinBufferSize(
                    SAMPLE_RATE,
                    AudioFormat.CHANNEL_IN_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT);
            int bufferSize = Math.max(CHUNK_BYTES * 8,
                    minimum > 0 ? minimum * 2 : CHUNK_BYTES * 8);

            audioRecord = new AudioRecord.Builder()
                    .setAudioFormat(format)
                    .setBufferSizeInBytes(bufferSize)
                    .setAudioPlaybackCaptureConfig(captureConfig)
                    .build();

            if (audioRecord.getState() != AudioRecord.STATE_INITIALIZED) {
                throw new IllegalStateException("AudioRecord se neinicializoval");
            }

            Thread.sleep(500L);
            sendspin.sendStreamStart(SAMPLE_RATE);
            sendspin.sendGroupPlaying(true);
            sendspin.sendMetadata("Zvuk telefonu", 0L, 0L, true);

            audioRecord.startRecording();
            sendStatus("Stream zvuku telefonu běží", true);
            updateNotification("Stream zvuku telefonu běží");

            byte[] buffer = new byte[CHUNK_BYTES];
            long nextTimestampUs = SendspinConnection.nowUs() + START_BUFFER_US;
            long lastStatusMs = 0L;

            while (!stopping.get()) {
                int read = audioRecord.read(buffer, 0, buffer.length, AudioRecord.READ_BLOCKING);
                if (read < 0) throw new IllegalStateException("AudioRecord chyba " + read);
                if (read == 0) continue;
                read -= read % BYTES_PER_FRAME;
                if (read <= 0) continue;

                sendspin.sendAudio(nextTimestampUs, buffer, 0, read);
                int frames = read / BYTES_PER_FRAME;
                nextTimestampUs += frames * 1_000_000L / SAMPLE_RATE;

                long nowMs = System.currentTimeMillis();
                if (nowMs - lastStatusMs >= 1000L) {
                    lastStatusMs = nowMs;
                    int level = calculateLevel(buffer, read);
                    sendStatus("Stream zvuku telefonu běží — úroveň " + level + " %", true);
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (Exception e) {
            if (!stopping.get()) {
                String reason = "Chyba streamu: " + safeMessage(e);
                mainHandler.post(() -> stopCapture(reason));
            }
        } finally {
            releaseAudioRecord();
        }
    }

    private int calculateLevel(byte[] data, int length) {
        int peak = 0;
        for (int i = 0; i + 1 < length; i += 2) {
            int value = (short) ((data[i] & 0xFF) | (data[i + 1] << 8));
            int absolute = Math.abs(value);
            if (absolute > peak) peak = absolute;
        }
        return Math.min(100, Math.round(peak * 100f / 32767f));
    }

    private synchronized void stopCapture(String reason) {
        if (!stopping.compareAndSet(false, true)) return;
        sendStatus(reason, false);

        Thread thread = captureThread;
        captureThread = null;
        if (thread != null) thread.interrupt();

        releaseAudioRecord();

        if (sendspin != null) {
            sendspin.sendGroupPlaying(false);
            sendspin.sendStreamEnd();
            sendspin.close();
            sendspin = null;
        }

        MediaProjection projection = mediaProjection;
        mediaProjection = null;
        if (projection != null) {
            try {
                projection.stop();
            } catch (Exception ignored) {
            }
        }

        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    private synchronized void releaseAudioRecord() {
        AudioRecord record = audioRecord;
        audioRecord = null;
        if (record != null) {
            try {
                record.stop();
            } catch (Exception ignored) {
            }
            try {
                record.release();
            } catch (Exception ignored) {
            }
        }
    }

    private void sendStatus(String text, boolean running) {
        updateNotification(text);
        Intent intent = new Intent(ACTION_STATUS);
        intent.setPackage(getPackageName());
        intent.putExtra(EXTRA_STATUS, text);
        intent.putExtra(EXTRA_RUNNING, running);
        sendBroadcast(intent);
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationManager manager = getSystemService(NotificationManager.class);
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID,
                    "ORIS stream zvuku",
                    NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Přenos zvuku telefonu do ORIS rádia");
            manager.createNotificationChannel(channel);
        }
    }

    private Notification buildNotification(String text) {
        Intent stopIntent = new Intent(this, SystemAudioCaptureService.class);
        stopIntent.setAction(ACTION_STOP);
        PendingIntent stopPending = PendingIntent.getService(
                this,
                1,
                stopIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Intent openIntent = new Intent(this, MainActivity.class);
        PendingIntent openPending = PendingIntent.getActivity(
                this,
                2,
                openIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        return builder
                .setContentTitle("ORIScore radio")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setOngoing(true)
                .setContentIntent(openPending)
                .addAction(new Notification.Action.Builder(
                        android.R.drawable.ic_media_pause,
                        "Zastavit",
                        stopPending).build())
                .build();
    }

    private void updateNotification(String text) {
        NotificationManager manager =
                (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        manager.notify(NOTIFICATION_ID, buildNotification(text));
    }

    private static String safeMessage(Exception e) {
        String message = e.getMessage();
        return message == null || message.trim().isEmpty()
                ? e.getClass().getSimpleName() : message;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        if (!stopping.get()) stopCapture("Stream ukončen");
        super.onDestroy();
    }
}
