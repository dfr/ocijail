#! /usr/bin/env python

import json
import os
import os.path
import socket
import subprocess
import tempfile
import unittest

class test_create(unittest.TestCase):
    "Parameter validation tests for create"

    def config(self):
        return {
            "ociVersion": "1.0.2",
            "process": {
                "args": [
                    "sh"
                ],
                "cwd": "/",
            },
            "root": {
                "path": "/tmp"
            }
        }

    def create(self, *args):
        args = ["ocijail/ocijail", "--testing=validation", "create"] + list(args)
        return subprocess.run(args=args)

    def run_with_config(self, c, *args, pre_check=None):
        with tempfile.TemporaryDirectory() as bundle_dir:
            if pre_check:
                pre_check(bundle_dir)
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            return self.create(*args, "--bundle", bundle_dir, "my_id")

    def check_bad_config(self, c, *args, pre_check=None):
        "Run a test with a bad config"
        res = self.run_with_config(c, *args, pre_check = pre_check)
        self.assertTrue(res.returncode != 0)

    def check_good_config(self, c, *args, pre_check=None):
        "Run a test with a good config"
        res = self.run_with_config(c, *args, pre_check = pre_check)
        self.assertTrue(res.returncode == 0)

    def test_oci_version(self):
        # ociVersion must be present
        c = self.config()
        del c["ociVersion"]
        self.check_bad_config(c)

        # ociVersion must be 1.0.x or 1.1.x
        c = self.config()
        c["ociVersion"] = "1.0.0"
        self.check_good_config(c)
        c["ociVersion"] = "1.1.0"
        self.check_good_config(c)
        c["ociVersion"] = "1.1.0-rc.2"
        self.check_good_config(c)
        c["ociVersion"] = "1.2.0"
        self.check_bad_config(c)

    def test_process(self):
        # process must be present
        c = self.config()
        del c["process"]
        self.check_bad_config(c)

    def test_process_cwd(self):
        # process.cwd must be present with a string value
        c = self.config()
        del c["process"]["cwd"]
        self.check_bad_config(c)
        c["process"]["cwd"] = 42
        self.check_bad_config(c)

    def test_process_args(self):
        # process.args must be an array with at least one element since
        # we are not windows
        c = self.config()
        del c["process"]["args"]
        self.check_bad_config(c)
        c["process"]["args"] = 42
        self.check_bad_config(c)
        c["process"]["args"] = []
        self.check_bad_config(c)
        c["process"]["args"] = ["/bin/true"]
        self.check_good_config(c)

    def test_process_user(self):
        # if process.user is present, it must be an object
        c = self.config()
        c["process"]["user"] = 99
        self.check_bad_config(c)
        c["process"]["user"] = {}
        self.check_bad_config(c)
        # uid and gid must both be present and numeric
        c["process"]["user"] = {"uid": "user-name", "gid":123}
        self.check_bad_config(c)
        c["process"]["user"] = {"uid": 123, "gid": "group-name"}
        self.check_bad_config(c)
        c["process"]["user"] = {"uid": 123, "gid": 123}
        self.check_good_config(c)
        # if umask is present, it must be numeric
        c["process"]["user"]["umask"] = "bad"
        self.check_bad_config(c)
        c["process"]["user"]["umask"] = 63
        self.check_good_config(c)
        # if additionalGids is present, it must be an array of numeric
        c["process"]["user"]["additionalGids"] = "bad"
        self.check_bad_config(c)
        c["process"]["user"]["additionalGids"] = []
        self.check_good_config(c)
        c["process"]["user"]["additionalGids"] = ["bad"]
        self.check_bad_config(c)
        c["process"]["user"]["additionalGids"] = [99, 100]
        self.check_good_config(c)

    def test_process_env(self):
        # if process.env is present, it must be an array of strings,
        # each of which must be <key>=<value>
        c = self.config()
        c["process"]["env"] = "notarray"
        self.check_bad_config(c)
        c["process"]["env"] = [99]
        self.check_bad_config(c)
        c["process"]["env"] = ["TERM=xterm"]
        self.check_good_config(c)

    def test_console_socket(self):
        # --console-socket must be present if and only if process.terminal is true
        c = self.config()
        c["process"]["terminal"] = True

        # Should fail: terminal is true and --console-socket not present
        self.check_bad_config(c)

        with tempfile.TemporaryDirectory() as sock_dir:
            # Should still fail if --console-socket doesn't refer to a socket
            sock_path = os.path.join(sock_dir, "sock") 
            open(sock_path, "w").close()
            self.check_bad_config(c, "--console-socket", sock_path)

            # Should succeed
            os.remove(sock_path)
            s = socket.socket(socket.AF_UNIX)
            s.bind(sock_path)
            self.check_good_config(c, "--console-socket", sock_path)

    def test_root_directory(self):
        # If there is no root in the config, there must be a root
        # sub-directory of the bundle
        c = self.config()
        del c["root"]
        self.check_bad_config(c)
        self.check_good_config(
            c,
            pre_check=lambda dir: os.mkdir(os.path.join(dir, "root"))
        )

    def test_mounts(self):
        # if present, mounts must be an array of objects
        c = self.config()
        c["mounts"] = "broken"
        self.check_bad_config(c)
        c["mounts"] = []
        self.check_good_config(c)
        c["mounts"] = ["broken"]
        self.check_bad_config(c)

        # Mount entries must have a string-valued destination field
        c["mounts"] = [{}]
        self.check_bad_config(c)
        c["mounts"] = [{"destination": 42}]
        self.check_bad_config(c)
        c["mounts"] = [{"destination": "/tmp"}]
        self.check_good_config(c)

        # If present, mount entry source field must have a string value
        c["mounts"] = [{"destination": "/tmp", "source": 42}]
        self.check_bad_config(c)
        c["mounts"] = [{"destination": "/tmp", "source": "/somewhere"}]
        self.check_good_config(c)

        # If present, mount entry type field must have a string value
        c["mounts"] = [{"destination": "/tmp", "type": 42}]
        self.check_bad_config(c)
        c["mounts"] = [{"destination": "/tmp", "type": "nullfs"}]
        self.check_good_config(c)

        # If present, mount entry options field must be an array of strings
        c["mounts"] = [{"destination": "/tmp", "options": 42}]
        self.check_bad_config(c)
        c["mounts"] = [{"destination": "/tmp", "options": []}]
        self.check_good_config(c)
        c["mounts"] = [{"destination": "/tmp", "options": [42]}]
        self.check_bad_config(c)
        c["mounts"] = [{"destination": "/tmp", "options": ["opt1","opt2","opt3=42"]}]
        self.check_good_config(c)

    def _test_hooks_sub(self, stage):
        # if present, each hook stage must be an array
        c = self.config()
        c["hooks"] = {stage: "broken"}
        self.check_bad_config(c)
        c["hooks"] = {stage: []}
        self.check_good_config(c)

        # each element of the array must be an object with a path
        # attribute
        c["hooks"] = {stage: ["broken"]}
        self.check_bad_config(c)
        c["hooks"] = {stage: [[]]}
        self.check_bad_config(c)
        c["hooks"] = {stage: [{"path": "/nonexistent/hook"}]}
        self.check_good_config(c)

        # if present, args must be an array of strings
        c["hooks"] = {stage: [
            {
                "path": "/nonexistent/hook",
                "args": "broken"
            }
        ]}
        self.check_bad_config(c)
        c["hooks"] = {stage: [
            {
                "path": "/nonexistent/hook",
                "args": [42]
            }
        ]}
        self.check_bad_config(c)
        c["hooks"] = {stage: [
            {
                "path": "/nonexistent/hook",
                "args": ["arg1"]
            }
        ]}
        self.check_good_config(c)

        # if present, env must be an array of strings
        c["hooks"] = {stage: [
            {
                "path": "/nonexistent/hook",
                "env": "broken"
            }
        ]}
        self.check_bad_config(c)
        c["hooks"] = {stage: [
            {
                "path": "/nonexistent/hook",
                "env": [42]
            }
        ]}
        self.check_bad_config(c)
        c["hooks"] = {stage: [
            {
                "path": "/nonexistent/hook",
                "env": ["env1"]
            }
        ]}
        self.check_good_config(c)

        # if present, timeout must be a number
        c["hooks"] = {stage: [
            {
                "path": "/nonexistent/hook",
                "timeout": "broken"
            }
        ]}
        self.check_bad_config(c)
        c["hooks"] = {stage: [
            {
                "path": "/nonexistent/hook",
                "timeout": []
            }
        ]}
        self.check_bad_config(c)
        c["hooks"] = {stage: [
            {
                "path": "/nonexistent/hook",
                "timeout": {}
            }
        ]}
        self.check_bad_config(c)
        c["hooks"] = {stage: [
            {
                "path": "/nonexistent/hook",
                "timeout": 42
            }
        ]}
        self.check_good_config(c)

    def test_hooks(self):
        # if present, hooks must be an object
        c = self.config()
        c["hooks"] = "broken"
        self.check_bad_config(c)
        c["hooks"] = {}
        self.check_good_config(c)

        self._test_hooks_sub("prestart")
        self._test_hooks_sub("createRuntime")
        self._test_hooks_sub("createContainer")
        self._test_hooks_sub("startContainer")
        self._test_hooks_sub("poststart")
        self._test_hooks_sub("poststop")
        

if __name__ == "__main__":
    unittest.main()
