# RetroSave Community

Read `CLAUDE.md` and `ARCHITECTURE.md` before changing synchronization,
storage, deployment or client behavior. The desktop target is Qt/QML with a
C++ agent; Android is Kotlin/Compose; the server is FastAPI/PostgreSQL/S3.

Keep useful French comments that explain Qt object lifetime, asynchronous
control flow and safety boundaries. Do not claim Windows, Android hardware or
GUI integration was verified solely from Linux or headless tests.

The following rules are mandatory:

- never read, hash, archive or transfer ROM/BIOS files;
- never replace local save data without a backup and an atomic write;
- preserve both sides of every conflict;
- add or update a shared vector before changing sync-core behavior;
- keep the C++ and Kotlin engines compatible with the shared vectors;
- document user-visible limitations in `docs/IMPLEMENTATION_STATUS.md`.
