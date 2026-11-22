# iiCalendarIntegration

[Windows build and execution](docs/WindowsBuildAndRun.md)

The installed consumer test uses the same build configuration as the SDK. This
keeps the MSVC runtime and iterator ABI consistent for Debug and Release builds;
consumer build failures include compiler and linker output in the test log.

Package version: **0.2.1**. The 2026-10-19 patch-release report records the build,
tests and installation of the former package. The current source exports the
renamed package.


C++23-based Qt non-dependent calendar SDK. It provides high-precision time, international calendar and timezone, schedule, to-do, activity, SQLite index, and data models for screen display. It does not reference the UI framework.

Public headers and implementations are grouped by responsibility under `src/`. Include `<iiCalendarIntegration>` and use the `iiCalendarIntegration` namespace. The renamed CMake package exports `iiCalendarIntegration::iiCalendarIntegration`. Python users import `iicalendarintegration`. Rebuild consumers of the former `iiCalendar` package. Existing connection documents retain their account identities.

```sh
python3 -B scripts/bootstrap_icu.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 6
ctest --test-dir build --output-on-failure \
  -E '^iiCalendarIntegration\.(installed_consumer|external_apple)$'
cmake --install build --prefix "$HOME/.local/SDK/iiCalendarIntegration"
```

If ICU already exists, specify `-DICU_ROOT=/path/to/icu` without bootstrap. The required dependencies are SQLite 3.37+, ICU 69+, and a C++23 compiler. Bootstrap validates the official ICU 78.3 archive, retains sources and libraries in `resources/dependencies/`, and removes its temporary build after completion. Network download is performed only during explicit bootstrap execution. The example target is opt-in with `-DIICALENDARINTEGRATION_BUILD_EXAMPLES=ON`. `install.sh` uses `build/` and the normal user SDK prefix; set `IICALENDARINTEGRATION_VALIDATE_INTEGRATION=1` to run the installed-consumer and Apple authorization tests. See [BuildLayout.md](docs/BuildLayout.md).

```cmake
find_package(iiCalendarIntegration 0.2 CONFIG REQUIRED)
target_link_libraries(MyApplication PRIVATE iiCalendarIntegration::iiCalendarIntegration)
```

Installation consumers require SQLite and ICU development package or `ICU_ROOT`. The top-level SDK installer is auto-discovered via CMakeLists.txt. To update the actual user installation on the host, use the SDK installer's `--only iiCalendarIntegration` and the desired prefix.

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

`CalendarAccountManager` connects Google, Microsoft/Outlook, device-managed
Apple and custom providers in a host-owned workspace, with an optional
authenticated iisacc session for existing integrations.
It manages account-scoped snapshots, enable/disable, disconnect,
credential-free connection restoration, expiry and stale-session isolation.
Pass an initial window to a connector or call `synchronize` to publish the
calendar list and all occurrences for that window together.
The native library remains Qt-free. A header-only adapter reads verified
iiAccountManager 0.2.9+ session bindings and their effective credential expiry.
See [Accounts](docs/Accounts.md) for the public API and complete host wiring.
The account-SDK integration test is configured separately from
`examples/iisacc-bridge`; iiCalendarIntegration's own CMake does not find or link that SDK.
The calendar library itself does not link iiFileProvider Native.

The host owns OAuth, token refresh and secure credential custody. The SDK
resolves/pins Google and Microsoft account identity, performs calendar reads and
validates bounded snapshots. Apple uses the OS EventKit accounts and requires
explicit full-access authorization. Default HTTPS uses libcurl 7.85+; with
`-DIICALENDARINTEGRATION_WITH_CURL=OFF`, inject `CalendarHttpGet`.

`CalendarObjectManager` remains the lower-level provider-object primitive used
by the account facade. Its calendar/event formats, refresh rules and platform
authorization contract are in [External calendars](docs/ExternalCalendars.md).

The optional `iicalendarintegration` Python account API, build requirements and examples are documented in [Accounts.md](docs/Accounts.md#python-account-api-contract).

## License

SPDX-License-Identifier: Apache-2.0

The original code and documentation in this repository are licensed under the
[Apache License 2.0](LICENSE). Third-party code, libraries, fonts, tools, and model
weights retain their respective licenses.

## Source header layout

Public headers and implementations live together under `src/`, grouped by chronology, events, storage, presentation, integrations and security. See [source layout](docs/SourceLayout.md).
There is no separate authored `include/` tree. The module provides the
`<iiCalendarIntegration>` entry. Installation places only public
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
