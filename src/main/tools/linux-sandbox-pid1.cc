// Copyright 2016 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * This is PID 1 inside the sandbox environment and runs in a separate user,
 * mount, UTS, IPC and PID namespace.
 */

#include "src/main/tools/linux-sandbox-pid1.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <math.h>
#include <mntent.h>
#include <net/if.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <string>

#ifndef MS_REC
// Some systems do not define MS_REC in sys/mount.h. We might be able to grab it
// from linux/fs.h instead (cf. #2667).
#include <linux/fs.h>
#endif

#include "src/main/tools/linux-sandbox-options.h"
#include "src/main/tools/linux-sandbox.h"
#include "src/main/tools/logging.h"
#include "src/main/tools/process-tools.h"

static std::string sandbox_root_dir = "";
static bool rootfs;

static bool IsDirectory(const char *path) {
  struct stat sb;
  if (stat(path, &sb) < 0) {
    DIE("stat(%s)", path);
  }
  return S_ISDIR(sb.st_mode);
}

// Recursively creates the file or directory specified in "path" and its parent
// directories.
static int CreateTarget(const char *path, bool is_directory) {
  PRINT_DEBUG("CreateTarget(%s, %s)", path, is_directory ? "true" : "false");
  if (path == NULL) {
    errno = EINVAL;
    return -1;
  }

  struct stat sb;
  // If the path already exists...
  if (stat(path, &sb) == 0) {
    if (is_directory && S_ISDIR(sb.st_mode)) {
      // and it's a directory and supposed to be a directory, we're done here.
      return 0;
    } else if (!is_directory && S_ISREG(sb.st_mode)) {
      // and it's a regular file and supposed to be one, we're done here.
      return 0;
    } else {
      // otherwise something is really wrong.
      errno = is_directory ? ENOTDIR : EEXIST;
      return -1;
    }
  } else {
    // If stat failed because of any error other than "the path does not exist",
    // this is an error.
    if (errno != ENOENT) {
      return -1;
    }
  }

  // Create the parent directory.
  if (CreateTarget(dirname(strdupa(path)), true) < 0) {
    DIE("CreateTarget(%s, true)", dirname(strdupa(path)));
  }

  if (is_directory) {
    if (mkdir(path, 0755) < 0) {
      DIE("mkdir(%s, 0755)", path);
    }
  } else {
    int handle;
    if ((handle = open(path, O_CREAT | O_WRONLY | O_EXCL, 0666)) < 0) {
      DIE("open(%s, O_CREAT | O_WRONLY | O_EXCL, 0666)", path);
    }
    if (close(handle) < 0) {
      DIE("close(%d)", handle);
    }
  }

  return 0;
}

static void SetupDevices() {
  if (!rootfs)
    return;
  CreateTarget("dev", true);
  const char *devs[] = {"/dev/null", "/dev/random", "/dev/urandom", "/dev/zero",
                        NULL};
  for (int i = 0; devs[i] != NULL; i++) {
    CreateTarget(devs[i] + 1, false);
    if (mount(devs[i], devs[i] + 1, NULL, MS_BIND, NULL) < 0) {
      DIE("mount(%s, %s, NULL, MS_BIND, NULL)", devs[i], devs[i] + 1);
    }
  }

  if (symlink("/proc/self/fd", "dev/fd") < 0) {
    DIE("symlink(/proc/self/fd, dev/fd)");
  }
}

static void CopyFile(const char *src, const char *dst) {
  int in_fd = open(src, O_RDONLY);
  if (in_fd < 0) {
    DIE("open(%s, O_RDONLY)", src);
  }
  int out_fd = open(dst, O_CREAT | O_WRONLY | O_EXCL, 0666);
  if (out_fd < 0) {
    DIE("open(%s, O_CREAT | O_WRONLY | O_EXCL, 0666)", dst);
  }

  char buf[8192];
  while (1) {
    ssize_t result = read(in_fd, &buf[0], sizeof(buf));
    if (!result) {
      break;
    }
    if (result < 0) {
      if (errno == EINTR) {
	continue;
      }
      DIE("read(in_fd, &buf[0], sizeof(buf)) for %s -> %s", src, dst);
    }
    if (write(out_fd, &buf[0], result) != result) {
      DIE("write(out_fd, &buf[0], result) for %s -> %s", src, dst);
    }
  }
  if (close(in_fd) < 0) {
    DIE("close(%s)", src);
  }
  if (close(out_fd) < 0) {
    DIE("close(%s)", dst);
  }
}

static int global_child_pid;

static void SetupSelfDestruction(int *sync_pipe) {
  // We could also poll() on the pipe fd to find out when the parent goes away,
  // and rely on SIGCHLD interrupting that otherwise. That might require us to
  // install some trivial handler for SIGCHLD. Using O_ASYNC to turn the pipe
  // close into SIGIO may also work. Another option is signalfd, although that's
  // almost as obscure as this prctl.
  if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) {
    DIE("prctl");
  }

  // Switch to a new process group, otherwise our process group will still refer
  // to the outer PID namespace. We might then accidentally kill our parent by a
  // call to e.g. `kill(0, sig)`.
  if (setpgid(0, 0) < 0) {
    DIE("setpgid");
  }

  // Verify that the parent still lives.
  char buf = 0;
  if (close(sync_pipe[0]) < 0) {
    DIE("close");
  }
  if (write(sync_pipe[1], &buf, 1) < 0) {
    DIE("write");
  }
  if (close(sync_pipe[1]) < 0) {
    DIE("close");
  }
}

static void SetupMountNamespace() {
  // Fully isolate our mount namespace private from outside events, so that
  // mounts in the outside environment do not affect our sandbox.
  if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) {
    DIE("mount");
  }
}

static void WriteFile(const std::string &filename, const char *fmt, ...) {
  FILE *stream = fopen(filename.c_str(), "w");
  if (stream == nullptr) {
    DIE("fopen(%s)", filename.c_str());
  }

  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf(stream, fmt, ap);
  va_end(ap);

  if (r < 0) {
    DIE("vfprintf");
  }

  if (fclose(stream) != 0) {
    DIE("fclose(%s)", filename.c_str());
  }
}

static void SetupUserNamespace() {
  // Disable needs for CAP_SETGID.
  struct stat sb;
  if (stat("/proc/self/setgroups", &sb) == 0) {
    WriteFile("/proc/self/setgroups", "deny");
  } else {
    // Ignore ENOENT, because older Linux versions do not have this file (but
    // also do not require writing to it).
    if (errno != ENOENT) {
      DIE("stat(/proc/self/setgroups");
    }
  }

  int inner_uid, inner_gid;
  if (opt.fake_root) {
    // Change our username to 'root'.
    inner_uid = 0;
    inner_gid = 0;
  } else if (opt.fake_username) {
    // Change our username to 'nobody'.
    struct passwd *pwd = getpwnam("nobody");
    if (pwd == nullptr) {
      DIE("unable to find passwd entry for user nobody")
    }

    inner_uid = pwd->pw_uid;
    inner_gid = pwd->pw_gid;
  } else {
    // Do not change the username inside the sandbox.
    inner_uid = global_outer_uid;
    inner_gid = global_outer_gid;
  }

  WriteFile("/proc/self/uid_map", "%d %d 1\n", inner_uid, global_outer_uid);
  WriteFile("/proc/self/gid_map", "%d %d 1\n", inner_gid, global_outer_gid);
}

static void SetupUtsNamespace() {
  if (sethostname("localhost", 9) < 0) {
    DIE("sethostname");
  }

  if (setdomainname("localdomain", 11) < 0) {
    DIE("setdomainname");
  }
}

static void MountFilesystems() {
  // Crappily detect if there's a rootfs.
  rootfs = std::find(opt.bind_mount_targets.begin(), opt.bind_mount_targets.end(), "/usr") != opt.bind_mount_targets.end();
  if (rootfs) {
    // working_dir is something like .../bazel-sandbox/[RANDOMNESS]/execroot/__main__. Place the root
    // two levels up.
    sandbox_root_dir = opt.working_dir.substr(0, opt.working_dir.find_last_of('/', opt.working_dir.find_last_of('/') - 1)) + "/root";
    PRINT_DEBUG("sandbox root dir: %s", sandbox_root_dir.c_str());
    CreateTarget(sandbox_root_dir.c_str(), true);
    if (mount(sandbox_root_dir.c_str(), sandbox_root_dir.c_str(), NULL, MS_BIND | MS_NOSUID, NULL) < 0) {
      DIE("mount(%s, %s, NULL, MS_BIND | MS_NOSUID, NULL)", sandbox_root_dir.c_str(), sandbox_root_dir.c_str());
    }
  }
  if (chdir((sandbox_root_dir + "/").c_str()) < 0) {
    DIE("chdir(%s)", sandbox_root_dir.c_str());
  }

  for (const std::string &tmpfs_dir : opt.tmpfs_dirs) {
    if (strncmp(tmpfs_dir.c_str(), sandbox_root_dir.c_str(), tmpfs_dir.size()) == 0) {
      // If sandbox root is under one of the tmpfs dir (e.g., when attempting to tmpfs mount
      // /dev/shm if sandbox root is /dev/shm/foobar)
      //
      // IMPORTANT:
      // - /dev/shm should be exclusive to the current sandbox. No changes to /dev/shm
      //   should leak out of the sandbox into the host system or other sandboxes.
      PRINT_DEBUG("tmpfs overlaps with working dir: %s", tmpfs_dir.c_str());

      // Using example tmpfs_dir = /dev/shm and working_dir = /dev/shm/bazel-sandbox/1234/execroot/__main__
      // Sandbox root = /dev/shm/bazel-sandbox/1234/root (PWD).
      // Create dev/shm in PWD and bind mount current /dev/shm into dev/shm.
      CreateTarget(tmpfs_dir.c_str() + 1, true);
      if (mount(tmpfs_dir.c_str(), tmpfs_dir.c_str() + 1, nullptr, MS_BIND,
                nullptr) < 0) {
        DIE("mount(%s, %s, nullptr, MS_BIND, nullptr)", tmpfs_dir.c_str(),
            tmpfs_dir.c_str() + 1);
      }

      // Mount empty tmpfs over /dev/shm
      PRINT_DEBUG("tmpfs: %s", tmpfs_dir.c_str());
      CreateTarget(tmpfs_dir.c_str(), true);
      if (mount("tmpfs", tmpfs_dir.c_str(), "tmpfs",
                MS_NOSUID | MS_NODEV | MS_NOATIME, nullptr) < 0) {
        DIE("mount(tmpfs, %s, tmpfs, MS_NOSUID | MS_NODEV | MS_NOATIME, nullptr)",
            tmpfs_dir.c_str());
      }

      // Create /dev/shm/bazel-sandbox/1234/execroot/__main__
      CreateTarget(opt.working_dir.c_str(), true);
      // Bind mount dev/shm/bazel-sandbox/1234/execroot/__main__ to /dev/shm/bazel-sandbox/1234/execroot/__main__
      // At this point, we have a reference to working dir in /dev/shm/bazel-sandbox/1234/execroot/__main__
      if (mount(opt.working_dir.c_str() + 1, opt.working_dir.c_str(), nullptr, MS_BIND,
                nullptr) < 0) {
        DIE("mount(%s, %s, nullptr, MS_BIND, nullptr)", opt.working_dir.c_str() + 1,
            opt.working_dir.c_str());
      }

      // PWD is still /dev/shm/bazel-sandbox/1234/root (in the old /dev/shm mount)
      // This needs to be maintained because we run `pivot_root` from this dir and `pivot_root`
      // requires old and new roots to be in the same mount point.
    } else {
      PRINT_DEBUG("tmpfs: %s", tmpfs_dir.c_str());
      CreateTarget(tmpfs_dir.c_str(), true);
      if (mount("tmpfs", tmpfs_dir.c_str(), "tmpfs",
            MS_NOSUID | MS_NODEV | MS_NOATIME, nullptr) < 0) {
        DIE("mount(tmpfs, %s, tmpfs, MS_NOSUID | MS_NODEV | MS_NOATIME, nullptr)",
            tmpfs_dir.c_str());
      }
    }
  }

  // Make sure that our working directory is a mount point. The easiest way to
  // do this is by bind-mounting it upon itself.
  PRINT_DEBUG("working dir: %s", opt.working_dir.c_str());

  CreateTarget(opt.working_dir.c_str() + 1, true);
  if (mount(opt.working_dir.c_str(), opt.working_dir.c_str() + 1, nullptr, MS_BIND,
            nullptr) < 0) {
    DIE("mount(%s, %s, nullptr, MS_BIND, nullptr)", opt.working_dir.c_str(),
        opt.working_dir.c_str() + 1);
  }

  for (size_t i = 0; i < opt.bind_mount_sources.size(); i++) {
    const std::string& source = opt.bind_mount_sources.at(i);
    const std::string& target = opt.bind_mount_targets.at(i);
    if (target == "/etc/hosts") {
      // HACK(): make /etc/hosts writable by making a copy of the file
      PRINT_DEBUG("copy: %s -> %s", source.c_str(), target.c_str() + 1);
      CopyFile(source.c_str(), target.c_str() + 1);
      continue;
    }
    CreateTarget(target.c_str() + 1, IsDirectory(source.c_str()));
    PRINT_DEBUG("bind mount: %s -> %s", source.c_str(), target.c_str());
    // rootfs: MS_REC is required for EC, which bind mounts things into the workspace.
    if (mount(source.c_str(), target.c_str() + 1, nullptr, MS_BIND | MS_REC, nullptr) < 0) {
      DIE("mount(%s, %s, nullptr, MS_BIND, nullptr)", source.c_str(),
          target.c_str());
    }
  }

  for (const std::string &writable_file : opt.writable_files) {
    PRINT_DEBUG("writable: %s", writable_file.c_str());
    CreateTarget(writable_file.c_str() + 1, IsDirectory(writable_file.c_str()));
    if (mount(writable_file.c_str(), writable_file.c_str() + 1, nullptr,
              MS_BIND | MS_REC, nullptr) < 0) {
      DIE("mount(%s, %s, nullptr, MS_BIND | MS_REC, nullptr)",
          writable_file.c_str(), writable_file.c_str() + 1);
    }
  }
}

// We later remount everything read-only, except the paths for which this method
// returns true.
static bool ShouldBeWritable(const std::string &mnt_dir_x) {
  if (mnt_dir_x == opt.working_dir) {
    return true;
  }

  if (mnt_dir_x.compare(0, sandbox_root_dir.size(), sandbox_root_dir)) {
    return false;
  }

  const std::string mnt_dir = mnt_dir_x.substr(sandbox_root_dir.size());

  if (mnt_dir == "") {
    return true;
  }

  for (const std::string &writable_file : opt.writable_files) {
    if (mnt_dir == writable_file) {
      return true;
    }
  }

  for (const std::string &tmpfs_dir : opt.tmpfs_dirs) {
    if (mnt_dir == tmpfs_dir) {
      return true;
    }
  }

  return false;
}

// Makes the whole filesystem read-only, except for the paths for which
// ShouldBeWritable returns true.
static void MakeFilesystemMostlyReadOnly() {
  FILE *mounts = setmntent("/proc/self/mounts", "r");
  if (mounts == nullptr) {
    DIE("setmntent");
  }

  struct mntent *ent;
  while ((ent = getmntent(mounts)) != nullptr) {
    // Skip things not under the sandbox root.
    if (strncmp(ent->mnt_dir, sandbox_root_dir.c_str(), sandbox_root_dir.size()))
      continue;

    int mountFlags = MS_BIND | MS_REMOUNT;

    // MS_REMOUNT does not allow us to change certain flags. This means, we have
    // to first read them out and then pass them in back again. There seems to
    // be no better way than this (an API for just getting the mount flags of a
    // mount entry as a bitmask would be great).
    if (hasmntopt(ent, "nodev") != nullptr) {
      mountFlags |= MS_NODEV;
    }
    if (hasmntopt(ent, "noexec") != nullptr) {
      mountFlags |= MS_NOEXEC;
    }
    if (hasmntopt(ent, "nosuid") != nullptr) {
      mountFlags |= MS_NOSUID;
    }
    if (hasmntopt(ent, "noatime") != nullptr) {
      mountFlags |= MS_NOATIME;
    }
    if (hasmntopt(ent, "nodiratime") != nullptr) {
      mountFlags |= MS_NODIRATIME;
    }
    if (hasmntopt(ent, "relatime") != nullptr) {
      mountFlags |= MS_RELATIME;
    }

    if (!ShouldBeWritable(ent->mnt_dir)) {
      mountFlags |= MS_RDONLY;
    }

    PRINT_DEBUG("remount %s: %s", (mountFlags & MS_RDONLY) ? "ro" : "rw",
                ent->mnt_dir);
    if (mount(nullptr, ent->mnt_dir, nullptr, mountFlags, nullptr) < 0) {
      // If we get EACCES or EPERM, this might be a mount-point for which we
      // don't have read access. Not much we can do about this, but it also
      // won't do any harm, so let's go on. The same goes for EINVAL or ENOENT,
      // which are fired in case a later mount overlaps an earlier mount, e.g.
      // consider the case of /proc, /proc/sys/fs/binfmt_misc and /proc, with
      // the latter /proc being the one that an outer sandbox has mounted on
      // top of its parent /proc. In that case, we're not allowed to remount
      // /proc/sys/fs/binfmt_misc, because it is hidden. If we get ESTALE, the
      // mount is a broken NFS mount. In the ideal case, the user would either
      // fix or remove that mount, but in cases where that's not possible, we
      // should just ignore it.
      if (errno != EACCES && errno != EPERM && errno != EINVAL &&
          errno != ENOENT && errno != ESTALE) {
        DIE("remount(nullptr, %s, nullptr, %d, nullptr)", ent->mnt_dir,
            mountFlags);
      }
    }
  }

  endmntent(mounts);
}

static void MountProc() {
  // Mount a new proc on top of the old one, because the old one still refers to
  // our parent PID namespace.
  CreateTarget("proc", true);
  if (mount("proc", "proc", "proc", MS_NODEV | MS_NOEXEC | MS_NOSUID,
            nullptr) < 0) {
    DIE("mount");
  }
}

static void SetupNetworking() {
  // When running in a separate network namespace, enable the loopback interface
  // because some application may want to use it.
  if (opt.create_netns) {
    int fd;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
      DIE("socket");
    }

    struct ifreq ifr = {};
    strncpy(ifr.ifr_name, "lo", IF_NAMESIZE);

    // Verify that name is valid.
    if (if_nametoindex(ifr.ifr_name) == 0) {
      DIE("if_nametoindex");
    }

    // Enable the interface.
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
      DIE("ioctl");
    }

    if (close(fd) < 0) {
      DIE("close");
    }
  }
}

static void EnterSandbox() {
  if (rootfs) {
    // Move the real root to old_root, then detach it.
    char old_root[] = "tmp/old-root-XXXXXX";
    if (mkdtemp(old_root) == NULL) {
      DIE("mkdtemp(%s)", old_root);
    }

    // pivot_root has no wrapper in libc, so we need syscall()
    if (syscall(SYS_pivot_root, ".", old_root) < 0) {
      DIE("pivot_root(., %s)", old_root);
    }

    if (chroot(".") < 0) {
      DIE("chroot(.)");
    }

    if (umount2(old_root, MNT_DETACH) < 0) {
      DIE("umount2(%s, MNT_DETACH)", old_root);
    }

    if (rmdir(old_root) < 0) {
      DIE("rmdir(%s)", old_root);
    }
  }

  if (chdir(opt.working_dir.c_str()) < 0) {
    DIE("chdir(%s)", opt.working_dir.c_str());
  }
}

// Reset the signal mask and restore the default handler for all signals.
static void RestoreSignalHandlersAndMask() {
  // Use an empty signal mask for the process (= unblock all signals).
  sigset_t empty_set;
  if (sigemptyset(&empty_set) < 0) {
    DIE("sigemptyset");
  }
  if (sigprocmask(SIG_SETMASK, &empty_set, nullptr) < 0) {
    DIE("sigprocmask(SIG_SETMASK, <empty set>, nullptr)");
  }

  // Set the default signal handler for all signals.
  struct sigaction sa = {};
  if (sigemptyset(&sa.sa_mask) < 0) {
    DIE("sigemptyset");
  }
  sa.sa_handler = SIG_DFL;
  for (int i = 1; i < NSIG; ++i) {
    // Ignore possible errors, because we might not be allowed to set the
    // handler for certain signals, but we still want to try.
    sigaction(i, &sa, nullptr);
  }
}

static void ForwardSignal(int signum) {
  PRINT_DEBUG("ForwardSignal(%d)", signum);
  kill(-global_child_pid, signum);
}

static void SetupSignalHandlers() {
  RestoreSignalHandlersAndMask();

  for (int signum = 1; signum < NSIG; signum++) {
    switch (signum) {
      // Some signals should indeed kill us and not be forwarded to the child,
      // thus we can use the default handler.
      case SIGABRT:
      case SIGBUS:
      case SIGFPE:
      case SIGILL:
      case SIGSEGV:
      case SIGSYS:
      case SIGTRAP:
        break;
      // It's fine to use the default handler for SIGCHLD, because we use
      // waitpid() in the main loop to wait for children to die anyway.
      case SIGCHLD:
        break;
      // One does not simply install a signal handler for these two signals
      case SIGKILL:
      case SIGSTOP:
        break;
      // Ignore SIGTTIN and SIGTTOU, as we hand off the terminal to the child in
      // SpawnChild().
      case SIGTTIN:
      case SIGTTOU:
        IgnoreSignal(signum);
        break;
      // All other signals should be forwarded to the child.
      default:
        InstallSignalHandler(signum, ForwardSignal);
        break;
    }
  }
}

static void SpawnChild() {
  global_child_pid = fork();

  if (global_child_pid < 0) {
    DIE("fork()");
  } else if (global_child_pid == 0) {
    // Put the child into its own process group.
    if (setpgid(0, 0) < 0) {
      DIE("setpgid");
    }

    // Try to assign our terminal to the child process.
    if (tcsetpgrp(STDIN_FILENO, getpgrp()) < 0 && errno != ENOTTY) {
      DIE("tcsetpgrp")
    }

    // Unblock all signals, restore default handlers.
    RestoreSignalHandlersAndMask();

    // Force umask to include read and execute for everyone, to make output
    // permissions predictable.
    umask(022);

    // argv[] passed to execve() must be a null-terminated array.
    opt.args.push_back(nullptr);

    if (execvp(opt.args[0], opt.args.data()) < 0) {
      DIE("execvp(%s, %p)", opt.args[0], opt.args.data());
    }
  }
}

static void WaitForChild() {
  while (1) {
    // Check for zombies to be reaped and exit, if our own child exited.
    int status;
    pid_t killed_pid = waitpid(-1, &status, 0);
    PRINT_DEBUG("waitpid returned %d", killed_pid);

    if (killed_pid < 0) {
      // Our PID1 process got a signal that interrupted the waitpid() call and
      // that was either ignored or forwared to the child. This is expected &
      // fine, just continue waiting.
      if (errno == EINTR) {
        continue;
      }
      DIE("waitpid")
    } else {
      if (killed_pid == global_child_pid) {
        // If the child process we spawned earlier terminated, we'll also
        // terminate. We can simply _exit() here, because the Linux kernel will
        // kindly SIGKILL all remaining processes in our PID namespace once we
        // exit.
        if (WIFSIGNALED(status)) {
          PRINT_DEBUG("child died due to signal %d", WTERMSIG(status));
          _exit(128 + WTERMSIG(status));
        } else {
          PRINT_DEBUG("child exited with code %d", WEXITSTATUS(status));
          _exit(WEXITSTATUS(status));
        }
      }
    }
  }
}

int Pid1Main(void *sync_pipe_param) {
  if (getpid() != 1) {
    DIE("Using PID namespaces, but we are not PID 1");
  }

  SetupSelfDestruction(reinterpret_cast<int *>(sync_pipe_param));
  SetupMountNamespace();
  SetupUserNamespace();
  if (opt.fake_hostname) {
    SetupUtsNamespace();
  }
  MountFilesystems();
  MountProc();
  SetupDevices();
  MakeFilesystemMostlyReadOnly();
  SetupNetworking();
  EnterSandbox();
  SetupSignalHandlers();
  SpawnChild();
  WaitForChild();
  _exit(EXIT_FAILURE);
}
