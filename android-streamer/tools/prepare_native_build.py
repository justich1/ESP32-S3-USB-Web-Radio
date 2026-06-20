from pathlib import Path

root = Path(__file__).resolve().parents[1]
java = root / "app" / "src" / "main" / "java" / "cz" / "oris" / "mobileaudio"

audio = java / "AudioStreamer.java"
text = audio.read_text(encoding="utf-8")
text = text.replace(
    "    private static final long START_BUFFER_US = 700_000L;\n"
    "    private static final long SEND_AHEAD_US = 500_000L;\n"
    "    private static final int CHUNK_MS = 20;\n",
    "    private static final int TARGET_SAMPLE_RATE = 48_000;\n"
    "    private static final long START_BUFFER_US = 800_000L;\n"
    "    private static final long SEND_AHEAD_US = 600_000L;\n"
    "    private static final int CHUNK_MS = 20;\n",
)
text = text.replace(
    "            int targetRate = (sourceRate == 44_100 || sourceRate == 48_000) ? sourceRate : 48_000;\n",
    "            final int targetRate = TARGET_SAMPLE_RATE;\n",
)
text = text.replace(
    "                    targetRate = (sourceRate == 44_100 || sourceRate == 48_000) ? sourceRate : 48_000;\n"
    "                    continue;\n",
    "                    continue;\n",
)
audio.write_text(text, encoding="utf-8")

sendspin = java / "SendspinConnection.java"
text = sendspin.read_text(encoding="utf-8")
text = text.replace("User-Agent: ORIS-Mobile-Audio/0.1", "User-Agent: ORIS-Mobile-Audio/0.3")
text = text.replace(
    'closeInternal("Připojení selhalo: " + safeMessage(e), false);',
    'closeInternal("Připojení selhalo: " + safeMessage(e), true);',
)
sendspin.write_text(text, encoding="utf-8")

print("Prepared 48 kHz audio and Sendspin 0.3 client")
