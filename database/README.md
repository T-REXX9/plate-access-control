# Gate access database

`gate_access.db` is the local SQLite database for the plate-recognition gate.
It contains registered vehicles, entry/exit events, website users, audit logs,
system status, and settings.

The database starts empty. Plate records must be uppercase letters and digits
without spaces or punctuation, matching the C++ OCR output.

To create or safely re-apply the schema:

```bash
./database/init_database.sh
```

The script does not delete existing records. Event photographs should remain
as image files; store their relative paths in `access_events.image_path`.
Passwords must be added by the future website as secure hashes, never as plain
text.

The admin website creates the first password securely at `/setup`. The live
C++ camera service writes recognition events to this database after two
matching OCR readings and checks `vehicles.is_active` before marking an event
as authorized.
