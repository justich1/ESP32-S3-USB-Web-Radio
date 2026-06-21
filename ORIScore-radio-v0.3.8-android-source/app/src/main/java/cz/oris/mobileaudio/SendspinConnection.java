package cz.oris.mobileaudio;

import android.util.Base64;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.UUID;

public final class SendspinConnection {
    public interface Listener {
        void onStatus(String message);
        void onReady(String radioName);
        void onDisconnected(String reason);
        void onRadioVolume(int volume);
    }

    private static final String PATH = "/sendspin";
    private static final SecureRandom RANDOM = new SecureRandom();

    private final Listener listener;
    private final Object writeLock = new Object();
    private final String serverId = "oris-mobile-" + UUID.randomUUID();

    private volatile Socket socket;
    private volatile InputStream input;
    private volatile OutputStream output;
    private volatile boolean connected;
    private volatile boolean protocolReady;
    private volatile String radioName = "Rádio";

    public SendspinConnection(Listener listener) {
        this.listener = listener;
    }

    public boolean isConnected() {
        return connected;
    }

    public boolean isProtocolReady() {
        return protocolReady;
    }

    public void connectAsync(String host, int port) {
        close();
        Thread thread = new Thread(() -> {
            try {
                notifyStatus("Připojuji se k " + host + ":" + port + "…");
                Socket s = new Socket();
                s.connect(new InetSocketAddress(host, port), 6000);
                s.setTcpNoDelay(true);
                s.setKeepAlive(true);

                InputStream in = s.getInputStream();
                OutputStream out = s.getOutputStream();
                performHandshake(host, port, in, out);

                socket = s;
                input = in;
                output = out;
                connected = true;
                protocolReady = false;
                notifyStatus("WebSocket připojen, čekám na rádio…");

                new Thread(this::readerLoop, "SendspinReader").start();
            } catch (Exception e) {
                closeInternal("Připojení selhalo: " + safeMessage(e), true);
            }
        }, "SendspinConnect");
        thread.start();
    }

    private void performHandshake(String host, int port, InputStream in, OutputStream out) throws IOException {
        byte[] nonce = new byte[16];
        RANDOM.nextBytes(nonce);
        String key = Base64.encodeToString(nonce, Base64.NO_WRAP);

        String request = "GET " + PATH + " HTTP/1.1\r\n"
                + "Host: " + host + ":" + port + "\r\n"
                + "Upgrade: websocket\r\n"
                + "Connection: Upgrade\r\n"
                + "Sec-WebSocket-Key: " + key + "\r\n"
                + "Sec-WebSocket-Version: 13\r\n"
                + "User-Agent: ORIScore-radio/0.3.8\r\n\r\n";
        out.write(request.getBytes(StandardCharsets.US_ASCII));
        out.flush();

        ByteArrayOutputStream response = new ByteArrayOutputStream();
        int matched = 0;
        while (response.size() < 8192) {
            int value = in.read();
            if (value < 0) throw new EOFException("Rádio ukončilo handshake");
            response.write(value);
            if ((matched == 0 && value == '\r')
                    || (matched == 1 && value == '\n')
                    || (matched == 2 && value == '\r')
                    || (matched == 3 && value == '\n')) {
                matched++;
                if (matched == 4) break;
            } else {
                matched = value == '\r' ? 1 : 0;
            }
        }

        String headers = response.toString(StandardCharsets.US_ASCII.name());
        if (!headers.startsWith("HTTP/1.1 101") && !headers.startsWith("HTTP/1.0 101")) {
            throw new IOException("WebSocket odmítnut: " + headers.split("\r?\n", 2)[0]);
        }
    }

    private void readerLoop() {
        try {
            while (connected) {
                int first = readRequired(input);
                int second = readRequired(input);
                boolean fin = (first & 0x80) != 0;
                int opcode = first & 0x0F;
                boolean masked = (second & 0x80) != 0;
                long length = second & 0x7F;

                if (length == 126) {
                    length = ((long) readRequired(input) << 8) | readRequired(input);
                } else if (length == 127) {
                    length = 0;
                    for (int i = 0; i < 8; i++) length = (length << 8) | readRequired(input);
                }

                if (!fin || length > 2_000_000L) throw new IOException("Nepodporovaný WebSocket rámec");

                byte[] mask = masked ? readExact(input, 4) : null;
                byte[] payload = readExact(input, (int) length);
                if (mask != null) {
                    for (int i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
                }

                if (opcode == 0x1) {
                    handleText(new String(payload, StandardCharsets.UTF_8));
                } else if (opcode == 0x8) {
                    sendFrame(0x8, payload, 0, payload.length);
                    throw new EOFException("Rádio ukončilo spojení");
                } else if (opcode == 0x9) {
                    sendFrame(0xA, payload, 0, payload.length);
                }
            }
        } catch (Exception e) {
            if (connected) closeInternal("Odpojeno: " + safeMessage(e), true);
        }
    }

    private void handleText(String text) {
        try {
            JSONObject message = new JSONObject(text);
            String type = message.optString("type", "");
            JSONObject payload = message.optJSONObject("payload");
            if (payload == null) payload = new JSONObject();

            if ("client/hello".equals(type)) {
                radioName = payload.optString("name", "Rádio");
                sendServerHello();
                protocolReady = true;
                notifyStatus("Připojeno: " + radioName);
                if (listener != null) listener.onReady(radioName);
                return;
            }

            if ("client/time".equals(type)) {
                long clientTransmitted = payload.optLong("client_transmitted", 0L);
                long received = nowUs();
                JSONObject replyPayload = new JSONObject();
                replyPayload.put("client_transmitted", clientTransmitted);
                replyPayload.put("server_received", received);
                replyPayload.put("server_transmitted", nowUs());
                sendJson("server/time", replyPayload);
                return;
            }

            if ("client/state".equals(type)) {
                JSONObject player = payload.optJSONObject("player");
                if (player != null && player.has("volume") && listener != null) {
                    listener.onRadioVolume(player.optInt("volume", 50));
                }
                return;
            }

            if ("stream/request-format".equals(type)) notifyStatus("Rádio požádalo o jiný formát zvuku");
        } catch (Exception ignored) {
            notifyStatus("Neznámá zpráva rádia");
        }
    }

    private void sendServerHello() throws Exception {
        JSONObject payload = new JSONObject();
        payload.put("server_id", serverId);
        payload.put("name", "ORIScore radio");
        payload.put("version", 1);
        sendJson("server/hello", payload);
    }

    public void sendStreamStart(int sampleRate) throws Exception {
        JSONObject player = new JSONObject();
        player.put("codec", "pcm");
        player.put("channels", 2);
        player.put("sample_rate", sampleRate);
        player.put("bit_depth", 16);
        JSONObject payload = new JSONObject();
        payload.put("player", player);
        sendJson("stream/start", payload);
        sendClear();
    }

    public void sendClear() throws Exception {
        JSONObject payload = new JSONObject();
        payload.put("player", new JSONObject());
        sendJson("stream/clear", payload);
    }

    public void sendStreamEnd() {
        try {
            JSONObject payload = new JSONObject();
            payload.put("player", new JSONObject());
            sendJson("stream/end", payload);
        } catch (Exception ignored) {
        }
    }

    public void sendGroupPlaying(boolean playing) {
        try {
            JSONObject payload = new JSONObject();
            payload.put("playback_state", playing ? "playing" : "stopped");
            sendJson("group/update", payload);
        } catch (Exception ignored) {
        }
    }

    public void sendMetadata(String title, long durationMs, long progressMs, boolean playing) {
        try {
            JSONObject progress = new JSONObject();
            progress.put("track_progress", Math.max(0, progressMs));
            progress.put("track_duration", Math.max(0, durationMs));
            progress.put("playback_speed", playing ? 1000 : 0);

            JSONObject metadata = new JSONObject();
            metadata.put("title", title == null ? "" : title);
            metadata.put("artist", JSONObject.NULL);
            metadata.put("album", JSONObject.NULL);
            metadata.put("artwork_url", JSONObject.NULL);
            metadata.put("timestamp", nowUs());
            metadata.put("progress", progress);

            JSONArray commands = new JSONArray();
            commands.put("play");
            commands.put("pause");
            commands.put("stop");
            commands.put("next");
            commands.put("previous");
            commands.put("volume");
            commands.put("mute");
            JSONObject controller = new JSONObject();
            controller.put("supported_commands", commands);

            JSONObject payload = new JSONObject();
            payload.put("metadata", metadata);
            payload.put("controller", controller);
            sendJson("server/state", payload);
        } catch (Exception ignored) {
        }
    }

    public void sendVolume(int volume) {
        try {
            JSONObject player = new JSONObject();
            player.put("command", "volume");
            player.put("volume", Math.max(0, Math.min(100, volume)));
            JSONObject payload = new JSONObject();
            payload.put("player", player);
            sendJson("server/command", payload);
        } catch (Exception ignored) {
        }
    }

    public void sendAudio(long timestampUs, byte[] pcm, int offset, int length) throws IOException {
        if (!protocolReady) throw new IOException("Sendspin není připraven");
        byte[] payload = new byte[9 + length];
        payload[0] = 4;
        for (int i = 0; i < 8; i++) payload[1 + i] = (byte) ((timestampUs >>> (56 - i * 8)) & 0xFF);
        System.arraycopy(pcm, offset, payload, 9, length);
        sendFrame(0x2, payload, 0, payload.length);
    }

    private void sendJson(String type, JSONObject payload) throws Exception {
        JSONObject message = new JSONObject();
        message.put("type", type);
        message.put("payload", payload);
        sendText(message.toString());
    }

    private void sendText(String text) throws IOException {
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        sendFrame(0x1, bytes, 0, bytes.length);
    }

    private void sendFrame(int opcode, byte[] payload, int offset, int length) throws IOException {
        OutputStream out = output;
        if (!connected || out == null) throw new IOException("Rádio není připojeno");

        synchronized (writeLock) {
            ByteArrayOutputStream frame = new ByteArrayOutputStream(length + 16);
            frame.write(0x80 | (opcode & 0x0F));
            if (length < 126) {
                frame.write(0x80 | length);
            } else if (length <= 0xFFFF) {
                frame.write(0x80 | 126);
                frame.write((length >>> 8) & 0xFF);
                frame.write(length & 0xFF);
            } else {
                frame.write(0x80 | 127);
                long longLength = length & 0xFFFFFFFFL;
                for (int shift = 56; shift >= 0; shift -= 8) frame.write((int) ((longLength >>> shift) & 0xFF));
            }

            byte[] mask = new byte[4];
            RANDOM.nextBytes(mask);
            frame.write(mask, 0, mask.length);
            for (int i = 0; i < length; i++) frame.write(payload[offset + i] ^ mask[i & 3]);
            out.write(frame.toByteArray());
            out.flush();
        }
    }

    public void close() {
        closeInternal("Odpojeno", false);
    }

    private void closeInternal(String reason, boolean notify) {
        connected = false;
        protocolReady = false;
        Socket s = socket;
        socket = null;
        input = null;
        output = null;
        try {
            if (s != null) s.close();
        } catch (IOException ignored) {
        }
        if (notify && listener != null) listener.onDisconnected(reason);
        notifyStatus(reason);
    }

    private void notifyStatus(String message) {
        if (listener != null) listener.onStatus(message);
    }

    public static long nowUs() {
        return System.nanoTime() / 1000L;
    }

    private static int readRequired(InputStream in) throws IOException {
        int value = in.read();
        if (value < 0) throw new EOFException();
        return value;
    }

    private static byte[] readExact(InputStream in, int length) throws IOException {
        byte[] result = new byte[length];
        int offset = 0;
        while (offset < length) {
            int read = in.read(result, offset, length - offset);
            if (read < 0) throw new EOFException();
            offset += read;
        }
        return result;
    }

    private static String safeMessage(Exception e) {
        String message = e.getMessage();
        return message == null || message.trim().isEmpty() ? e.getClass().getSimpleName() : message;
    }
}
