"""Check install commands with fake tools; never install or access calendars."""
import os
from pathlib import Path
import re
import subprocess
import sys
import ii_file_provider as provider
project = Path(__file__).resolve().parents[1]
owner = Path(sys.argv[1]).resolve()
bin_dir = owner / "bin"
provider.mkdir(bin_dir, exist_ok=False)
for name in ("cmake", "ctest"):
    script = bin_dir / name
    provider.write_text(script, "#!/bin/sh\nprintf '" + name + ":'\nprintf ' <%s>' \"$@\"\nprintf '\\n'\n")
    script.chmod(0o700)
env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + "/usr/bin:/bin", IICALENDAR_INSTALL_PREFIX=str(owner / "install"))
env.pop("IICALENDAR_VALIDATE_INTEGRATION", None)
def run():
    p = subprocess.run(["/bin/bash", str(project / "install.sh")], env=env, capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    return p.stdout
output = run()
assert str(project / "build") in output and "<--install>" in output
assert "<-DCMAKE_INSTALL_PREFIX=" + env["IICALENDAR_INSTALL_PREFIX"] + ">" in output
line = next(line for line in output.splitlines() if line.startswith("ctest:"))
pattern = line.split(" <-E> <", 1)[1].removesuffix(">")
assert re.fullmatch(pattern, "iiCalendar.installed_consumer")
assert re.fullmatch(pattern, "iiCalendar.external_apple")
assert not re.fullmatch(pattern, "iiCalendar.external_apple_codec")
assert not re.fullmatch(pattern, "iiCalendar.storage_memory")
env["IICALENDAR_VALIDATE_INTEGRATION"] = "1"
assert "<-E>" not in run()
