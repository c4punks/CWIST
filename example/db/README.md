# db examples

Four steps that build on each other, each showing one concept from the
CWIST database API. All four are console programs — they print to stdout
and exit, no server is started.

| Step | Shows |
|------|-------|
| [`step-1-open-query`](step-1-open-query/main.c) | open a SQLite database behind a Unix socket with `cwist_db_transfer_sqlite_to_socket` + `cwist_orm_open_socket`, then `cwist_orm_exec`, `cwist_orm_insert`, and `cwist_orm_select` |
| [`step-2-migrations`](step-2-migrations/main.c) | versioned schema migrations with `cwist_migrate_up`, `cwist_migrate_down`, and `cwist_migrate_version` |
| [`step-3-zod-validate`](step-3-zod-validate/main.c) | validate JSON against a schema with `cwist_zod_parse` / `cwist_zod_validate`, and repair aliased field names with `cwist_json_heal` |
| [`step-4-json-insert`](step-4-json-insert/main.c) | self-healing insert: heal broken JSON with `cwist_json_heal`, then insert via `cwist_orm_insert` |

## Build

Build the library first, from the repository root:

```sh
make
```

Then build each step (steps 1, 2, and 4 have a rule in the root Makefile,
step 3 ships its own Makefile):

```sh
make example/db/step-1-open-query/open-query
make example/db/step-2-migrations/migrations
make -C example/db/step-3-zod-validate
make example/db/step-4-json-insert/json-insert
```

`make examples-check` builds all four steps.

## Run

Each step is self-contained and uses an in-memory SQLite database
(`:memory:`), so nothing persists between runs.

### Step 1: open & query

```sh
./example/db/step-1-open-query/open-query
```

Creates a `users` table, inserts three rows as cJSON objects with
`cwist_orm_insert`, and selects them back:

```
=== ORM: Open, Insert & Select ===
Table 'users' created
3 rows inserted via ORM

[SELECT]
  id=1    name=Alice     age=30
  id=2    name=Bob       age=25
  id=3    name=Carol     age=35

=== Done ===
```

### Step 2: migrations

```sh
./example/db/step-2-migrations/migrations
```

Defines three `cwist_migration_t` entries — `create_users` (v1),
`add_email_to_users` (v2), `create_posts` (v3) — then applies them all
(`cwist_migrate_up`), rolls back one step (`cwist_migrate_down` with
count 1), and re-applies:

```
[Migrate UP -- apply all]
Current version: 3

[Migrate DOWN -- roll back 1 step]
Current version after rollback: 2

[Migrate UP -- re-apply]
Current version: 3
```

### Step 3: zod-style validation

```sh
./example/db/step-3-zod-validate/step-3-zod-validate
```

Declares a `product` schema with `cwist_schema_field_t` (required fields,
types, and alias names: `cost`/`amount` map to `price`). It validates
four inputs and prints one error per failed field, then shows
`cwist_json_heal` renaming the `cost` alias to `price` so the healed
document passes validation.

### Step 4: self-healing JSON insert

```sh
./example/db/step-4-json-insert/json-insert
```

Inserts four events into an `events` table. Every input passes through
`cwist_json_heal` first, so a trailing comma is repaired at level L1 and
aliased fields (`cat` → `category`, `points` → `score`) are renamed at
level L2. Input that cannot be healed (`not json at all!!!`) fails the
insert. The final `SELECT` shows the three rows that made it:

```
  1   | Launch Party         | social     | 95
  2   | Team Standup         | work       | 80
  3   | Hackathon            | tech       | 90
```

## Clean

```sh
make -C example/db/step-3-zod-validate clean
rm -f example/db/step-1-open-query/open-query \
      example/db/step-2-migrations/migrations \
      example/db/step-4-json-insert/json-insert
```
