"""ICU resources stay in the project and existing prefixes are validated."""
import importlib.util
from pathlib import Path
import sys
import unittest
import ii_file_provider as provider

project = Path(__file__).resolve().parents[1]
owner = Path(sys.argv[1]).resolve()
spec = importlib.util.spec_from_file_location("calendar_bootstrap", project / "scripts/bootstrap_icu.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)

class PathsTest(unittest.TestCase):
    def test_paths_and_prefix_validation(self):
        root = owner / "project"
        provider.mkdir(root / "resources", parents=True, exist_ok=False)
        deps, build, prefix = bootstrap.resource_paths(root)
        self.assertEqual(deps, root / "resources/dependencies")
        self.assertEqual(build, root / "build/icu-bootstrap")
        self.assertEqual(prefix, deps / "icu/install")
        self.assertFalse(bootstrap.installed_prefix_ready(prefix))
        provider.mkdir(prefix / "include/unicode", parents=True, exist_ok=True)
        provider.mkdir(prefix / "lib", parents=True, exist_ok=True)
        header = prefix / "include/unicode/uvernum.h"
        provider.write_text(header, '#define U_ICU_VERSION "78.3"\n')
        for name in ("uc", "i18n", "data"):
            provider.write_bytes(prefix / "lib" / ("libicu" + name + ".a"), b"fixture")
        self.assertTrue(bootstrap.installed_prefix_ready(prefix))
        provider.write_text(header, '#define U_ICU_VERSION "77.1"\n')
        self.assertFalse(bootstrap.installed_prefix_ready(prefix))
        link = root / "resources/dependencies/icu/source"
        outside = owner / "outside"
        provider.mkdir(outside, exist_ok=False)
        try:
            link.symlink_to(outside, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"Cannot create a symbolic link on this platform: {error}")
        with self.assertRaises(ValueError):
            bootstrap.resource_paths(root)
        self.assertTrue(link.is_symlink())

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
