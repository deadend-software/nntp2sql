# nntp2sql (beta 0.50)

Download Usenet (NNTP) article headers into a SQL database, then browse, read and fetch them with
GTK4 viewers. Everything is plain C and builds on macOS, Linux and BSD.

| Program | What it is |
|---|---|
| `nntp2sql` | Command-line downloader: NNTP server → SQLite / MySQL / MariaDB / PostgreSQL |
| `isotope-viewer` | GTK4 browser for the stored articles and yEnc files: opens by group, threaded text articles, article windows with images, replies, file downloads |
| `hydrogen-group-viewer` | GTK4 browser for the `groups` table: every newsgroup, filterable and sortable |

Run `nntp2sql` with no arguments for the complete, always-current option reference. Man pages:
`nntp2sql(1)`, `isotope-viewer(1)`, `hydrogen-group-viewer(1)`.

## Features

**Downloading**
- NNTP over implicit TLS (`--ssl`) or STARTTLS, optional authentication
- Parallel connections (16 by default, up to 64), automatic reconnect and retry, live progress bar
- Two modes: `--headers-only` (bulk XOVER, much faster) or full HEAD per article
- `--limit N` for the newest N articles, `--articles N` for exactly N stored,
  `--since MMDDYYYY` to start at the first article posted on or after a date,
  `--resume` to continue from the last article already stored for the group
- Clean Ctrl+C: everything stored so far is kept
- Bounded memory on huge groups (the work queue is built in windows, not for the whole group at once)

**Storage**
- SQLite, MySQL/MariaDB and PostgreSQL
- One row per `(group, article number)`: re-fetching updates rows instead of duplicating them
- `--remove-duplicates` repairs older databases (same article number or same Message-ID within a group;
  cross-posts are kept)
- Headers that are not valid UTF-8 (Latin-1 / Windows-1252 from old newsreaders) are converted instead of
  being rejected by the database

**Groups**
- `--list-groups` downloads the server's whole newsgroup list into the `groups` table

**Configuration**
- `--save-config` saves your options to `~/.config/nntp2sql/nntp2sql.conf` (mode 0600)
- `--save-config-db` saves them inside the database, in a table named after it (`news0` → `news0_settings`);
  the two are independent
- `--load-config` / `--load-config-db` show what is saved (or, with an option that asks for work, use it)
- `--confirm-db-defaults` shows what the database's stored defaults would change and asks first
- Both viewers use these as defaults, so a saved server and login are filled in when you open them

**Export and viewers**
- HTML export of one group or a list of groups
- Viewers detect nntp2sql databases (`news0`, `news1`, ...) automatically and start with connection
  strings that work on a default local MySQL, PostgreSQL or SQLite
- isotope-viewer loads only the group list on open and one group's articles when you choose it; text articles
  are shown as threads; Read article opens a window with the text and any image, from which you can reply

## Requirements

- Tooling: Autotools (`autoconf`, `automake`, `libtool`), `pkg-config`, a C11 compiler.
- Core libraries: `openssl`, `sqlite3`, `mysqlclient` (MariaDB/MySQL), optional `libpq` (PostgreSQL;
  the viewers load it at run time if present).
- Viewer libraries (GTK4): `gtk4`, `glib2`, `pango`, `harfbuzz`, `gdk-pixbuf2`, `cairo`, `graphene`, and
  zlib.

macOS (Homebrew)
```sh
brew install autoconf automake libtool pkg-config \
             openssl@3 sqlite mysql gtk4 pango harfbuzz gdk-pixbuf cairo graphene libpq
```

Debian/Ubuntu
```sh
sudo apt-get install -y build-essential autoconf automake libtool pkg-config \
    libssl-dev libsqlite3-dev libmysqlclient-dev libpq-dev \
    libgtk-4-dev libglib2.0-dev libpango1.0-dev libharfbuzz-dev \
    libgdk-pixbuf-2.0-dev libcairo2-dev libgraphene-1.0-dev
```

Fedora
```sh
sudo dnf install -y gcc autoconf automake libtool pkgconf-pkg-config \
    openssl-devel sqlite-devel mysql-devel postgresql-devel \
    gtk4-devel glib2-devel pango-devel harfbuzz-devel \
    gdk-pixbuf2-devel cairo-devel graphene-devel
```

FreeBSD
```sh
pkg install autoconf automake libtool pkgconf \
    openssl sqlite3 mysql80-client postgresql15-client \
    gtk4 glib pango harfbuzz gdk-pixbuf2 cairo graphene
```

## Build
```sh
autoreconf -fi
./configure
make -j
sudo make install        # installs the programs and their man pages
```
- The viewers are built by default; disable them with `./configure --disable-viewer`.
- PostgreSQL support is built into `nntp2sql` when `libpq` is found.

## Quick start

Newest 5000 articles of a group into a local SQLite file:
```sh
./nntp2sql --host news.example.com --ssl --user me --pass secret \
    --db-type sqlite --db-name news.db --init-db \
    --group alt.test --limit 5000 --headers-only
./isotope-viewer --db-type sqlite news.db
```

Everything posted since 15 October 2026, into MySQL:
```sh
./nntp2sql --host news.example.com --ssl --user me --pass secret \
    --db-type mysql --db-name news0 --db-user root --init-db \
    --group alt.test --since 10152026 --headers-only
```

## Command reference (summary)

Run `./nntp2sql` for descriptions and defaults of every switch.

| Area | Options |
|---|---|
| News server | `--host` `--port` `--ssl` `--starttls` `--user` `--pass` |
| Database | `--db-type` `--db-name` `--db-host` `--db-port` `--db-user` `--db-pass` |
| What to fetch | `--group` `--headers-only`/`--xover` `--limit` `--articles`/`--count`/`--max-articles`/`-n` `--since`/`--from-date` `--resume`/`--continue`/`--from-last` `--threads` `--retries` `--upsert` `--update-only`/`--no-upsert` |
| Groups and maintenance | `--list-groups`/`--all-groups` `--remove-duplicates`/`--dedupe` `--export-html` `--export-html-out` `--group-list` |
| Creating a database | `--init-db` `--create-db` `--skeleton-db`/`--create-skeleton-db` |
| Configuration | `--save-config` `--save-config-db` `--load-config`/`--read-config` `--load-config-db`/`--read-config-db` `--confirm-db-defaults`/`--ask-db-defaults` `--no-config` `--conf` `--write-conf` |
| Output | `--progress-width` `--verbose` `--log` `--help` |

`--list-groups`, `--remove-duplicates`, `--create-db`, `--skeleton-db`, `--write-conf`, `--save-config`,
`--save-config-db` do their job and exit without downloading articles. `--load-config` and `--load-config-db`
do the same when used alone, and read settings for the run when combined with an option that asks for work.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 2 | Invalid or missing arguments |
| 3 | Bad configuration file |
| 10–15 | Network, TLS, NNTP greeting, NNTP command or login failure |
| 20–22 | Database connection, schema or statement failure |
| 30 | Runtime error, or a download that ended with failed requests or writes |
| 130 | Stopped with Ctrl+C |

## Resuming

`--resume` (aliases `--continue`, `--from-last`) picks up where the last run stopped. It looks up the highest
article number already stored for the group and starts there, so each run only fetches what is new:
```sh
./nntp2sql --db-type mysql --db-name news0 --load-config-db --group alt.test --resume --headers-only
```
The last stored article is fetched again (harmless, writes are upserts) so a batch that was cut off is
completed. If nothing is stored yet the run starts normally; if the stored numbers are beyond the server's
range (a renumbered group) it warns and starts from the beginning. With `--limit N` it takes N articles
going forward from that point; with `--since` it starts at the later of the two.

## Saving configuration

```sh
# To the file ~/.config/nntp2sql/nntp2sql.conf only:
./nntp2sql --host news.example.com --ssl --user me --pass secret --threads 8 --save-config

# Into the database only (a table named after it: news0 -> news0_settings):
./nntp2sql --host news.example.com --ssl --user me --pass secret --threads 8 \
    --db-type mysql --db-name news0 --db-user root --save-config-db

# Later: name only what changes, and review what the database would apply:
./nntp2sql --db-type mysql --db-name news0 --db-user root --confirm-db-defaults \
    --group alt.test --limit 1000
```

**Reading settings explicitly.** The saved file is already read silently into normal runs when it exists,
and the database's settings are applied automatically. `--load-config` and `--load-config-db` make that
explicit and report what was read (passwords hidden); they fail with exit code 3 if the file or the stored
settings are missing. `--load-config-db` always needs the database named on the command line with `--db-name`
(e.g. `--db-type mysql --db-name news0`); one supplied only by a config file does not count. On their own they only *show* the settings and exit, touching no server. Add an option
that asks for work and they read the settings and then do it:
```sh
./nntp2sql --db-type mysql --db-name news0 --load-config-db                   # show what news0 stores, then exit
./nntp2sql --db-type mysql --db-name news0 --load-config-db --group alt.test  # use news0's settings and download headers
```
Combine one with a save option to copy settings between the two places (then exit):
```sh
# database -> file
./nntp2sql --db-type mysql --db-name news0 --db-user root --load-config-db --save-config
# file -> another database
./nntp2sql --load-config --db-type mysql --db-name news1 --db-user root --save-config-db
```

**Precedence.** Command-line options always win. Below that, server, port, group, threads and similar
settings stored in the database override the config file, while the NNTP login from the config file wins
over the one stored in the database. Anything unset takes the built-in default. The saved file is read
automatically when it exists; `--no-config` skips it.

**Both places hold passwords in plain text.** The file is created readable only by you; protect the
database the same way.

## Database schema

| Table | Columns |
|---|---|
| `groups` | `name` (unique), `article_count`, `first`, `last` |
| `articles` | `artnum`, `subject`, `author`, `date`, `message_id`, `refs`, `bytes`, `line_count`, `group_name`; unique index on `(group_name, artnum)` |
| `<database>_settings` | `key`, `value` (options stored by `--save-config-db`), e.g. `news0_settings` in the database `news0`; a SQLite file `data/news.db` uses `news_settings` |

The options table is named after the database, so each database keeps its own settings (only letters, digits
and `_` are kept in the name). Databases made by older versions stored them in one table called `settings`:
the first time `nntp2sql` opens such a database it copies those rows into the new table (the old table is left
in place), and the viewers read the old table whenever the new one is empty.

## Viewers

```sh
./isotope-viewer                                     # opens with the defaults; click Open
./isotope-viewer --db-type mysql "127.0.0.1,news0,root,,3306"
./isotope-viewer --db-type postgres "host=localhost dbname=news0 user=me password=pw"
./hydrogen-group-viewer --db-type sqlite news.db
```

Connection strings, by backend: SQLite takes a file (or a folder of files); MySQL and PostgreSQL take
`host,database,user,password,port` (also `scheme://user:pass@host:port/db` and
`host=... dbname=... user=... password=...`). Defaults for a stock local install:
`nntp2sql.db`, `127.0.0.1,nntp2sql,root,,3306`, `127.0.0.1,nntp2sql,postgres,,5432`. If the default
database is not there, the first nntp2sql database found is opened instead.

**isotope-viewer** opens quickly: it loads only the *list of groups* and holds no articles until you choose
one (**Select a group to load...** is the first dropdown entry; "All groups" loads everything and is slow on
large databases).

- **Modes:** Files / Articles / **Text articles**. Text articles shows only text posts as *threads*: each
  reply sits under the post it answers (from the `References` header), indented and marked with an arrow.
- **Hide text articles** (Articles mode) and **Hide incomplete files** filter the list; search covers subject,
  author, group, date, Message-ID and more.
- **Read article** opens the article in its own window: headers, text, and any image it carries (a base64
  MIME attachment or an inline yEnc body, checksum-verified) with zoom, fit and Save image.
- **Reply** (in that window) opens an editor with the original quoted and the `References` chain built for
  you. Press **Post** and confirm: the reply is sent with NNTP POST through the server in the main window.
  Posting is public and cannot be undone, so a confirmation always appears first.
- **View image** shows an image file in the same article window; **Save viewed images** keeps a copy.
- Queue and download files (parts are assembled and verified against sizes and CRC32; existing files are never
  overwritten), and **Auto-load text articles** to see a post's body in the details pane as you select it.

**hydrogen-group-viewer** shows every row of the `groups` table with its estimated article count and
first/last article numbers. Fill it with `nntp2sql --list-groups`.

## Application icons

Each GUI program has its own icon, named like the program:

| Program | Icon | Artwork |
|---|---|---|
| `isotope-viewer` | A newspaper (masthead, headlines, photo, columns) with a small network badge | `icons/hicolor/scalable/apps/isotope-viewer.svg` |
| `hydrogen-group-viewer` | A card listing newsgroups as an indented hierarchy, on a green tile, with a hydrogen-atom badge | `icons/hicolor/scalable/apps/hydrogen-group-viewer.svg` |

Both are rendered at 16–512 px. Where the platform allows it, each program uses its icon automatically:

| Platform | How the icon is used | Status |
|---|---|---|
| macOS | The Dock icon is set at run time through the Objective-C runtime (a bare binary has no bundle icon); `icons/<program>.icns` is provided for an `.app` bundle | Tested: for both programs the Dock icon changes from the generic default to the program's own icon |
| Linux / BSD | GTK looks up the icon `<program>` in the icon theme. `make install` puts the PNG/SVG files in `<prefix>/share/icons/hicolor` and the `.desktop` files in `<prefix>/share/applications`; from a build folder the programs add `./icons` to GTK's search path | Lookup tested with GTK on macOS and the install paths checked with a staged install; not run on a Linux or BSD desktop |
| Windows | The window icon comes from the same icon-theme lookup. Each `.exe` gets its icon from `icons/<program>.rc` / `<program>.ico`, compiled with `windres` when configured for a MinGW host | Written but not built or run (no Windows machine available) |

`icons/make-icons.sh` regenerates every size, the `.ico` and the `.icns` of both icons from the SVGs (needs
`rsvg-convert`, `python3`, and `iconutil` on macOS). GTK has no per-window icons on macOS and the icon theme
cannot be used there for the Dock, which is why the macOS path is separate.

## HTML export
```sh
./nntp2sql --db-type sqlite --db-name news.db --export-html --group alt.test --export-html-out alt.test.html
./nntp2sql --db-type sqlite --db-name news.db --export-html --group-list groups.txt --export-html-out ./exports
```

## How downloading works

Work is split into batches held in a queue; each worker thread owns its own NNTP connection and database
writes are serialized. `--headers-only` requests XOVER batches (up to 500 articles); otherwise HEAD is
requested for each article number. Neither mode downloads article bodies. The queue is expanded one
window at a time, so even a group spanning billions of article numbers starts immediately and uses a
small, constant amount of memory. Connections have a 10-second connect timeout and a 15-second I/O
timeout. Missing or expired articles are counted separately; failed requests or database writes make the
exit code 30.

## Known issues and limits

- **Starting point.** Without `--limit`, `--articles`, `--since` or `--resume`, a group is read from its
  oldest article number upward. On large binary groups the oldest articles have usually expired, so the
  early part of the run is almost entirely "missing". Use `--since`, `--limit` or `--resume`.
- **`--since` accuracy.** The start is found by binary search on article dates, which assumes dates rise
  roughly with article numbers; a few mis-dated posts can shift it slightly. It needs the server's XOVER,
  and an article that cannot be read during the search counts as older than your date.
- **Sparse numbering.** Some servers number articles sparsely, so a group's stored article count (and the
  `--list-groups` estimate) can be much larger than the number of real articles.
- **Text encoding.** Non-UTF-8 header bytes are converted assuming Windows-1252.
- **Plain-text passwords** in the config file and in the database's `<database>_settings` table.
- **PostgreSQL** is the least exercised backend, and `--init-db` cannot create a PostgreSQL database.
- **Early exits.** `--write-conf` and `--save-config` (without `--save-config-db`) exit before
  contacting any server or database.
- **Incomplete files are not saved.** isotope-viewer saves a yEnc file only when every part is in the database
  *and* still on the server and the decoded sizes and CRC32 verify. With a missing part it refuses the file with
  a message and writes nothing (no partial file is left behind).
- **isotope-viewer.** Loads one group's articles at a time (choosing "All groups" loads everything into
  memory); threads use only the `References` header and only in Text articles mode; only the first image of
  an article is shown and uuencoded attachments are not decoded; images in file downloads are recognised by
  the file name in the subject; a download will not overwrite an existing file.
- **Posting.** Replies only: no new threads, cancels or attachments; the server assigns the Message-ID and
  Date; whether posting is allowed depends on your server and account.
- **hydrogen-group-viewer.** Does not remember connection strings between runs.
- **Icons.** The Windows icon resource and the Linux/BSD desktop integration have not been run on those systems; a
  Dock/taskbar icon also depends on the desktop honouring the `.desktop` file or window icon.
- **Xcode project.** The Xcode project builds `nntp2sql` and `isotope-viewer` only; use Autotools for
  `hydrogen-group-viewer`.

## Tests

Local integration tests that need no external NNTP account:
```sh
python3 tests/test_threaded_download.py /path/to/built/nntp2sql
```

## Source layout

`main.c` (the CLI), `src/export_html.c` (HTML export), `src/db_postgres.c` (PostgreSQL adapter),
`gui/viewer.c` (isotope-viewer), `gui/icon.h` (application icon), `icons/` (icon artwork, `.desktop` files, Windows resource), `gui/article.h` (article parsing, replies and posting), `gui/group_viewer.c` (hydrogen-group-viewer), `gui/catalog.h`
(database connection, detection and loading shared by the viewers), `gui/transfer.h` and `gui/yenc.h`
(downloading and yEnc decoding), `man/` (manual pages), `configure.ac` and `Makefile.am`.

Cherry Soda Research and Software
