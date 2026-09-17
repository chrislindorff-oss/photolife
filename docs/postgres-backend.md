# Shared Postgres backend

By default, PhotoLife's catalogue is a local SQLite file: fast, zero-setup,
but usable by one person on one machine. As an alternative, a catalogue can
live in a shared Postgres database instead, so several collaborators — each
running their own local install of PhotoLife — can manage one catalogue
together. Every client connects directly to the same Postgres database; there
is no PhotoLife server component.

This is a v1 feature with a deliberately simple concurrency model:
**last write wins**. If two collaborators confirm a match, or star/unstar a
Best Shot, for the same capture at the same moment, whichever write commits
last is what sticks — there's no conflict warning or merge UI. For a small
group of collaborators this is rarely an issue in practice, but it's worth
knowing before you rely on it.

## Provisioning a database

Any reachable Postgres 13+ server works. The walkthrough below uses
[Google Cloud SQL for PostgreSQL](https://cloud.google.com/sql/docs/postgres),
since that's a natural fit for a small group of collaborators who don't want
to run their own server.

1. **Create the instance** (Cloud Console, or `gcloud`):

   ```
   gcloud sql instances create photolife-catalogue \
     --database-version=POSTGRES_16 \
     --tier=db-f1-micro \
     --region=us-central1
   ```

   A `db-f1-micro` tier is plenty for a catalogue's size (thousands of rows,
   not millions) — scale up only if a full library scan or match run over a
   shared connection feels slow.

2. **Create the database and a role per collaborator** (or one shared role,
   if you'd rather not manage several):

   ```
   gcloud sql databases create photolife --instance=photolife-catalogue
   gcloud sql users create helena --instance=photolife-catalogue --password=...
   ```

   PhotoLife doesn't currently attribute writes to individual users in the
   UI, so separate roles buy you audit trail (via Postgres's own logs) and
   independent revocation, not a visibly different in-app experience.

3. **Decide how clients will reach it.** Two supported approaches:

   - **Authorized networks + `sslmode=require`** — simplest to set up if
     every collaborator has a static IP or a small, stable IP range (add
     each with `gcloud sql instances patch photolife-catalogue
     --authorized-networks=...`). PhotoLife's Settings dialog connects
     straight to the instance's public IP.
   - **Cloud SQL Auth Proxy** — each collaborator runs the
     [proxy](https://cloud.google.com/sql/docs/postgres/sql-proxy) locally
     (`cloud-sql-proxy photolife-catalogue`), which listens on
     `127.0.0.1:5432` and handles IAM-based auth and TLS without managing
     certificates by hand. PhotoLife then just points at `localhost` — from
     its point of view this is still "connect directly to Postgres," the
     proxy is transparent.

   Either way, nothing in PhotoLife itself needs to change — the Settings
   dialog only cares about a host, port, database name, user and password.

4. **Schema migrations run automatically.** The first PhotoLife instance to
   open the empty database applies all pending migrations (the same ones
   that would run against a local SQLite file); nothing needs to be
   pre-seeded.

## Connecting from PhotoLife

**File → Catalogue Settings…** opens a dialog with a **Local (SQLite)** /
**Shared (Postgres)** picker. Choosing Shared (Postgres) shows:

| Field | Value |
|---|---|
| Host | the instance's IP, or `localhost` if using the Auth Proxy |
| Port | `5432` (Postgres default) |
| Database | the database name from step 2 above |
| User / Password | the role's credentials |
| SSL mode | `require` for a direct authorized-networks connection; `disable` is fine behind the Auth Proxy (it already handles TLS) |

**Test Connection** opens a real (throwaway) connection with the entered
details and reports success or the underlying error — useful for catching a
typo'd host or a firewall rule that hasn't propagated yet, before saving.

Saving takes effect **the next time PhotoLife starts** — the dialog doesn't
hot-swap the currently-open catalogue. Every collaborator repeats this setup
once, pointing at the same host/database.

The password is stored the same way PhotoLife already stores an iNaturalist
API token: in the OS's native settings store (`QSettings`, e.g. the Windows
registry, macOS defaults, or a config file under `~/.config` on Linux) —
**not encrypted**. Treat it like any other locally-stored credential.

## Runtime dependency: `libpq`

Qt's Postgres driver (`QPSQL`) is a separate plugin from SQLite's, and it
links against `libpq`, the Postgres client library — neither ships by
default everywhere PhotoLife runs:

- **Linux (system Qt / this repo's own dev setup)**: install
  `libqt6sql6-psql` alongside the existing `libqt6sql6-sqlite` (e.g.
  `sudo apt install libqt6sql6-psql`). `.github/workflows/ci.yml` already
  does this for the CI job that runs the Postgres-backed test suite.
- **Linux (AppImage)**: `packaging/linux/build-appimage.sh` explicitly keeps
  `libqsqlpsql.so` when it prunes `linuxdeploy`'s auto-bundled SQL drivers
  (originally pruned down to just SQLite, to avoid a proprietary Mimer SQL
  dependency neither driver needs) — `libpq.so.5` itself is picked up
  automatically by `linuxdeploy`'s normal dependency scan.
- **Windows (installer)**: `.github/workflows/release.yml` installs
  `libpq:x64-windows` via vcpkg alongside exiv2/libraw specifically so its
  DLL is available; `windeployqt --sql` bundles Qt's `QPSQL` plugin
  automatically, but not the plugin's own `libpq.dll`/OpenSSL dependencies,
  since those aren't Qt binaries — the existing "copy every vcpkg DLL into
  the staged install" step picks them up once vcpkg has built them.

If you're building PhotoLife yourself outside of CI and want the Shared
(Postgres) option to work, make sure your Qt installation includes the
`QPSQL` driver (most distro Qt packages split it out the same way Ubuntu
does above) before reconfiguring.

## Testing against a local Postgres

For development, `docker-compose.yml` at the repo root starts a disposable
Postgres 16 container:

```
docker compose up -d
export PHOTOLIFE_TEST_PG_DSN="host=localhost port=5432 dbname=photolife_test user=photolife_test password=photolife_test sslmode=disable"
ctest --test-dir build --output-on-failure
```

Test cases that need a live Postgres server (in `tst_database.cpp` and a
couple of other suites) read `PHOTOLIFE_TEST_PG_DSN` and skip themselves
when it isn't set, so the rest of the suite runs the same with or without
Postgres available.
