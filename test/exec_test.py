#! /usr/bin/env python

import json
import os
import os.path
import socket
import subprocess
import tempfile
import unittest

class test_exec(unittest.TestCase):
    "Parameter validation tests for exec"

    def process(self):
        return {
            "args": [
                "sh"
            ],
            "cwd": "/"
        };

    def run_(self, *args):
        args = ["ocijail/ocijail", "--testing=validation", "exec"] + list(args)
        return subprocess.run(args=args)

    def run_with_process(self, p, *args):
        with tempfile.TemporaryDirectory() as bundle_dir:
            process_path = os.path.join(bundle_dir, "process.json")
            with open(process_path, "w") as f:
                json.dump(p, f)
            return self.run_(*args, "--process", process_path, "my_id")

    def check_bad_process(self, p, *args):
        "Run a test with a bad proces"
        res = self.run_with_process(p, *args);
        self.assertTrue(res.returncode != 0)

    def check_good_process(self, p, *args):
        "Run a test with a good proces"
        res = self.run_with_process(p, *args);
        self.assertTrue(res.returncode == 0)

    def test_process_cwd(self):
        # process.cwd must be present with a string value
        p = self.process()
        del p["cwd"]
        self.check_bad_process(p)
        p["cwd"] = 42
        self.check_bad_process(p)
        p["cwd"] = "/tmp"
        self.check_good_process(p)

    def test_process_args(self):
        # process.args must be an array with at least one element since
        # we are not windows
        p = self.process()
        del p["args"]
        self.check_bad_process(p)
        p["args"] = 42
        self.check_bad_process(p)
        p["args"] = []
        self.check_bad_process(p)
        p["args"] = ["/bin/true"]
        self.check_good_process(p)

    def test_process_user(self):
        # if process.user is present, it must be an object
        p = self.process()
        p["user"] = 99
        self.check_bad_process(p)
        p["user"] = {}
        self.check_bad_process(p)
        # uid and gid must both be present and numeric
        p["user"] = {"uid": "user-name", "gid":123}
        self.check_bad_process(p)
        p["user"] = {"uid": 123, "gid": "group-name"}
        self.check_bad_process(p)
        p["user"] = {"uid": 123, "gid": 123}
        self.check_good_process(p)
        # if umask is present, it must be numeric
        p["user"]["umask"] = "bad"
        self.check_bad_process(p)
        p["user"]["umask"] = 63
        self.check_good_process(p)
        # if additionalGids is present, it must be an array of numeric
        p["user"]["additionalGids"] = "bad"
        self.check_bad_process(p)
        p["user"]["additionalGids"] = []
        self.check_good_process(p)
        p["user"]["additionalGids"] = ["bad"]
        self.check_bad_process(p)
        p["user"]["additionalGids"] = [99, 100]
        self.check_good_process(p)

    def test_process_env(self):
        # if process.env is present, it must be an array of strings
        p = self.process()
        p["env"] = "notarray"
        self.check_bad_process(p)
        p["env"] = [99]
        self.check_bad_process(p)
        p["env"] = ["TERM=xterm"]
        self.check_good_process(p)

    def test_console_socket(self):
        # --console-socket must be present if and only if process.terminal is true
        p = self.process()
        p["terminal"] = True

        # Should succeed: non-detached is allowed withput --console-socket
        self.check_good_process(p)

        # Should fail: terminal is true and --console-socket not present
        self.check_bad_process(p, "--detach")

        with tempfile.TemporaryDirectory() as sock_dir:
            # Should still fail if --console-socket doesn't refer to a socket
            sock_path = os.path.join(sock_dir, "sock") 
            open(sock_path, "w").close()
            self.check_bad_process(p, "--detach", "--console-socket", sock_path)

            # Should succeed
            os.remove(sock_path)
            s = socket.socket(socket.AF_UNIX)
            s.bind(sock_path)
            self.check_good_process(p, "--detach", "--console-socket", sock_path)

if __name__ == "__main__":
    unittest.main()
