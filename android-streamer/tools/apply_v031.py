from pathlib import Path
import base64
import gzip
import hashlib

root = Path(__file__).resolve().parents[1]
source_dir = root / "tools" / "v031"
java_dir = root / "app" / "src" / "main" / "java" / "cz" / "oris" / "mobileaudio"
java_dir.mkdir(parents=True, exist_ok=True)

main_parts = sorted(source_dir.glob("MainActivity.java.gz.b64.*"))
if not main_parts:
    raise SystemExit("Missing MainActivity source chunks")

main_encoded = "".join(part.read_text(encoding="utf-8").strip() for part in main_parts)
main_source = gzip.decompress(base64.b64decode(main_encoded))
main_sha = hashlib.sha256(main_source).hexdigest()
expected_main_sha = "582632f4a6326cb33d085846253ff11f1cf59f3fda62838e2657b8eb3197e5c0"
if main_sha != expected_main_sha:
    raise SystemExit(f"MainActivity checksum mismatch: {main_sha}")
(java_dir / "MainActivity.java").write_bytes(main_source)

api_encoded = (source_dir / "OrisApi.java.gz.b64").read_text(encoding="utf-8").strip()
api_source = gzip.decompress(base64.b64decode(api_encoded))
api_sha = hashlib.sha256(api_source).hexdigest()
expected_api_sha = "9b25ab23bf49b3f785b09bd04e3f24183c1d52e0a5952aa5e2671db88eea5292"
if api_sha != expected_api_sha:
    raise SystemExit(f"OrisApi checksum mismatch: {api_sha}")
(java_dir / "OrisApi.java").write_bytes(api_source)

print(f"Applied ORIS Mobile Audio 0.3.1 sources: MainActivity={main_sha}, OrisApi={api_sha}")
