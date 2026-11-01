"""Local NNTP integration tests. Run: python3 tests/test_threaded_download.py BINARY."""
import collections
from contextlib import closing
import pathlib
import re
import signal
import socketserver
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, mode):
        super().__init__(("127.0.0.1", 0), Handler)
        self.mode = mode
        self.lock = threading.Lock()
        self.attempts = collections.Counter()
        self.connections = set()
        self.active = self.peak = self.serial = 0


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        s = self.server
        with s.lock:
            s.serial += 1
            connection = s.serial
        try:
            self.wfile.write(b"200 local test server\r\n")
            for raw in self.rfile:
                command = raw.decode().strip()
                if command.startswith("GROUP "):
                    if s.mode == "worker_failure" and connection > 1:
                        self.wfile.write(b"411 unavailable group\r\n")
                        return
                    if s.mode == "high_artnum":
                        self.wfile.write(b"211 600 5000000001 5000000600 test.group\r\n")
                    else:
                        self.wfile.write(b"211 600 1 600 test.group\r\n")
                    continue
                if not command.startswith(("HEAD ", "XOVER ")):
                    self.wfile.write(b"500 unexpected command\r\n")
                    continue
                with s.lock:
                    s.attempts[command] += 1
                    attempt = s.attempts[command]
                    s.connections.add(connection)
                    s.active += 1
                    s.peak = max(s.peak, s.active)
                try:
                    time.sleep(0.003)
                    overview = command.startswith("XOVER")
                    first, last = (map(int, command.split()[1].split("-")) if overview
                                   else (int(command.split()[1]), int(command.split()[1])))
                    if s.mode == "exhaust" or (s.mode == "reconnect" and attempt == 1):
                        self.wfile.write(b"224 overview\r\n" if overview else b"221 headers\r\n")
                        self.wfile.write(b"partial response without terminator\r\n")
                        return
                    if not overview and first % 17 == 0:
                        self.wfile.write(b"423 missing article\r\n")
                        continue
                    self.wfile.write(b"224 overview\r\n" if overview else b"221 headers\r\n")
                    for number in range(first, last + 1):
                        if number % 17 == 0:
                            continue
                        subject = "" if number % 5 == 0 else f"Subject {number}"
                        if overview:
                            line = f"{number}\t{subject}\tAuthor {number}\tDate {number}\t<{number}>\t\t123\t7\r\n"
                            if s.mode == "malformed":
                                line = f"{number}\ttoo few fields\r\n"
                            if s.mode == "duplicate":
                                line += line
                        else:
                            line = (f"Subject: {subject}\r\nFrom: Author {number}\r\nDate: Date {number}\r\n"
                                    f"Message-ID: <{number}>\r\nReferences: \r\nBytes: 123\r\nLines: 7\r\n")
                        self.wfile.write(line.encode())
                        if s.mode == "slow_stream":
                            self.wfile.flush()
                            time.sleep(0.008)
                    self.wfile.write(b".\r\n")
                finally:
                    with s.lock:
                        s.active -= 1
        except (BrokenPipeError, ConnectionResetError):
            pass


class Downloads(unittest.TestCase):
    def run_case(self, overview, mode="normal", threads=4, limit=None):
        with tempfile.TemporaryDirectory() as directory:
            db = pathlib.Path(directory) / "articles.db"
            if mode == "dbfail":
                with closing(sqlite3.connect(db)) as con:
                    con.execute("CREATE TABLE articles (id INTEGER PRIMARY KEY, artnum INTEGER, subject TEXT, author TEXT, date TEXT, message_id TEXT, refs TEXT, bytes INTEGER, line_count INTEGER, group_name TEXT)")
                    con.execute("CREATE TRIGGER reject_write BEFORE INSERT ON articles BEGIN SELECT RAISE(FAIL, 'test database failure'); END")
            with Server(mode) as server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                args = [BINARY, "--host", "127.0.0.1", "--port", str(server.server_address[1]),
                        "--db-type", "sqlite", "--db-name", str(db), "--group", "test.group",
                        "--upsert", "--threads", str(threads), "--retries", "1"]
                if overview:
                    args.append("--headers-only")
                if limit:
                    args.extend(["--limit", str(limit)])
                try:
                    result = subprocess.run(args, capture_output=True, text=True, timeout=45)
                finally:
                    server.shutdown()
                    thread.join()
                failing = mode in ("exhaust", "dbfail", "malformed", "duplicate", "worker_failure")
                self.assertEqual(result.returncode, 30 if failing else 0, result.stderr[-3000:])
                self.assertNotIn("AddressSanitizer", result.stderr)
                self.assertNotIn("runtime error:", result.stderr)
                if mode != "worker_failure":
                    self.assertGreater(server.peak, 1 if threads > 1 else 0)
                self.assertTrue(all(k.startswith("XOVER" if overview else "HEAD") for k in server.attempts))
                if mode == "reconnect":
                    self.assertTrue(all(v == 2 for v in server.attempts.values()))
                    self.assertGreater(len(server.connections), threads)
                with closing(sqlite3.connect(db)) as con:
                    rows = con.execute("SELECT artnum,subject,author,date,message_id,refs,bytes,line_count FROM articles ORDER BY artnum").fetchall()
                if failing:
                    self.assertEqual(rows, [])
                else:
                    expected = [(n, "" if n % 5 == 0 else f"Subject {n}", f"Author {n}", f"Date {n}", f"<{n}>", "", 123, 7)
                                for n in range(601 - limit if limit else 1, 601) if n % 17]
                    self.assertEqual(rows, expected)
                    lines = [l for l in result.stdout.splitlines() if l.strip()]
                    self.assertGreaterEqual(len(lines), 2)
                    final_progress = lines[-2]
                    self.assertIn("Headers/XOVER" if overview else "Group/HEAD", final_progress)
                    self.assertIn("hdr/s" if overview else "art/s", final_progress)
                    self.assertIn("Elapsed", final_progress)
                    self.assertIn("ETA 00:00", final_progress)
                    self.assertIn("0/" + str(threads), final_progress)
                    self.assertIn("[#", final_progress)
                    self.assertIn("100.0%", final_progress)
                    self.assertTrue(lines[-1].startswith("Download complete:"))

    def test_parallel_head(self): self.run_case(False)
    def test_parallel_overview(self): self.run_case(True)
    def test_head_reconnect(self): self.run_case(False, "reconnect")
    def test_overview_reconnect(self): self.run_case(True, "reconnect")
    def test_head_exhaustion(self): self.run_case(False, "exhaust")
    def test_overview_exhaustion(self): self.run_case(True, "exhaust")
    def test_head_database_failure(self): self.run_case(False, "dbfail")
    def test_overview_database_failure(self): self.run_case(True, "dbfail")
    def test_overview_malformed(self): self.run_case(True, "malformed")
    def test_single_worker(self): self.run_case(True, threads=1)
    def test_small_parallel_batch(self): self.run_case(True, limit=8)
    def test_maximum_workers(self): self.run_case(False, threads=64)
    def test_duplicate_overview(self): self.run_case(True, "duplicate")
    def test_worker_connection_failure(self): self.run_case(True, "worker_failure")

    def test_skeleton_create_and_init_db(self):
        with tempfile.TemporaryDirectory() as directory:
            # 1. --skeleton-db with default db-type and db-name in cwd
            res = subprocess.run([BINARY, "--skeleton-db", "--group", "test.group", "--headers-only"],
                                 cwd=directory, capture_output=True, text=True, timeout=15)
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertIn("Skeleton database created for 'nntp2sql.db' (sqlite)", res.stdout)
            skel_db = pathlib.Path(directory) / "nntp2sql.db"
            self.assertTrue(skel_db.exists())
            with closing(sqlite3.connect(skel_db)) as con:
                settings = dict(con.execute("SELECT key, value FROM settings").fetchall())
                self.assertEqual(settings.get("schema_version"), "1")
                self.assertEqual(settings.get("group"), "test.group")
                self.assertEqual(settings.get("headers_only"), "1")
                groups = con.execute("SELECT name, article_count, first, last FROM groups").fetchall()
                self.assertEqual(groups, [("test.group", 0, 0, 0)])

            # Download into skeleton DB using its stored default group & headers_only (without --group or --upsert)
            with Server("normal") as server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    dl = subprocess.run(
                        [BINARY, "--host", "127.0.0.1", "--port", str(server.server_address[1]),
                         "--db-type", "sqlite", "--db-name", str(skel_db), "--limit", "10"],
                        capture_output=True, text=True, timeout=30
                    )
                finally:
                    server.shutdown()
                    thread.join()
                self.assertEqual(dl.returncode, 0, dl.stderr)
                with closing(sqlite3.connect(skel_db)) as con:
                    cnt = con.execute("SELECT COUNT(*) FROM articles WHERE group_name='test.group'").fetchone()[0]
                    self.assertGreater(cnt, 0)

            # 2. --create-db without --group
            created_db = pathlib.Path(directory) / "created.db"
            res2 = subprocess.run([BINARY, "--db-type", "sqlite", "--db-name", str(created_db), "--create-db"],
                                  capture_output=True, text=True, timeout=15)
            self.assertEqual(res2.returncode, 0, res2.stderr)
            self.assertIn("Database and schema created", res2.stdout)
            self.assertTrue(created_db.exists())

            # 3. --init-db standalone (without --group)
            init_db = pathlib.Path(directory) / "init.db"
            res3 = subprocess.run([BINARY, "--db-type", "sqlite", "--db-name", str(init_db), "--init-db"],
                                  capture_output=True, text=True, timeout=15)
            self.assertEqual(res3.returncode, 0, res3.stderr)
            self.assertIn("Database initialized", res3.stdout)
            self.assertTrue(init_db.exists())

    def test_input_validation_and_error_reporting(self):
        cases = [
            (["--unknown-flag"], 2, "unknown option '--unknown-flag'"),
            (["--host"], 2, "option '--host' requires a value"),
            (["--port", "abc"], 2, "invalid value 'abc' for --port"),
            (["--port", "70000"], 2, "invalid value '70000' for --port"),
            (["--threads", "0"], 2, "invalid value '0' for --threads"),
            (["--threads", "99"], 2, "invalid value '99' for --threads"),
            (["--retries", "-1"], 2, "invalid value '-1' for --retries"),
            (["--limit", "0"], 2, "invalid value '0' for --limit"),
            (["--progress-width", "2"], 2, "invalid value '2' for --progress-width"),
            (["--ssl", "--starttls", "--db-type", "sqlite", "--db-name", "x.db", "--group", "g"], 2, "cannot specify both --ssl and --starttls"),
            (["--user", "alice", "--db-type", "sqlite", "--db-name", "x.db", "--group", "g"], 2, "both --user and --pass must be provided"),
            (["--db-type", "oracle", "--db-name", "x.db", "--group", "g"], 2, "invalid --db-type 'oracle'"),
            (["--db-type", "sqlite", "--group", "g"], 2, "missing required option --db-name"),
            (["--conf", "/nonexistent/path/to/nntp2sql.conf"], 3, "configuration error"),
        ]
        for cli_args, expected_code, expected_err in cases:
            res = subprocess.run([BINARY] + cli_args, capture_output=True, text=True, timeout=10)
            self.assertEqual(res.returncode, expected_code, f"Failed for {cli_args}: {res.stderr}")
            self.assertIn(f"Error (code {expected_code})", res.stderr)
            self.assertIn(expected_err, res.stderr)

    def test_ctrl_c_shows_sql_records_updated_and_updates_db_asap(self):
        for overview in (True, False):
            with self.subTest(overview=overview):
                with tempfile.TemporaryDirectory() as directory:
                    db = pathlib.Path(directory) / "ctrlc.db"
                    with Server("slow_stream") as server:
                        thread = threading.Thread(target=server.serve_forever, daemon=True)
                        thread.start()
                        args = [BINARY, "--host", "127.0.0.1", "--port", str(server.server_address[1]),
                                "--db-type", "sqlite", "--db-name", str(db), "--group", "test.group",
                                "--upsert", "--threads", "2", "--retries", "0"]
                        if overview:
                            args.append("--headers-only")
                        proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                        try:
                            live_articles = 0
                            live_groups = 0
                            deadline = time.time() + 10.0
                            while time.time() < deadline:
                                if db.exists():
                                    try:
                                        with closing(sqlite3.connect(db, timeout=2.0)) as con:
                                            live_groups = con.execute("SELECT COUNT(*) FROM groups").fetchone()[0]
                                            live_articles = con.execute("SELECT COUNT(*) FROM articles").fetchone()[0]
                                    except sqlite3.Error:
                                        pass
                                if live_groups == 1 and live_articles >= 15:
                                    break
                                time.sleep(0.02)
                            self.assertEqual(live_groups, 1, "Group SQL record should be updated immediately")
                            self.assertGreaterEqual(live_articles, 15, "Article/header SQL records should be updated ASAP while downloading")
                            proc.send_signal(signal.SIGINT)
                            stdout, stderr = proc.communicate(timeout=10)
                        finally:
                            if proc.poll() is None:
                                proc.kill()
                            server.shutdown()
                            thread.join()

                        self.assertEqual(proc.returncode, 128 + int(signal.SIGINT), stderr)
                        self.assertIn("Interrupted (Ctrl+C)", stdout)
                        self.assertIn("Download interrupted (Ctrl+C):", stdout)
                        match = re.search(
                            r"Download interrupted \(Ctrl\+C\): (\d+) SQL records updated \((\d+) articles/headers stored, (\d+) group records updated, (\d+) missing\)",
                            stdout,
                        )
                        self.assertIsNotNone(match, f"Unexpected stdout: {stdout}")
                        sql_updated = int(match.group(1))
                        articles_stored = int(match.group(2))
                        groups_updated = int(match.group(3))
                        self.assertEqual(groups_updated, 1)
                        self.assertEqual(sql_updated, articles_stored + groups_updated)

                        with closing(sqlite3.connect(db)) as con:
                            final_groups = con.execute("SELECT COUNT(*) FROM groups").fetchone()[0]
                            final_articles = con.execute("SELECT COUNT(*) FROM articles").fetchone()[0]
                        self.assertEqual(final_groups, groups_updated)
                        self.assertEqual(final_articles, articles_stored)
                        self.assertGreaterEqual(final_articles, 15)

    def test_define_article_count_and_64bit_artnums(self):
        for flag in ("--articles", "--limit"):
            with self.subTest(flag=flag):
                with tempfile.TemporaryDirectory() as directory:
                    db = pathlib.Path(directory) / "high64.db"
                    with Server("high_artnum") as server:
                        thread = threading.Thread(target=server.serve_forever, daemon=True)
                        thread.start()
                        args = [
                            BINARY, "--host", "127.0.0.1", "--port", str(server.server_address[1]),
                            "--db-type", "sqlite", "--db-name", str(db), "--group", "test.group",
                            "--headers-only", "--upsert", "--threads", "8", flag, "120"
                        ]
                        try:
                            res = subprocess.run(args, capture_output=True, text=True, timeout=30)
                        finally:
                            server.shutdown()
                            thread.join()
                        self.assertEqual(res.returncode, 0, res.stderr)
                        self.assertIn("Download complete: 120 stored, 0 missing", res.stdout)
                        self.assertIn("ETA 00:00", res.stdout)
                        with closing(sqlite3.connect(db)) as con:
                            grp = con.execute("SELECT article_count, first, last FROM groups WHERE name='test.group'").fetchone()
                            self.assertEqual(grp, (600, 5000000001, 5000000600))
                            rows = con.execute("SELECT artnum FROM articles ORDER BY artnum").fetchall()
                            self.assertEqual(len(rows), 120)
                            self.assertTrue(all(r[0] > 5000000000 for r in rows))


if __name__ == "__main__":
    unittest.main()
