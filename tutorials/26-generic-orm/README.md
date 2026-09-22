# Tutorial 26: Generic ORM Query Builder

Construct type-safe database queries in C11 using macro-based generic dispatch (`_Generic`).

## Key Concepts
- Opening an ORM handle with `cwist_orm_open_socket(fd)` and closing it with `cwist_orm_close_socket(orm)`.
- Inserting and updating rows with `cwist_orm_insert(orm, table, data)` and `cwist_orm_update(orm, table, data, where)`.
- Querying rows into a `cJSON` result with `cwist_orm_query(orm, sql, &result)`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut26
```
