#! /usr/bin/env python

import array
import io
import json
import os
import os.path
import secrets
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
                "cwd": "/",
                "env": [
                    "PATH=/bin:/usr/bin:/sbin:/usr/sbin",
                ],
            },
            "root": {
                "path": "/"
            }
        }

    def create(self, bundle_dir, terminal=False, expected_ret=0):
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
            stderr=subprocess.PIPE
        ) as proc:
            if terminal:
                conn, _ = sock.accept()
                msg, fds = recv_fds(conn, 512, 10)
                self.assertEqual(len(fds), 1)
                stdout = io.FileIO(fds[0], "w+")
                conn.close()
                sock.close()
            else:
                # Steal the pipes so that they don't get closed during
                # the call to proc.wait()
                stdout = proc.stdout
                stderr = proc.stderr
                proc.stdout = None
                proc.stderr = None
                ret = proc.wait()
                if ret != 0:
                    out = stderr.read().decode("utf-8")
                    print(f"failed with stderr: {out}")
                    self.assertTrue(ret == expected_ret)
                    return -1, ""
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

    def run_with_config(self, c, expected_ret=0):
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            with open(os.path.join("/tmp", "config.json"), "w") as f:
                json.dump(c, f)
            if "terminal" in c["process"]:
                terminal = c["process"]["terminal"]
            else:
                terminal = False
            pid, stdout = self.create(bundle_dir, terminal=terminal, expected_ret=expected_ret)
            if pid == -1:
                return 1, ""
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

    def test_validate_command_path(self):
        with tempfile.TemporaryDirectory() as root_dir:
            random_dir = secrets.token_urlsafe(8)
            shutil.copytree("/rescue", os.path.join(root_dir, random_dir))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = ["echo", "Hello World"]
            c["process"]["env"] = [f"PATH=/{random_dir}"]
            ret, out = self.run_with_config(c)
            self.assertEqual(ret, 0)
            self.assertEqual(out, "Hello World\n")

    def test_validate_command_abs(self):
        with tempfile.TemporaryDirectory() as root_dir:
            random_dir = secrets.token_urlsafe(8)
            shutil.copytree("/rescue", os.path.join(root_dir, random_dir))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = [f"/{random_dir}/echo", "Hello World"]
            ret, out = self.run_with_config(c)
            self.assertEqual(ret, 0)
            self.assertEqual(out, "Hello World\n")

    def test_tmpcopyup(self):
        # Running the container should copy the contents of /tmpdir to a
        # tmpfs which is mounted over it
        with tempfile.TemporaryDirectory() as root_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = ["cat", "/tmpdir/file"]
            c["process"]["env"] = ["PATH=/rescue"]
            c["mounts"] = [
                {
                    "type": "tmpfs",
                    "destination": "/tmpdir",
                    "options": ["tmpcopyup"],
                },
            ]
            os.mkdir(os.path.join(root_dir, "tmpdir"))
            with open(os.path.join(root_dir, "tmpdir", "file"), "w") as f:
                f.write("Hello World\n")
            ret, out = self.run_with_config(c)
            self.assertEqual(ret, 0)
            self.assertEqual(out, "Hello World\n")
            # Delete the container so that the tmpfs is unmounted
            # before we delete root_dir
            self.delete()

    # setup is a function which is called to initialise the root, destination is
    # the path inside the root for our mount and real_destination is the path
    # inside the root after resolving symlinks
    def symlink_test_helper(self, setup, destination, real_destination, expected_ret=0):
        # Test mounting to a path which goes via a symbolic link
        with tempfile.TemporaryDirectory() as root_dir:
            with tempfile.NamedTemporaryFile(mode="wb", buffering=0) as file_to_mount:
                shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
                file_to_mount.write(b"Hello World\n")
                setup(root_dir)

                c = self.config()
                c["root"]["path"] = root_dir
                c["process"]["args"] = ["cat", real_destination]
                c["process"]["env"] = ["PATH=/rescue"]
                c["mounts"] = [
                    {
                        "type": "nullfs",
                        "destination": destination,
                        "source": file_to_mount.name,
                    },
                ]
                ret, out = self.run_with_config(c, expected_ret)
                self.assertEqual(ret, expected_ret)
                if ret == 0:
                    self.assertEqual(out, "Hello World\n")
                # Delete the container so that the file mount is unmounted
                # before we delete root_dir
                self.delete()

    def test_mount_absolute_symlink(self):
        def setup(root_dir):
            # If mounting to /var/run/foo and /var/run is a symlink to /run, the
            # mount should target {root_dir}/run/foo
            os.mkdir(os.path.join(root_dir, "run"))
            os.mkdir(os.path.join(root_dir, "var"))
            os.symlink("/run", os.path.join(root_dir, "var/run"))
        self.symlink_test_helper(setup, "/var/run/foo", "/run/foo")

    def test_mount_relative_symlink(self):
        def setup(root_dir):
            # If mounting to /var/run/foo and /var/run is a symlink to somedir, the
            # mount should target {root_dir}/var/somedir/foo
            os.mkdir(os.path.join(root_dir, "var"))
            os.symlink("somedir", os.path.join(root_dir, "var/run"))
        self.symlink_test_helper(setup, "/var/run/foo", "/var/somedir/foo")

    def test_mount_symlink_parent(self):
        def setup(root_dir):
            # If mounting to /var/run/foo and /var/run is a symlink to "../somedir", the
            # mount should target {root_dir}/somedir/foo
            # 
            os.mkdir(os.path.join(root_dir, "somedir"))
            os.mkdir(os.path.join(root_dir, "var"))
            os.symlink("../somedir", os.path.join(root_dir, "var/run"))
        self.symlink_test_helper(setup, "/var/run/foo", "/somedir/foo")

    def test_mount_symlink_root_escape(self):
        def setup(root_dir):
            # Verify that its not possible for a mount to escape above the root
            # directory using ".."
            # 
            os.mkdir(os.path.join(root_dir, "somedir"))
            os.mkdir(os.path.join(root_dir, "var"))
            os.symlink("../../somedir", os.path.join(root_dir, "var/run"))
        self.symlink_test_helper(setup, "/var/run/foo", "/somedir/foo")

    def test_mount_recursive_symlink(self):
        def setup(root_dir):
            # Make sure that symlinks to symlinks are resolve correctly
            #
            os.mkdir(os.path.join(root_dir, "var"))
            os.symlink("somelink", os.path.join(root_dir, "var/run"))
            os.symlink("somedir", os.path.join(root_dir, "var/somelink"))
        self.symlink_test_helper(setup, "/var/run/foo", "/var/somedir/foo")

    def test_mount_symlink_loop(self):
        def setup(root_dir):
            # Symlink loops should cause an error
            #
            os.mkdir(os.path.join(root_dir, "var"))
            os.symlink("somelink", os.path.join(root_dir, "var/run"))
            os.symlink("somelink", os.path.join(root_dir, "var/somelink"))
        self.symlink_test_helper(setup, "/var/run/foo", "/var/somedir/foo", expected_ret=1)

if __name__ == "__main__":
    if os.getenv("OCIJAIL_PATH"):
        cmd = os.getenv("OCIJAIL_PATH")
    unittest.main()

