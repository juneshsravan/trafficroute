# Migrations

Schema version 1 is `../schema.sql` (created automatically by both the server
and the importer with `CREATE TABLE IF NOT EXISTS`). Future schema changes go
here as numbered SQL files (`002_add_x.sql`, ...) applied in order, and the
same change is made in `backend/src/database.c`.

PostgreSQL/PostGIS path: the tables use portable SQL types. Replace the FTS5
virtual table with a `tsvector` column + GIN index, store coordinates as
`geography(Point,4326)`, and re-implement `database.c` with libpq.
