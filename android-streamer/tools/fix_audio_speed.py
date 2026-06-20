from pathlib import Path

path = Path(__file__).resolve().parents[1] / "app/src/main/java/cz/oris/mobileaudio/AudioStreamer.java"
text = path.read_text(encoding="utf-8")

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

path.write_text(text, encoding="utf-8")
print(f"Patched {path}")
