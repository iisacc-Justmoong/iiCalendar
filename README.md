# iiCalendar

[Windows build and execution](docs/WindowsBuildAndRun.md)

The installed consumer test uses the same build configuration as the SDK. This
keeps the MSVC runtime and iterator ABI consistent for Debug and Release builds;
consumer build failures include compiler and linker output in the test log.

Package release: **0.2.1** (2026-10-19). This patch packages the current
source snapshot; build, test and installation evidence is recorded in the SDK
patch-release report. Earlier versioned feature notes remain historical.


C++23-based Qt non-dependent calendar SDK. It provides high-precision time, international calendar and timezone, schedule, to-do, activity, SQLite index, and data models for screen display. It does not reference the UI framework.

Public headers and implementations are grouped by responsibility under `src/`. Usage examples are `examples/day_details.cpp`. Consumers continue to use `<iiCalendar/...>` through the exported CMake target.

```sh
python3 scripts/bootstrap_icu.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 6
ctest --test-dir build --output-on-failure
./build/iiCalendarExample
cmake --install build --prefix "$PWD/build/install"
```

If ICU already exists, specify `-DICU_ROOT=/path/to/icu` without bootstrap. The required dependencies are SQLite 3.37+, ICU 69+, and C++23 compiler. Bootstrap validates official ICU 78.3 sources from POSIX host to SHA-256 and statically builds only at `build/deps/icu/`. Network download is performed only during explicit bootstrap execution. CMake is not downloaded.

```cmake
find_package(iiCalendar 0.2 CONFIG REQUIRED)
target_link_libraries(MyApplication PRIVATE iiCalendar::iiCalendar)
```

Installation consumers require SQLite and ICU development package or `ICU_ROOT`. The top-level SDK installer is auto-discovered via CMakeLists.txt. To update the actual user installation on the host, use the SDK installer's `--only iiCalendar` and the desired prefix.

The specific supported scope, time scales, recurrence rules, and storage contracts are recorded in [design documentation](docs/Design.md); the scope of tests and platform verification is recorded in [verification documentation](docs/Validation.md).

`Store` can also use an already-open `std::shared_ptr<sqlite3>` supplied by the
application. The application supplies a deleter that closes the connection and
must give the store exclusive use of that connection for its lifetime. This
permits application-owned connection setup; `Store::inMemory()` opens an
in-memory database without creating a file. For persistent databases and
account documents, the separately configured [file-provider consumer](examples/file-provider)
shows application-owned composition with iiFileProvider. The former `Store(path)`
and account `saveConnections(path)` / `loadConnections(path)` members have been
removed: applications must migrate to the consumer adapter or supply their own
connection and document I/O. Rebuild existing consumers against the new API.

## Accounts and external calendars

`CalendarAccountManager` requires an authenticated iisacc owner and connects
additional Google, Microsoft/Outlook and device-managed Apple calendar accounts.
It manages account-scoped calendars/events, enable/disable, disconnect,
credential-free connection restoration, expiry and stale-session isolation.
The native library remains Qt-free. A header-only adapter reads verified
iiAccountManager 0.2.9+ session bindings and their effective credential expiry.
See [Accounts](docs/Accounts.md) for the public API and complete host wiring.
The account-SDK integration test is configured separately from
`examples/iisacc-bridge`; iiCalendar's own CMake does not find or link that SDK.
The calendar library itself does not link iiFileProvider Native.

The host owns OAuth, token refresh and secure credential custody. The SDK
resolves/pins Google and Microsoft account identity, performs calendar reads and
validates bounded snapshots. Apple uses the OS EventKit accounts and requires
explicit full-access authorization. Default HTTPS uses libcurl 7.85+; with
`-DIICALENDAR_WITH_CURL=OFF`, inject `CalendarHttpGet`.

`CalendarObjectManager` remains the lower-level provider-object primitive used
by the account facade. Its calendar/event formats, refresh rules and platform
authorization contract are in [External calendars](docs/ExternalCalendars.md).

## License

SPDX-License-Identifier: MIT

The original code and documentation in this repository are licensed under the
[MIT License](LICENSE). Third-party code, libraries, fonts, tools, and model
weights retain their respective licenses.

## Source header layout

Public headers and implementations live together under `src/`, grouped by chronology, events, storage, presentation, integrations and security. See [source layout](docs/SourceLayout.md).
There is no separate authored `include/` tree. Existing public `#include` names
and exported CMake targets are unchanged; installation still places only public
headers in the package include directory. Generated export headers belong to
`build/`, not to the authored source tree.

Validate this layout from the SDK workspace with
`python3 -B tests/test_source_header_layout.py`. Reconfigure, build, run CTest,
and verify a staged installed consumer after changing source/header paths.

<!-- workspace-docs:readme:start -->
## Documentation entry points

- Human overview: this English `README.md`.
- Agent instructions: [AGENT.md](AGENT.md).
- Human documentation: [documentation index](docs/README.md).
- Structured agent documentation: [agent index](docs/agent-index.json).
<!-- workspace-docs:readme:end -->
