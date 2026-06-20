package cz.oris.mobileaudio;

import android.content.Context;
import android.media.AudioFormat;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.net.Uri;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;

public final class AudioStreamer {
    public interface Listener {
        void onStatus(String message);
        void onTrackChanged(int index, String name);
        void onProgress(long positionMs, long durationMs);
        void onStopped();
    }

    private static final long START_BUFFER_US = 700_000L;
    private static final long SEND_AHEAD_US = 500_000L;
    private static final int CHUNK_MS = 20;

    private final Context context;
    private final SendspinConnection connection;
    private final Listener listener;
    private final Object pauseLock = new Object();

    private volatile Thread worker;
    private volatile int generation;
    private volatile boolean paused;
    private volatile int currentIndex = -1;
    private volatile List<Uri> playlistUris = new ArrayList<>();
    private volatile List<String> playlistNames = new ArrayList<>();

    public AudioStreamer(Context context, SendspinConnection connection, Listener listener) {
        this.context = context.getApplicationContext();
        this.connection = connection;
        this.listener = listener;
    }

    public int getCurrentIndex() {
        return currentIndex;
    }

    public boolean isPaused() {
        return paused;
    }

    public synchronized void play(List<Uri> uris, List<String> names, int startIndex) {
        if (uris == null || uris.isEmpty()) {
            notifyStatus("Nejdřív vyber skladby");
            return;
        }
        if (!connection.isProtocolReady()) {
            notifyStatus("Nejdřív připoj rádio");
            return;
        }

        stopWorker(false);
        playlistUris = new ArrayList<>(uris);
        playlistNames = new ArrayList<>(names);
        int safeIndex = Math.max(0, Math.min(startIndex, uris.size() - 1));
        int token = ++generation;
        paused = false;

        Thread thread = new Thread(() -> runPlaylist(token, safeIndex), "OrisAudioStreamer");
        worker = thread;
        thread.start();
    }

    public synchronized void stop() {
        stopWorker(true);
    }

    public void pauseOrResume() {
        if (worker == null) return;
        synchronized (pauseLock) {
            paused = !paused;
            connection.sendGroupPlaying(!paused);
            if (!paused) pauseLock.notifyAll();
        }
        notifyStatus(paused ? "Pozastaveno" : "Pokračuji");
    }

    public void next() {
        List<Uri> uris = playlistUris;
        if (uris.isEmpty()) return;
        int next = currentIndex < 0 ? 0 : (currentIndex + 1) % uris.size();
        play(uris, playlistNames, next);
    }

    public void previous() {
        List<Uri> uris = playlistUris;
        if (uris.isEmpty()) return;
        int previous = currentIndex <= 0 ? uris.size() - 1 : currentIndex - 1;
        play(uris, playlistNames, previous);
    }

    private synchronized void stopWorker(boolean sendEnd) {
        generation++;
        paused = false;
        synchronized (pauseLock) {
            pauseLock.notifyAll();
        }
        Thread old = worker;
        worker = null;
        if (old != null && old != Thread.currentThread()) {
            old.interrupt();
            try {
                old.join(700);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
        if (sendEnd) {
            connection.sendGroupPlaying(false);
            connection.sendStreamEnd();
            currentIndex = -1;
            if (listener != null) listener.onStopped();
        }
    }

    private void runPlaylist(int token, int startIndex) {
        int index = startIndex;
        try {
            while (isActive(token) && index < playlistUris.size()) {
                currentIndex = index;
                String name = nameAt(index);
                if (listener != null) listener.onTrackChanged(index, name);
                notifyStatus("Přehrávám: " + name);
                boolean completed = decodeAndStream(token, playlistUris.get(index), name);
                if (!completed || !isActive(token)) break;
                index++;
            }
        } finally {
            if (isActive(token)) {
                connection.sendGroupPlaying(false);
                connection.sendStreamEnd();
                currentIndex = -1;
                worker = null;
                if (listener != null) listener.onStopped();
                notifyStatus("Playlist skončil");
            }
        }
    }

    private boolean decodeAndStream(int token, Uri uri, String title) {
        MediaExtractor extractor = new MediaExtractor();
        MediaCodec decoder = null;
        boolean streamStarted = false;
        try {
            extractor.setDataSource(context, uri, null);
            int trackIndex = findAudioTrack(extractor);
            if (trackIndex < 0) throw new IOException("Soubor neobsahuje zvuk");
            extractor.selectTrack(trackIndex);

            MediaFormat inputFormat = extractor.getTrackFormat(trackIndex);
            String mime = inputFormat.getString(MediaFormat.KEY_MIME);
            if (mime == null) throw new IOException("Neznámý zvukový formát");

            long durationUs = inputFormat.containsKey(MediaFormat.KEY_DURATION)
                    ? inputFormat.getLong(MediaFormat.KEY_DURATION) : 0L;
            int sourceRate = inputFormat.containsKey(MediaFormat.KEY_SAMPLE_RATE)
                    ? inputFormat.getInteger(MediaFormat.KEY_SAMPLE_RATE) : 48_000;
            int sourceChannels = inputFormat.containsKey(MediaFormat.KEY_CHANNEL_COUNT)
                    ? inputFormat.getInteger(MediaFormat.KEY_CHANNEL_COUNT) : 2;
            int pcmEncoding = AudioFormat.ENCODING_PCM_16BIT;

            try {
                inputFormat.setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_16BIT);
            } catch (Exception ignored) {
            }

            decoder = MediaCodec.createDecoderByType(mime);
            decoder.configure(inputFormat, null, null, 0);
            decoder.start();

            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean inputEnded = false;
            boolean outputEnded = false;
            int targetRate = (sourceRate == 44_100 || sourceRate == 48_000) ? sourceRate : 48_000;
            long nextTimestampUs = 0L;
            long framesSent = 0L;
            long lastProgressUpdateMs = 0L;

            while (isActive(token) && !outputEnded) {
                if (!inputEnded) {
                    int inputIndex = decoder.dequeueInputBuffer(10_000);
                    if (inputIndex >= 0) {
                        ByteBuffer inputBuffer = decoder.getInputBuffer(inputIndex);
                        if (inputBuffer == null) throw new IOException("Chybí vstupní buffer dekodéru");
                        inputBuffer.clear();
                        int sampleSize = extractor.readSampleData(inputBuffer, 0);
                        if (sampleSize < 0) {
                            decoder.queueInputBuffer(inputIndex, 0, 0, 0,
                                    MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputEnded = true;
                        } else {
                            decoder.queueInputBuffer(inputIndex, 0, sampleSize,
                                    extractor.getSampleTime(), extractor.getSampleFlags());
                            extractor.advance();
                        }
                    }
                }

                int outputIndex = decoder.dequeueOutputBuffer(info, 10_000);
                if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat outputFormat = decoder.getOutputFormat();
                    if (outputFormat.containsKey(MediaFormat.KEY_SAMPLE_RATE)) {
                        sourceRate = outputFormat.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                    }
                    if (outputFormat.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) {
                        sourceChannels = outputFormat.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                    }
                    if (outputFormat.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
                        pcmEncoding = outputFormat.getInteger(MediaFormat.KEY_PCM_ENCODING);
                    }
                    targetRate = (sourceRate == 44_100 || sourceRate == 48_000) ? sourceRate : 48_000;
                    continue;
                }

                if (outputIndex < 0) continue;

                ByteBuffer outputBuffer = decoder.getOutputBuffer(outputIndex);
                if (outputBuffer != null && info.size > 0
                        && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                    outputBuffer.position(info.offset);
                    outputBuffer.limit(info.offset + info.size);
                    byte[] decoded = new byte[info.size];
                    outputBuffer.get(decoded);

                    byte[] stereoPcm = convertToStereoPcm16(
                            decoded, sourceChannels, sourceRate, targetRate, pcmEncoding);
                    if (stereoPcm.length > 0) {
                        if (!streamStarted) {
                            connection.sendStreamStart(targetRate);
                            connection.sendGroupPlaying(true);
                            connection.sendMetadata(title, durationUs / 1000L, 0L, true);
                            nextTimestampUs = SendspinConnection.nowUs() + START_BUFFER_US;
                            streamStarted = true;
                        }

                        int bytesPerFrame = 4;
                        int framesPerChunk = Math.max(1, targetRate * CHUNK_MS / 1000);
                        int bytesPerChunk = framesPerChunk * bytesPerFrame;
                        int offset = 0;
                        while (offset < stereoPcm.length && isActive(token)) {
                            long pauseDuration = waitWhilePaused(token);
                            nextTimestampUs += pauseDuration;
                            if (!isActive(token)) break;

                            int size = Math.min(bytesPerChunk, stereoPcm.length - offset);
                            size -= size % bytesPerFrame;
                            if (size <= 0) break;

                            paceUntil(nextTimestampUs - SEND_AHEAD_US, token);
                            if (!isActive(token)) break;
                            connection.sendAudio(nextTimestampUs, stereoPcm, offset, size);

                            int chunkFrames = size / bytesPerFrame;
                            framesSent += chunkFrames;
                            nextTimestampUs += chunkFrames * 1_000_000L / targetRate;
                            offset += size;

                            long positionMs = framesSent * 1000L / targetRate;
                            long nowMs = System.currentTimeMillis();
                            if (nowMs - lastProgressUpdateMs >= 500L) {
                                lastProgressUpdateMs = nowMs;
                                if (listener != null) listener.onProgress(positionMs, durationUs / 1000L);
                                connection.sendMetadata(title, durationUs / 1000L, positionMs, true);
                            }
                        }
                    }
                }

                outputEnded = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                decoder.releaseOutputBuffer(outputIndex, false);
            }

            if (streamStarted) {
                connection.sendGroupPlaying(false);
                connection.sendStreamEnd();
            }
            return outputEnded && isActive(token);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        } catch (Exception e) {
            notifyStatus("Chyba přehrávání: " + safeMessage(e));
            if (streamStarted) {
                connection.sendGroupPlaying(false);
                connection.sendStreamEnd();
            }
            return false;
        } finally {
            try {
                extractor.release();
            } catch (Exception ignored) {
            }
            if (decoder != null) {
                try {
                    decoder.stop();
                } catch (Exception ignored) {
                }
                try {
                    decoder.release();
                } catch (Exception ignored) {
                }
            }
        }
    }

    private long waitWhilePaused(int token) throws InterruptedException {
        long startedUs = 0L;
        synchronized (pauseLock) {
            while (paused && isActive(token)) {
                if (startedUs == 0L) startedUs = SendspinConnection.nowUs();
                pauseLock.wait(250L);
            }
        }
        return startedUs == 0L ? 0L : Math.max(0L, SendspinConnection.nowUs() - startedUs);
    }

    private void paceUntil(long targetUs, int token) throws InterruptedException {
        while (isActive(token)) {
            long remainingUs = targetUs - SendspinConnection.nowUs();
            if (remainingUs <= 0) return;
            Thread.sleep(Math.min(20L, Math.max(1L, remainingUs / 1000L)));
        }
    }

    private boolean isActive(int token) {
        return token == generation && connection.isProtocolReady() && !Thread.currentThread().isInterrupted();
    }

    private static int findAudioTrack(MediaExtractor extractor) {
        for (int i = 0; i < extractor.getTrackCount(); i++) {
            MediaFormat format = extractor.getTrackFormat(i);
            String mime = format.getString(MediaFormat.KEY_MIME);
            if (mime != null && mime.startsWith("audio/")) return i;
        }
        return -1;
    }

    private static byte[] convertToStereoPcm16(
            byte[] decoded,
            int channels,
            int sourceRate,
            int targetRate,
            int encoding
    ) {
        if (channels <= 0) channels = 2;
        short[] stereo;

        if (encoding == AudioFormat.ENCODING_PCM_FLOAT) {
            int sampleCount = decoded.length / 4;
            int frames = sampleCount / channels;
            stereo = new short[frames * 2];
            ByteBuffer buffer = ByteBuffer.wrap(decoded).order(ByteOrder.nativeOrder());
            for (int frame = 0; frame < frames; frame++) {
                float left = buffer.getFloat((frame * channels) * 4);
                float right = channels == 1 ? left : buffer.getFloat((frame * channels + 1) * 4);
                stereo[frame * 2] = floatToShort(left);
                stereo[frame * 2 + 1] = floatToShort(right);
            }
        } else if (encoding == AudioFormat.ENCODING_PCM_8BIT) {
            int frames = decoded.length / channels;
            stereo = new short[frames * 2];
            for (int frame = 0; frame < frames; frame++) {
                int leftByte = decoded[frame * channels] & 0xFF;
                int rightByte = channels == 1 ? leftByte : decoded[frame * channels + 1] & 0xFF;
                stereo[frame * 2] = (short) ((leftByte - 128) << 8);
                stereo[frame * 2 + 1] = (short) ((rightByte - 128) << 8);
            }
        } else {
            int frames = decoded.length / (2 * channels);
            stereo = new short[frames * 2];
            ByteBuffer buffer = ByteBuffer.wrap(decoded).order(ByteOrder.nativeOrder());
            for (int frame = 0; frame < frames; frame++) {
                int sampleBase = frame * channels;
                short left = buffer.getShort(sampleBase * 2);
                short right = channels == 1 ? left : buffer.getShort((sampleBase + 1) * 2);
                stereo[frame * 2] = left;
                stereo[frame * 2 + 1] = right;
            }
        }

        if (sourceRate <= 0) sourceRate = targetRate;
        if (sourceRate != targetRate && stereo.length >= 4) {
            stereo = resampleStereo(stereo, sourceRate, targetRate);
        }

        byte[] result = new byte[stereo.length * 2];
        int pos = 0;
        for (short sample : stereo) {
            result[pos++] = (byte) (sample & 0xFF);
            result[pos++] = (byte) ((sample >>> 8) & 0xFF);
        }
        return result;
    }

    private static short[] resampleStereo(short[] input, int sourceRate, int targetRate) {
        int inputFrames = input.length / 2;
        int outputFrames = Math.max(1, (int) Math.round(inputFrames * (double) targetRate / sourceRate));
        short[] output = new short[outputFrames * 2];
        double ratio = (double) sourceRate / targetRate;

        for (int outFrame = 0; outFrame < outputFrames; outFrame++) {
            double sourcePosition = outFrame * ratio;
            int first = Math.min(inputFrames - 1, (int) sourcePosition);
            int second = Math.min(inputFrames - 1, first + 1);
            double fraction = sourcePosition - first;

            for (int channel = 0; channel < 2; channel++) {
                int a = input[first * 2 + channel];
                int b = input[second * 2 + channel];
                output[outFrame * 2 + channel] = (short) Math.round(a + (b - a) * fraction);
            }
        }
        return output;
    }

    private static short floatToShort(float value) {
        float clamped = Math.max(-1.0f, Math.min(1.0f, value));
        return (short) Math.round(clamped * 32767.0f);
    }

    private String nameAt(int index) {
        List<String> names = playlistNames;
        return index >= 0 && index < names.size() ? names.get(index) : "Skladba";
    }

    private void notifyStatus(String message) {
        if (listener != null) listener.onStatus(message);
    }

    private static String safeMessage(Exception e) {
        String message = e.getMessage();
        return message == null || message.trim().isEmpty() ? e.getClass().getSimpleName() : message;
    }
}
