# iisacc calendar account consumer

Configure this independent C++23 consumer after building and installing the
iiCalendar, iiAccountManager and iiFileProvider Native packages. It tests the header-only session
adapter with a local fixture server and the synthetic account snapshot in
`resources/iisacc-account-fixture.json`; it does not use a live account.
The fixture is a synthetic snapshot of iiAccountManager's account test data;
update it when the account contract changes and keep the bridge test in sync.

Build the consumer under iiCalendar's `build/` directory, then run the
`iiCalendar.iisacc_bridge` CTest. The SDK library itself does not discover or
link iiAccountManager. The calendar library still uses iiFileProvider.
