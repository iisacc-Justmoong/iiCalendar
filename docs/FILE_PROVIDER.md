# File creation ownership in iiCalendar

Calendar index databases and connection documents are opened by the separate
`examples/file-provider` application adapter using the native provider. Calendar
schema, queries, transactions, recurrence and calendar semantics stay in
iiCalendar, which has no provider SDK dependency.

The common [ownership contract](../../iiFileProvider/docs/FILE_OWNERSHIP.md) defines stream, staging, control-file and
installed-package rules. Source regression tests remain with this module; the
workspace creation audit also checks these boundaries. Build/deployment and target
runtime validation are separate evidence.

The application passes an already-open SQLite connection to `Store` and keeps
exclusive use of it for the store lifetime. Account documents use the calendar
module's metadata-only document API and provider-owned file publication.
Existing applications using `Store(path)` or the removed account file methods
must migrate to this adapter and rebuild.
