"""Real SQLite + local NNTP integration tests for the viewer core.
Usage: python3 tests/test_viewer_download.py /path/to/viewer_driver
"""
import collections
import os
import pathlib
import socketserver
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib
from contextlib import closing

DRIVER = str(pathlib.Path(sys.argv.pop(1)).resolve())
DATA = bytes(range(256)) * 100


def encode(data):
    result = bytearray()
    for value in data:
        ch = (value + 42) & 255
        if ch in (0, 9, 10, 13, 32, 46, 61):
            result.extend((61, (ch + 64) & 255))
        else:
            result.append(ch)
    # Split before an escape, never between '=' and its following character.
    lines = []
    while result:
        length = min(128, len(result))
        if result[length - 1] == 61:
            length -= 1
        line = bytes(result[:length])
        del result[:length]
        lines.append((b'.' if line.startswith(b'.') else b'') + line + b'\r\n')
    return b''.join(lines)


def body(part, total, mode):
    width = len(DATA) // total
    begin = (part - 1) * width + 1
    end = len(DATA) if part == total else part * width
    data = DATA[begin - 1:end]
    name = '../escape.bin' if mode == 'unsafe' else 'sample data.bin'
    if mode == 'name_conflict' and part == 2:
        name = 'different.bin'
    if mode == 'overlap' and part == 2:
        begin -= 1
        end -= 1
    crc = zlib.crc32(data)
    if mode == 'badcrc':
        crc ^= 1
    if total == 1:
        header = f'=ybegin line=128 size={len(DATA)} name={name}\r\n'
        trailer = f'=yend size={len(data)} crc32={crc:08x}\r\n'
    else:
        header = (f'=ybegin part={part} total={total} line=128 size={len(DATA)} name={name}\r\n'
                  f'=ypart begin={begin} end={end}\r\n')
        fullcrc = zlib.crc32(DATA) ^ (1 if mode == 'bad_full_crc' else 0)
        trailer = f'=yend size={len(data)} part={part} pcrc32={crc:08x}'
        if part == total:
            trailer += f' crc32={fullcrc:08x}'
        trailer += '\r\n'
    if mode == 'wrong_size':
        trailer = trailer.replace(f'size={len(data)}', f'size={len(data) + 1}')
    return header.encode() + encode(data) + trailer.encode()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 64

    def __init__(self, total, mode):
        super().__init__(('127.0.0.1', 0), Handler)
        self.total, self.mode = total, mode
        self.lock = threading.Lock()
        self.attempts = collections.Counter()
        self.active = self.peak = 0


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        server = self.server
        try:
            self.wfile.write(b'200 test server\r\n')
            for raw in self.rfile:
                command = raw.decode().strip()
                if command.startswith('AUTHINFO USER'):
                    self.wfile.write(b'381 password\r\n')
                    continue
                if command.startswith('AUTHINFO PASS'):
                    self.wfile.write(b'481 rejected\r\n' if server.mode == 'authfail' else b'281 authenticated\r\n')
                    continue
                if command.startswith('GROUP '):
                    self.wfile.write(b'211 group\r\n')
                    continue
                if not command.startswith('BODY '):
                    self.wfile.write(b'500 unsupported\r\n')
                    continue
                argument = command.split()[1]
                part = int(argument[1:].split('@')[0]) if argument.startswith('<') else int(argument)
                with server.lock:
                    server.attempts[part] += 1
                    attempt = server.attempts[part]
                    server.active += 1
                    server.peak = max(server.peak, server.active)
                try:
                    time.sleep(0.1 if server.mode == 'cancel' else 0.04)
                    if server.mode == 'missing' and part == 2:
                        self.wfile.write(b'430 missing\r\n')
                        continue
                    self.wfile.write(b'222 body\r\n')
                    wire = body(part, server.total, server.mode)
                    if server.mode == 'disconnect' and attempt == 1:
                        self.wfile.write(wire[:100])
                        return
                    if server.mode == 'truncated':
                        self.wfile.write(wire[:100])
                        return
                    if server.mode == 'oversized_line':
                        wire = b'A' * 70000 + b'\r\n'
                    self.wfile.write(wire + b'.\r\n')
                finally:
                    with server.lock:
                        server.active -= 1
        except (BrokenPipeError, ConnectionResetError):
            pass


def database(path, total, missing=False, conflict=False, no_message=False, extra=0):
    with closing(sqlite3.connect(path)) as con:
        con.execute('CREATE TABLE articles (artnum INTEGER, subject TEXT, author TEXT, date TEXT, group_name TEXT, message_id TEXT)')
        for part in range(1, total + 1):
            if missing and part == 2:
                continue
            row = (part, f'"sample data.bin" yEnc ({part}/{total})', 'Poster', 'Today', 'test.group', '' if no_message else f'<{part}@test>')
            con.execute('INSERT INTO articles VALUES (?,?,?,?,?,?)', row)
        if conflict:
            con.execute('INSERT INTO articles VALUES (?,?,?,?,?,?)', (100, '"sample data.bin" yEnc (1/4)', 'Poster', 'Today', 'test.group', '<other@test>'))
        for i in range(extra):
            con.execute('INSERT INTO articles VALUES (?,?,?,?,?,?)', (1000 + i, f'Text article {i}', None, None, 'test.group', None))
        con.commit()


class ViewerTests(unittest.TestCase):
    def run_download(self, mode='normal', total=4, no_message=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            db = root / 'news.db'
            database(db, total, missing=mode == 'missing_headers', conflict=mode == 'conflict', no_message=no_message)
            destination = root / 'chosen location'
            destination.mkdir()
            existing = destination / 'sample data.bin'
            if mode == 'existing':
                existing.write_bytes(b'leave me alone')
            with Server(total, mode) as server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                env = os.environ.copy()
                if mode in ('auth', 'authfail'):
                    env.update(TEST_NNTP_USER='reader', TEST_NNTP_PASS='secret')
                args = [DRIVER, 'download', str(db), str(destination), '127.0.0.1', str(server.server_address[1]), '2', '4']
                if mode == 'cancel':
                    args.append('30')
                try:
                    result = subprocess.run(args, capture_output=True, text=True, timeout=20, env=env)
                finally:
                    server.shutdown()
                    thread.join()
                good = mode in ('normal', 'disconnect', 'auth')
                self.assertEqual(result.returncode, 0 if good else 1, result.stderr)
                self.assertNotIn('AddressSanitizer', result.stderr)
                self.assertNotIn('runtime error:', result.stderr)
                if good:
                    self.assertEqual(existing.read_bytes(), DATA)
                    if total > 1:
                        self.assertGreater(server.peak, 1)
                elif mode == 'existing':
                    self.assertEqual(existing.read_bytes(), b'leave me alone')
                else:
                    self.assertFalse(existing.exists())
                self.assertFalse(list(destination.glob('.nntp2sql-*')))
                self.assertFalse((root / 'escape.bin').exists())

    def test_multipart(self): self.run_download()
    def test_single_part(self): self.run_download(total=1)
    def test_fallback_article_number(self): self.run_download(no_message=True)
    def test_reconnect(self): self.run_download('disconnect')
    def test_corrupt_crc(self): self.run_download('badcrc')
    def test_corrupt_full_crc(self): self.run_download('bad_full_crc')
    def test_wrong_size(self): self.run_download('wrong_size')
    def test_truncation(self): self.run_download('truncated')
    def test_missing_server_part(self): self.run_download('missing')
    def test_missing_database_part(self): self.run_download('missing_headers')
    def test_conflicting_headers(self): self.run_download('conflict')
    def test_conflicting_filename(self): self.run_download('name_conflict')
    def test_overlapping_parts(self): self.run_download('overlap')
    def test_unsafe_filename(self): self.run_download('unsafe')
    def test_existing_file(self): self.run_download('existing')
    def test_cancel(self): self.run_download('cancel')
    def test_auth(self): self.run_download('auth')
    def test_auth_failure(self): self.run_download('authfail')
    def test_line_limit(self): self.run_download('oversized_line')

    def test_catalog_over_1000_rows(self):
        with tempfile.TemporaryDirectory() as temporary:
            db = pathlib.Path(temporary) / 'news.db'
            database(db, 4, extra=1500)
            result = subprocess.run([DRIVER, 'inspect', str(db)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ['1504 1', '4 4 1'])

    def test_missing_database_is_not_created(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / 'missing.db'
            result = subprocess.run([DRIVER, 'inspect', str(path)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1)
            self.assertFalse(path.exists())

    def test_catalog_groups_from_sql(self):
        with tempfile.TemporaryDirectory() as temporary:
            db = pathlib.Path(temporary) / 'groups.db'
            with closing(sqlite3.connect(db)) as con:
                con.execute('CREATE TABLE groups (id INTEGER PRIMARY KEY, name TEXT UNIQUE, article_count INTEGER, first INTEGER, last INTEGER)')
                con.execute('CREATE TABLE articles (artnum INTEGER, subject TEXT, author TEXT, date TEXT, group_name TEXT, message_id TEXT)')
                con.execute("INSERT INTO groups VALUES (1, 'comp.lang.c', 10, 1, 10)")
                con.execute("INSERT INTO groups VALUES (2, 'alt.binaries.boneless', 5, 1, 5)")
                con.execute("INSERT INTO articles VALUES (1, 'Post 1', 'alice', '2026-01-01', 'sci.math', '<1@test>')")
                con.execute("INSERT INTO articles VALUES (2, 'Post 2', 'bob', '2026-01-01', 'alt.binaries.boneless', '<2@test>')")
                con.commit()
            result = subprocess.run([DRIVER, 'groups', str(db)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ['alt.binaries.boneless', 'comp.lang.c', 'sci.math'])

    def test_queue_download_and_universal_search(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            db = root / 'queue.db'
            destination = root / 'out'
            destination.mkdir()
            with closing(sqlite3.connect(db)) as con:
                con.execute('CREATE TABLE articles (artnum INTEGER, subject TEXT, author TEXT, date TEXT, group_name TEXT, message_id TEXT)')
                for part in range(1, 3):
                    con.execute('INSERT INTO articles VALUES (?,?,?,?,?,?)',
                                (part, f'"alpha.bin" yEnc ({part}/2)', 'alice@example.com', '2026-04-20', 'alt.binaries.boneless', f'<{part}@test>'))
                for part in range(1, 3):
                    con.execute('INSERT INTO articles VALUES (?,?,?,?,?,?)',
                                (10 + part, f'"bravo.bin" yEnc ({part}/2)', 'carol@example.com', '2026-04-20', 'alt.binaries.test', f'<{10 + part}@test>'))
                con.execute('INSERT INTO articles VALUES (?,?,?,?,?,?)',
                            (99, 'Plain text discussion note', 'dave@example.com', '2026-04-20', 'alt.discussion', '<note-99@test>'))
                con.commit()

            # Search files by filename, author, completeness
            s1 = subprocess.run([DRIVER, 'search-files', str(db), 'bravo.bin carol complete'],
                                capture_output=True, text=True, timeout=10)
            self.assertEqual(s1.returncode, 0, s1.stderr)
            self.assertEqual(len(s1.stdout.strip().splitlines()), 1)
            self.assertIn('bravo.bin', s1.stdout)

            # Search queued files specifically using 'queued' keyword
            sq = subprocess.run([DRIVER, 'search-files', str(db), 'queued', 'queue-even'],
                                capture_output=True, text=True, timeout=10)
            self.assertEqual(sq.returncode, 0, sq.stderr)
            self.assertEqual(len(sq.stdout.strip().splitlines()), 1)
            self.assertIn('alpha.bin', sq.stdout)

            # Search articles by message-id, article #, date, and group
            sa = subprocess.run([DRIVER, 'search-articles', str(db), '<note-99@test> #99 alt.discussion 2026-04-20 article'],
                                capture_output=True, text=True, timeout=10)
            self.assertEqual(sa.returncode, 0, sa.stderr)
            self.assertEqual(sa.stdout.strip(), '99')

            # Batch queue download of a complete yEnc file via queue-download
            db_single = root / 'single_queue.db'
            database(db_single, 4)
            with Server(4, 'normal') as server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    qdl = subprocess.run(
                        [DRIVER, 'queue-download', str(db_single), str(destination), '127.0.0.1',
                         str(server.server_address[1]), '2', '4'],
                        capture_output=True, text=True, timeout=20
                    )
                finally:
                    server.shutdown()
                    thread.join()
            self.assertEqual(qdl.returncode, 0, qdl.stderr)
            self.assertIn('QUEUE_OK 1/1', qdl.stdout)
            self.assertEqual((destination / 'sample data.bin').read_bytes(), DATA)

    def test_2m_records_and_2m_queued_files_scale(self):
        result = subprocess.run([DRIVER, 'scale-check', '2000000'], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'SCALE_OK 2000000 2000000 2000000 1')

    def test_default_connections_and_schema_discovery(self):
        # 1. Verify default connection strings for SQLite, MySQL, and PostgreSQL
        defs = subprocess.run([DRIVER, 'defaults'], capture_output=True, text=True, timeout=10)
        self.assertEqual(defs.returncode, 0, defs.stderr)
        self.assertIn('sqlite=nntp2sql.db', defs.stdout)
        self.assertIn('mysql=127.0.0.1,nntp2sql,root,,3306', defs.stdout)
        self.assertIn('postgres=127.0.0.1,nntp2sql,postgres,,5432', defs.stdout)

        # 2. Verify flexible connection string parsing and database selection formatting
        p1 = subprocess.run([DRIVER, 'parse-conn', 'mysql', '127.0.0.1,nntp2sql,root,,3306'],
                            capture_output=True, text=True, timeout=10)
        self.assertEqual(p1.returncode, 0)
        self.assertIn('host=127.0.0.1 db=nntp2sql user=root pass= port=3306 def_db=1 def_user=1 def_host=1', p1.stdout)

        p2 = subprocess.run([DRIVER, 'parse-conn', 'mysql', 'news0'],
                            capture_output=True, text=True, timeout=10)
        self.assertEqual(p2.returncode, 0)
        self.assertIn('host=127.0.0.1 db=news0 user=root pass= port=3306 def_db=0 def_user=1 def_host=1', p2.stdout)

        f1 = subprocess.run([DRIVER, 'format-conn', 'mysql', '127.0.0.1,nntp2sql,root,,3306', 'news0'],
                            capture_output=True, text=True, timeout=10)
        self.assertEqual(f1.returncode, 0)
        self.assertEqual(f1.stdout.strip(), '127.0.0.1,news0,root,,3306')

        f2 = subprocess.run([DRIVER, 'format-conn', 'postgres', '127.0.0.1,nntp2sql,postgres,,5432', 'news0'],
                            capture_output=True, text=True, timeout=10)
        self.assertEqual(f2.returncode, 0)
        self.assertEqual(f2.stdout.strip(), '127.0.0.1,news0,postgres,,5432')

        # 3. Verify schema-based database discovery (detects news0 even without .db extension, ignores non-nntp2sql DBs)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            news0 = root / 'news0'
            news1 = root / 'news1.db'
            unrelated = root / 'unrelated.db'
            database(news0, 4)
            database(news1, 4)
            with sqlite3.connect(news0) as conn:
                conn.execute('CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT)')
                conn.execute("INSERT OR REPLACE INTO settings VALUES ('host', 'news.usenet.example')")
                conn.execute("INSERT OR REPLACE INTO settings VALUES ('port', '563')")
                conn.execute("INSERT OR REPLACE INTO settings VALUES ('threads', '12')")
                conn.execute("INSERT OR REPLACE INTO settings VALUES ('group', 'alt.binaries.boneless')")
            with sqlite3.connect(unrelated) as conn:
                conn.execute('CREATE TABLE customers (id INTEGER PRIMARY KEY, email TEXT)')

            disc = subprocess.run([DRIVER, 'discover', 'sqlite', str(root)],
                                  capture_output=True, text=True, timeout=10)
            self.assertEqual(disc.returncode, 0, disc.stderr)
            found_names = [pathlib.Path(line).name for line in disc.stdout.strip().splitlines() if line.strip()]
            self.assertEqual(found_names, ['news0', 'news1.db'])

            # 4. Verify click-and-go default connection auto-resolves to news0 when nntp2sql.db is absent
            insp = subprocess.run([DRIVER, 'inspect-db', 'sqlite', ''],
                                  cwd=str(root), capture_output=True, text=True, timeout=10)
            self.assertEqual(insp.returncode, 0, insp.stderr)
            self.assertIn('db=news0', insp.stdout)
            self.assertIn('host=news.usenet.example port=563 workers=12', insp.stdout)
            self.assertIn('articles=4 files=1', insp.stdout)


if __name__ == '__main__':
    unittest.main()
