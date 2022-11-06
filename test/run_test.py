#! /usr/bin/env python

import array
import io
import json
import os
import os.path
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest

cmd = "ocijail/ocijail"

def recv_fds(sock, msglen, maxfds):
    fds = array.array("i")   # Array of ints
    msg, ancdata, flags, addr = sock.recvmsg(msglen, socket.CMSG_LEN(maxfds * fds.itemsize))
    for cmsg_level, cmsg_type, cmsg_data in ancdata:
        if cmsg_level == socket.SOL_SOCKET and cmsg_type == socket.SCM_RIGHTS:
            # Append data, ignoring any truncated integers at the end.
            fds.frombytes(cmsg_data[:len(cmsg_data) - (len(cmsg_data) % fds.itemsize)])
    return msg, list(fds)

class test_run(unittest.TestCase):
    "End-to-end tests for running containers"

    count = 1

    def setUp(self):
        self.container_id = f"ocijail_test_{os.getpid()}_{test_run.count}"
        test_run.count += 1

    def tearDown(self):
        self.delete()

    def config(self):
        return {
            "ociVersion": "1.0.2",
            "process": {
                "cwd": "/"
            },
            "root": {
                "path": "/"
            }
        }

    def create(self, bundle_dir, terminal=False):
        pid_file = os.path.join(bundle_dir, "pid")
        args = [
            cmd,
            "create",
            "--pid-file", pid_file,
            "--bundle", bundle_dir,
        ]
        if terminal:
            console_socket = os.path.join(bundle_dir, "sock")
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.bind(console_socket)
            sock.listen()
            args += ["--console-socket", console_socket]
        args.append(self.container_id)
        with subprocess.Popen(
            args=args,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL
        ) as proc:
            if terminal:
                conn, _ = sock.accept()
                msg, fds = recv_fds(conn, 512, 10)
                self.assertEqual(len(fds), 1)
                stdout = io.FileIO(fds[0], "w+")
                conn.close()
                sock.close()
            else:
                # Steal the stdout so that it doesn't get closed during
                # the call to proc.wait()
                stdout = proc.stdout
                proc.stdout = None
            ret = proc.wait()
        self.assertTrue(ret == 0)
        with open(pid_file, "r") as f:
            pid = int(f.read())
        return pid, stdout

    def start(self):
        args = [cmd, "start", self.container_id]
        ret = subprocess.run(args=args)
        self.assertTrue(ret.returncode == 0)

    def delete(self):
        args = [cmd, "delete", self.container_id]
        ret = subprocess.run(args=args)
        #self.assertTrue(ret.returncode == 0)

    def run_with_config(self, c):
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            if "terminal" in c["process"]:
                terminal = c["process"]["terminal"]
            else:
                terminal = False
            pid, stdout = self.create(bundle_dir, terminal=terminal)
            self.start()
            out = stdout.read().decode("utf-8")
            stdout.close()
            pid2, status = os.waitpid(pid, os.WEXITED)
            self.assertTrue(pid == pid2)
            self.assertEqual(status & 0xff, 0)
            return status >> 8, out

    def test_exit_code(self):
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "exit 42"]
        ret, _ = self.run_with_config(c)
        self.assertEqual(ret, 42)

    def test_stdout(self):
        c = self.config()
        c["process"]["args"] = ["echo", "Hello", "World"]
        ret, out = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out, "Hello World\n")

    def test_stdout_tty(self):
        c = self.config()
        c["process"]["args"] = ["echo", "Hello", "World"]
        c["process"]["terminal"] = True
        ret, out = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out, "Hello World\r\n")

    def test_chroot(self):
        with tempfile.TemporaryDirectory() as root_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = ["sh", "-c", "echo Hello World > /file"]
            c["process"]["env"] = ["PATH=/rescue"]
            ret, _ = self.run_with_config(c)
            self.assertEqual(ret, 0)
            with open(os.path.join(root_dir, "file"), "r") as f:
                self.assertEqual(f.read(), "Hello World\n")

    def test_hook_create_runtme(self):
        with tempfile.TemporaryDirectory() as scratch:
            c = self.config()
            c["process"]["args"] = ["sh", "-c", f"exit $(cat {scratch}/file)"]
            c["hooks"] = {
                "createRuntime": [
                    {
                        "path": "/bin/sh",
                        "env": [
                            "foo=99"
                        ],
                        "args": [
                            "-c",
                            f"echo $foo > {scratch}/file"
                        ]
                    }
                ]
            }
            ret, _ = self.run_with_config(c)
            self.assertEqual(ret, 99)

    def test_hook_prestart(self):
        with tempfile.TemporaryDirectory() as scratch:
            c = self.config()
            c["process"]["args"] = ["sh", "-c", f"exit $(cat {scratch}/file)"]
            c["hooks"] = {
                "prestart": [
                    {
                        "path": "/bin/sh",
                        "args": [
                            "-c",
                            f"echo 99 > {scratch}/file"
                        ]
                    }
                ]
            }
            ret, _ = self.run_with_config(c)
            self.assertEqual(ret, 99)

    def test_hook_create_container(self):
        with tempfile.TemporaryDirectory() as root_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = ["sh", "-c", "exit $(cat /file)"]
            c["process"]["env"] = ["PATH=/rescue"]
            c["hooks"] = {
                "createContainer": [
                    {
                        "path": "/rescue/sh",
                        "args": [
                            "-c",
                            "echo 42 > file"
                        ]
                    }
                ]
            }
            ret, _ = self.run_with_config(c)
            self.assertEqual(ret, 42)

    def test_hook_start_container(self):
        with tempfile.TemporaryDirectory() as root_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = ["sh", "-c", "exit $(cat /file)"]
            c["process"]["env"] = ["PATH=/rescue"]
            c["hooks"] = {
                "startContainer": [
                    {
                        "path": "/rescue/sh",
                        "args": [
                            "-c",
                            "echo 42 > /file"
                        ]
                    }
                ]
            }
            ret, _ = self.run_with_config(c)
            self.assertEqual(ret, 42)

    def test_hook_poststop(self):
        with tempfile.TemporaryDirectory() as scratch:
            c = self.config()
            c["process"]["args"] = ["sh", "-c", f"exit $(cat {scratch}/file)"]
            c["hooks"] = {
                "createRuntime": [
                    {
                        "path": "/bin/sh",
                        "env": [
                            "foo=123"
                        ],
                        "args": [
                            "-c",
                            f"echo $foo > {scratch}/file"
                        ]
                    }
                ],
                "poststop": [
                    {
                        "path": "/bin/rm",
                        "args": [
                            f"{scratch}/file"
                        ]
                    }
                ]
            }
            ret, _ = self.run_with_config(c)
            self.assertEqual(ret, 123)
            self.assertTrue(os.path.exists(f"{scratch}/file"))
            self.delete()
            self.assertFalse(os.path.exists(f"{scratch}/file"))

if __name__ == "__main__":
    if os.getenv("OCIJAIL_PATH"):
        cmd = os.getenv("OCIJAIL_PATH")
    unittest.main()

