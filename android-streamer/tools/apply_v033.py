from pathlib import Path
import base64
import gzip

root = Path(__file__).resolve().parents[1]
payload = (root / "tools/v033/apply_v033.py.gz.b64").read_text(encoding="utf-8").strip()
source = gzip.decompress(base64.b64decode(payload)).decode("utf-8")
runtime = root / "tools/apply_v033_runtime.py"
namespace = {"__name__": "__main__", "__file__": str(runtime)}
exec(compile(source, str(runtime), "exec"), namespace)
