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

Any reachable Postgres 13+ server works — PhotoLife only ever speaks
ordinary Postgres wire protocol to it, so nothing below is Google-specific.
The walkthrough uses [Google Cloud SQL for
PostgreSQL](https://cloud.google.com/sql/docs/postgres), but that costs money
from the moment the instance exists (there's no free tier — even the
smallest `db-f1-micro` tier bills by the hour).

### Free-tier alternatives

If cost matters more than staying inside Google Cloud, a managed Postgres
host with a genuine free tier is a drop-in swap for Cloud SQL below — sign
up, create a database, and paste its host/port/database/user/password into
PhotoLife's Settings dialog exactly the same way. No code or packaging
changes needed either way; it's all still `QPSQL` talking to Postgres.

- **[Neon](https://neon.tech)** — serverless Postgres, free tier includes
  0.5 GB storage and autosuspends when idle (a small catalogue fits
  comfortably; the DSN it gives you already includes `sslmode=require`).
- **[Supabase](https://supabase.com)** — free tier includes 500 MB database
  storage; built on plain Postgres, so the "connection string" it shows you
  maps directly onto the Settings dialog's fields.
- **[Railway](https://railway.app)** — usage-based free credit rather than
  a permanent free tier, simplest of the three to click through.

Google's Firestore and BigQuery both have real always-free tiers too, but
neither is usable here: Firestore is a NoSQL document store with no SQL
interface (PhotoLife's schema is relational — joins, foreign keys,
`WITH RECURSIVE` taxonomy-tree queries — none of which map onto documents
without rewriting every query in the codebase), and BigQuery is an
analytics warehouse tuned for scanning large batches, not the frequent
small transactional reads/writes a photo catalogue does constantly (and it
bills per byte scanned, which adds up fast under that access pattern).

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

   **Or, using Neon** (free tier — see above): sign up at
   [neon.tech](https://neon.tech), click **New Project**, give it a name
   and region, and create it. Unlike Cloud SQL, that single step already
   gives you a database and a role — there's no separate "create database"
   / "create user" step (skip straight to [Connecting from
   PhotoLife](#connecting-from-photolife) below). The project dashboard
   shows a connection string like:

   ```
   postgresql://alex:AbCdEf123456@ep-cool-lab-12345.us-east-2.aws.neon.tech/neondb?sslmode=require
   ```

   which maps onto PhotoLife's Settings fields as Host
   `ep-cool-lab-12345.us-east-2.aws.neon.tech`, Port `5432`, Database
   `neondb`, User `alex`, Password `AbCdEf123456`, SSL mode `require`.

   **Or, using Supabase** (free tier — see above): sign up at
   [supabase.com](https://supabase.com), click **New Project**, set a
   database password (this becomes the `postgres` user's password) and
   pick a region. Once it's provisioned, go to **Project Settings →
   Database** for the connection details. Supabase shows two connection
   strings — use the **direct connection** (port `5432`), not the
   "connection pooling" one (port `6543`/PgBouncer): PhotoLife's queries
   rely on server-side prepared statements (`QSqlQuery::prepare()`), which
   a transaction-mode connection pooler generally can't support reliably.

   **Or, using Railway** (usage-based free credit — see above): sign up at
   [railway.app](https://railway.app), start a **New Project**, and choose
   **Provision PostgreSQL** from the template gallery — this spins up a
   Postgres service with a database and role already created. Open that
   service and go to its **Connect** tab (or the **Variables** tab, which
   lists the same details as `PGHOST`/`PGPORT`/`PGDATABASE`/`PGUSER`/
   `PGPASSWORD`) for a connection string like:

   ```
   postgresql://postgres:AbCdEf123456@viaduct.proxy.rlwy.net:41234/railway
   ```

   which maps onto PhotoLife's Settings fields the same way as Neon's above
   — Railway's proxy host already speaks TLS, so `require` is a safe SSL
   mode default here too.

2. **Create the database and a role per collaborator** (Cloud SQL only —
   Neon, Supabase and Railway already did this in step 1; skip to step 4).
   Or one shared role, if you'd rather not manage several:

   ```
   gcloud sql databases create photolife --instance=photolife-catalogue
   gcloud sql users create [username] --instance=photolife-catalogue --password=...
   ```

   PhotoLife doesn't currently attribute writes to individual users in the
   UI, so separate roles buy you audit trail (via Postgres's own logs) and
   independent revocation, not a visibly different in-app experience.

3. **Decide how clients will reach it** (Cloud SQL only — Neon, Supabase
   and Railway are all reachable over the internet with TLS by default, no
   networking step needed). Two supported approaches:

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

## Where do the photos live?

Postgres (or SQLite) only ever holds the catalogue's *metadata* — file
paths, taxonomy, match decisions — never the photo files themselves.
PhotoLife has always worked this way, even in local SQLite mode: it indexes
whatever folder you add as a watched root, wherever that folder actually
lives.

That means the photo files can live somewhere other than each
collaborator's local disk — e.g. a Google Cloud Storage bucket — as long as
it's mounted as an ordinary local filesystem path (e.g. via
[`gcsfuse`](https://cloud.google.com/storage/docs/gcs-fuse)) before adding
it as a watched folder. PhotoLife just reads files at a path; it doesn't
know or care whether that path resolves to local disk, a network mount, or
a FUSE-mounted bucket.

**Known limitation — same path on every machine, required today.**
PhotoLife stores each file's *absolute* path in the catalogue
(`folder.path`, `rendition.path`) and has no relative-path or per-machine
path-remapping support yet. For a shared catalogue's photo paths to resolve
for everyone, every collaborator currently has to mount the shared storage
at the *exact same absolute path*. That's achievable within one OS (e.g.
every Linux collaborator using `/mnt/photolife-photos`), but **not** across
a mixed Linux/Windows group — the two platforms don't share a path syntax
at all (`/mnt/photolife-photos` vs `G:\...` or `\\...`), so there is no
single mount point that works on both today. Supporting that properly needs
a real change (paths stored relative to a per-machine-configurable library
root) that hasn't been built yet.

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
