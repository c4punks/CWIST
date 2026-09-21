# Tutorial 27: Programmatic Database Migrations

Apply and track database schema migrations programmatically on application startup.

## Key Concepts
- Defining migrations as `cwist_migration_t` structs (version, name,
  `up_sql`, optional `down_sql`).
- Applying pending migrations with `cwist_migrate_up()`; each migration
  runs inside its own transaction, so a failure leaves the schema unchanged.
- Querying the applied schema version with `cwist_migrate_version()`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut27
```
