# File creation ownership in iiCalendar

Calendar index databases are opened through the native provider. Calendar schema, queries, transactions, recurrence and calendar semantics stay in iiCalendar.

The common [ownership contract](../../iiFileProvider/docs/FILE_OWNERSHIP.md) defines stream, staging, control-file and
installed-package rules. Source regression tests remain with this module; the
workspace creation audit also checks these boundaries. Build/deployment and target
runtime validation are separate evidence.

Changed boundaries:


- `src/iiCalendar/Store.cpp`
