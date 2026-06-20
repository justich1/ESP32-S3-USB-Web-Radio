from pathlib import Path

root = Path(__file__).resolve().parents[1]
parts = root / "tools" / "native_plain"
out = root / "app" / "src" / "main" / "java" / "cz" / "oris" / "mobileaudio"
out.mkdir(parents=True, exist_ok=True)

main_names = [
    *(f"MainActivity_{i:02d}.part" for i in range(8)),
    "MainActivity_08a.part",
    "MainActivity_08b.part",
    "MainActivity_09.part",
    "MainActivity_10.part",
]
api_names = ["OrisApi_00.part", "OrisApi_01.part"]

(out / "MainActivity.java").write_text(
    "".join((parts / name).read_text(encoding="utf-8") for name in main_names),
    encoding="utf-8",
)
(out / "OrisApi.java").write_text(
    "".join((parts / name).read_text(encoding="utf-8") for name in api_names),
    encoding="utf-8",
)

print("Assembled native MainActivity.java and OrisApi.java")
