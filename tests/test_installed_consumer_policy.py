"""Installed-consumer staging cannot replace another build or manifest."""
import importlib.util
from pathlib import Path
import sys
import unittest
import ii_file_provider as provider

project = Path(__file__).resolve().parents[1]
owner = Path(sys.argv[1]).resolve()
spec = importlib.util.spec_from_file_location("calendar_consumer", project / "tests/installed_consumer.py")
consumer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(consumer)

class ConsumerIsolationTest(unittest.TestCase):
    def test_manifest_redirect_is_exact_and_non_destructive(self):
        build = owner / "build"
        provider.mkdir(build, exist_ok=False)
        source = (build.as_posix() + "/install_local_manifest.txt\n" +
                  build.as_posix() + "/${CMAKE_INSTALL_MANIFEST}\n")
        redirected = consumer.redirect_manifest(source, build, owner)
        self.assertIn(owner.as_posix() + "/install_local_manifest.txt", redirected)
        self.assertIn(owner.as_posix() + "/${CMAKE_INSTALL_MANIFEST}", redirected)
        self.assertNotIn(build.as_posix() + "/install_local_manifest.txt", redirected)
        with self.assertRaises(ValueError):
            consumer.redirect_manifest(source + source, build, owner)
        with self.assertRaises(ValueError):
            consumer.redirect_manifest("missing marker", build, owner)

    def test_existing_consumer_directory_is_preserved(self):
        build = owner / "other-build"
        provider.mkdir(build, exist_ok=False)
        path = consumer.reserve_consumer(build, "ab12")
        provider.write_text(path / "sentinel", "preserve")
        with self.assertRaises(FileExistsError):
            consumer.reserve_consumer(build, "ab12")
        self.assertEqual((path / "sentinel").read_text(), "preserve")
        for token in ("../x", "nope", "12345"):
            with self.assertRaises(ValueError):
                consumer.reserve_consumer(build, token)

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
