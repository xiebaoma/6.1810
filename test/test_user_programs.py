#!/usr/bin/env python3
import os
import re
import signal
import subprocess
import time
import unittest


class Xv6Session:
    def __init__(self, repo_root, boot_timeout=90):
        self.repo_root = repo_root
        self.boot_timeout = boot_timeout
        self.proc = None
        self.output = ""
        self._counter = 0

    def start(self):
        if self.proc is not None:
            return
        self.proc = subprocess.Popen(
            ["make", "qemu"],
            cwd=self.repo_root,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            preexec_fn=os.setsid,
        )
        self.wait_for(r"init: starting sh", timeout=self.boot_timeout)
        self.wait_for(r"\$ ", timeout=30)

    def stop(self):
        if self.proc is None:
            return
        try:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
        if self.proc.stdin is not None:
            self.proc.stdin.close()
        if self.proc.stdout is not None:
            self.proc.stdout.close()
        self.proc = None

    def _read_once(self):
        if self.proc is None or self.proc.stdout is None:
            return
        chunk = os.read(self.proc.stdout.fileno(), 4096)
        if not chunk:
            return
        self.output += chunk.decode("utf-8", errors="replace")

    def wait_for(self, pattern, timeout=20):
        deadline = time.time() + timeout
        compiled = re.compile(pattern, re.MULTILINE)
        while time.time() < deadline:
            if compiled.search(self.output):
                return
            self._read_once()
            time.sleep(0.05)
        raise AssertionError(f"Timeout waiting for pattern: {pattern}\n--- OUTPUT ---\n{self.output[-4000:]}")

    def sendline(self, line):
        if self.proc is None or self.proc.stdin is None:
            raise RuntimeError("xv6 session not started")
        self.proc.stdin.write((line + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def run_command(self, command, timeout=30):
        self._counter += 1
        start_tag = f"__UT_START_{self._counter}__"
        end_tag = f"__UT_END_{self._counter}__"
        before_len = len(self.output)

        self.sendline(f"echo {start_tag}; {command}; echo {end_tag}")
        self.wait_for(rf"(?m)^{re.escape(end_tag)}$", timeout=timeout)

        chunk = self.output[before_len:]
        start_match = re.search(rf"(?m)^{re.escape(start_tag)}$", chunk)
        if start_match is None:
            raise AssertionError(f"Start tag not found for: {command}\n{chunk[-2000:]}")
        end_match = re.search(rf"(?m)^{re.escape(end_tag)}$", chunk[start_match.end():])
        if end_match is None:
            raise AssertionError(f"Cannot delimit command output for: {command}\n{chunk[-2000:]}")
        end_pos = start_match.end() + end_match.start()
        return chunk[start_match.end():end_pos]


class TestUserProgramsSmoke(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        cls.sess = Xv6Session(cls.repo_root)
        cls.sess.start()

    @classmethod
    def tearDownClass(cls):
        cls.sess.stop()

    def assertContains(self, text, pattern, msg=None):
        if re.search(pattern, text, re.MULTILINE) is None:
            self.fail(msg or f"Pattern not found: {pattern}\n--- OUTPUT ---\n{text}")

    def test_boot_init(self):
        self.assertContains(self.sess.output, r"init: starting sh")

    def test_shell_builtin_and_pipeline(self):
        out = self.sess.run_command("echo shell-pipe | wc", timeout=20)
        self.assertContains(out, r"\b1\s+1\s+11\b")

        out = self.sess.run_command("echo left ; echo right", timeout=10)
        self.assertContains(out, r"left")
        self.assertContains(out, r"right")

    def test_files_and_basic_tools(self):
        self.sess.run_command("echo alpha beta > t_u_file", timeout=10)

        out = self.sess.run_command("cat t_u_file", timeout=10)
        self.assertContains(out, r"alpha beta")

        out = self.sess.run_command("wc t_u_file", timeout=10)
        self.assertContains(out, r"\b1\s+2\s+11\s+t_u_file\b")

        out = self.sess.run_command("grep alpha t_u_file", timeout=10)
        self.assertContains(out, r"alpha beta")

        self.sess.run_command("mkdir t_u_dir", timeout=10)
        out = self.sess.run_command("ls .", timeout=10)
        self.assertContains(out, r"t_u_dir")

        self.sess.run_command("ln t_u_file t_u_link", timeout=10)
        out = self.sess.run_command("cat t_u_link", timeout=10)
        self.assertContains(out, r"alpha beta")

        out = self.sess.run_command("find . t_u_link", timeout=10)
        self.assertContains(out, r"\./t_u_link")

        self.sess.run_command("echo 10 11 12 30 31 > t_u_nums", timeout=10)
        out = self.sess.run_command("sixfive t_u_nums", timeout=10)
        self.assertContains(out, r"(?m)^10$")
        self.assertContains(out, r"(?m)^12$")
        self.assertContains(out, r"(?m)^30$")

        out = self.sess.run_command("memdump", timeout=10)
        self.assertContains(out, r"Example 1:")
        self.assertContains(out, r"Example 5:")

        self.sess.run_command("rm t_u_file t_u_link t_u_nums", timeout=10)
        self.sess.run_command("rm t_u_dir", timeout=10)

    def test_syscall_stress_and_misc_programs(self):
        out = self.sess.run_command("uptime", timeout=10)
        self.assertContains(out, r"(?m)^\d+$")

        self.sess.run_command("sleep 1", timeout=10)

        out = self.sess.run_command("kill", timeout=10)
        self.assertContains(out, r"usage: kill pid")

        out = self.sess.run_command("forktest", timeout=30)
        self.assertContains(out, r"fork test OK")

        self.sess.run_command("zombie", timeout=15)

        out = self.sess.run_command("stressfs", timeout=60)
        self.assertContains(out, r"stressfs starting")
        self.assertContains(out, r"read")

        self.sess.run_command("logstress lf0 lf1", timeout=60)
        out = self.sess.run_command("ls .", timeout=10)
        self.assertContains(out, r"lf0")
        self.assertContains(out, r"lf1")
        self.sess.run_command("rm lf0 lf1", timeout=10)

    def test_sandbox_interpose(self):
        out = self.sess.run_command("sandbox 32768 - cat README", timeout=20)
        self.assertContains(out, r"cat: cannot open README")

        out = self.sess.run_command("sandbox 32768 README grep xv6 README", timeout=20)
        self.assertContains(out, r"xv6 is a re-implementation")

        out = self.sess.run_command("sandbox 32768 README grep xv6 x", timeout=20)
        self.assertContains(out, r"grep: cannot open x")

        # stressfs forks and opens files; blocked open in children verifies
        # that interpose restrictions are inherited across fork().
        self.sess.run_command("rm stressfs0 stressfs1 stressfs2 stressfs3", timeout=10)
        self.sess.run_command("sandbox 32768 - stressfs", timeout=30)
        out = self.sess.run_command("ls .", timeout=10)
        self.assertIsNone(re.search(r"\\bstressfs[0-3]\\b", out))

    def test_usertests_quick(self):
        out = self.sess.run_command("usertests -q", timeout=360)
        self.assertContains(out, r"ALL TESTS PASSED")


class TestLongRunningPrograms(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    def _run_until(self, cmd, pattern, timeout=45):
        sess = Xv6Session(self.repo_root)
        try:
            sess.start()
            sess.sendline(cmd)
            sess.wait_for(pattern, timeout=timeout)
        finally:
            sess.stop()

    def test_forphan_starts(self):
        self._run_until("forphan", r"wait for kill and reclaim", timeout=30)

    def test_dorphan_starts(self):
        self._run_until("dorphan", r"wait for kill and reclaim", timeout=30)

    def test_grind_runs(self):
        self._run_until("grind", r"[AB]", timeout=45)


if __name__ == "__main__":
    unittest.main(verbosity=2)
