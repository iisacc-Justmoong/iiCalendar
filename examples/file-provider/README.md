# Calendar file-provider composition

This independently configured consumer links iiCalendar and iiFileProvider Native.
`App/FileProviderAdapter.h` opens file databases through the provider and gives
an owned SQLite connection to `Store`; it saves and restores account documents
through the provider's bounded, atomic file API. The application owns this
composition. Use a private path for each signed-in owner and serialize writes.
Configure this directory separately against installed packages. Its synthetic
contract tests create temporary files under `../../resources` and remove them.

```sh
cmake -S examples/file-provider -B build/file-provider-consumer \
  '-DCMAKE_PREFIX_PATH=/path/to/iiCalendar;/path/to/iiFileProvider' \
  -DICU_ROOT=/path/to/icu
cmake --build build/file-provider-consumer
ctest --test-dir build/file-provider-consumer --output-on-failure
```
