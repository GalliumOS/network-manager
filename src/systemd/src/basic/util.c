/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "nm-sd-adapt.h"

#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include "alloc-util.h"
#include "btrfs-util.h"
#include "build.h"
#include "cgroup-util.h"
#include "def.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "hashmap.h"
#include "hostname-util.h"
#include "log.h"
#include "macro.h"
#include "missing.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "set.h"
#include "signal-util.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "time-util.h"
#include "umask-util.h"
#include "user-util.h"
#include "util.h"

#if 0 /* NM_IGNORED */
int saved_argc = 0;
char **saved_argv = NULL;
static int saved_in_initrd = -1;
#endif /* NM_IGNORED */

size_t page_size(void) {
        static thread_local size_t pgsz = 0;
        long r;

        if (_likely_(pgsz > 0))
                return pgsz;

        r = sysconf(_SC_PAGESIZE);
        assert(r > 0);

        pgsz = (size_t) r;
        return pgsz;
}

#if 0 /* NM_IGNORED */
bool plymouth_running(void) {
        return access("/run/plymouth/pid", F_OK) >= 0;
}

bool display_is_local(const char *display) {
        assert(display);

        return
                display[0] == ':' &&
                display[1] >= '0' &&
                display[1] <= '9';
}

int socket_from_display(const char *display, char **path) {
        size_t k;
        char *f, *c;

        assert(display);
        assert(path);

        if (!display_is_local(display))
                return -EINVAL;

        k = strspn(display+1, "0123456789");

        f = new(char, strlen("/tmp/.X11-unix/X") + k + 1);
        if (!f)
                return -ENOMEM;

        c = stpcpy(f, "/tmp/.X11-unix/X");
        memcpy(c, display+1, k);
        c[k] = 0;

        *path = f;

        return 0;
}

int block_get_whole_disk(dev_t d, dev_t *ret) {
        char *p, *s;
        int r;
        unsigned n, m;

        assert(ret);

        /* If it has a queue this is good enough for us */
        if (asprintf(&p, "/sys/dev/block/%u:%u/queue", major(d), minor(d)) < 0)
                return -ENOMEM;

        r = access(p, F_OK);
        free(p);

        if (r >= 0) {
                *ret = d;
                return 0;
        }

        /* If it is a partition find the originating device */
        if (asprintf(&p, "/sys/dev/block/%u:%u/partition", major(d), minor(d)) < 0)
                return -ENOMEM;

        r = access(p, F_OK);
        free(p);

        if (r < 0)
                return -ENOENT;

        /* Get parent dev_t */
        if (asprintf(&p, "/sys/dev/block/%u:%u/../dev", major(d), minor(d)) < 0)
                return -ENOMEM;

        r = read_one_line_file(p, &s);
        free(p);

        if (r < 0)
                return r;

        r = sscanf(s, "%u:%u", &m, &n);
        free(s);

        if (r != 2)
                return -EINVAL;

        /* Only return this if it is really good enough for us. */
        if (asprintf(&p, "/sys/dev/block/%u:%u/queue", m, n) < 0)
                return -ENOMEM;

        r = access(p, F_OK);
        free(p);

        if (r >= 0) {
                *ret = makedev(m, n);
                return 0;
        }

        return -ENOENT;
}

bool kexec_loaded(void) {
       _cleanup_free_ char *s = NULL;

       if (read_one_line_file("/sys/kernel/kexec_loaded", &s) < 0)
               return false;

       return s[0] == '1';
}

int prot_from_flags(int flags) {

        switch (flags & O_ACCMODE) {

        case O_RDONLY:
                return PROT_READ;

        case O_WRONLY:
                return PROT_WRITE;

        case O_RDWR:
                return PROT_READ|PROT_WRITE;

        default:
                return -EINVAL;
        }
}

int fork_agent(pid_t *pid, const int except[], unsigned n_except, const char *path, ...) {
        bool stdout_is_tty, stderr_is_tty;
        pid_t parent_pid, agent_pid;
        sigset_t ss, saved_ss;
        unsigned n, i;
        va_list ap;
        char **l;

        assert(pid);
        assert(path);

        /* Spawns a temporary TTY agent, making sure it goes away when
         * we go away */

        parent_pid = getpid_cached();

        /* First we temporarily block all signals, so that the new
         * child has them blocked initially. This way, we can be sure
         * that SIGTERMs are not lost we might send to the agent. */
        assert_se(sigfillset(&ss) >= 0);
        assert_se(sigprocmask(SIG_SETMASK, &ss, &saved_ss) >= 0);

        agent_pid = fork();
        if (agent_pid < 0) {
                assert_se(sigprocmask(SIG_SETMASK, &saved_ss, NULL) >= 0);
                return -errno;
        }

        if (agent_pid != 0) {
                assert_se(sigprocmask(SIG_SETMASK, &saved_ss, NULL) >= 0);
                *pid = agent_pid;
                return 0;
        }

        /* In the child:
         *
         * Make sure the agent goes away when the parent dies */
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0)
                _exit(EXIT_FAILURE);

        /* Make sure we actually can kill the agent, if we need to, in
         * case somebody invoked us from a shell script that trapped
         * SIGTERM or so... */
        (void) reset_all_signal_handlers();
        (void) reset_signal_mask();

        /* Check whether our parent died before we were able
         * to set the death signal and unblock the signals */
        if (getppid() != parent_pid)
                _exit(EXIT_SUCCESS);

        /* Don't leak fds to the agent */
        close_all_fds(except, n_except);

        stdout_is_tty = isatty(STDOUT_FILENO);
        stderr_is_tty = isatty(STDERR_FILENO);

        if (!stdout_is_tty || !stderr_is_tty) {
                int fd;

                /* Detach from stdout/stderr. and reopen
                 * /dev/tty for them. This is important to
                 * ensure that when systemctl is started via
                 * popen() or a similar call that expects to
                 * read EOF we actually do generate EOF and
                 * not delay this indefinitely by because we
                 * keep an unused copy of stdin around. */
                fd = open("/dev/tty", O_WRONLY);
                if (fd < 0) {
                        log_error_errno(errno, "Failed to open /dev/tty: %m");
                        _exit(EXIT_FAILURE);
                }

                if (!stdout_is_tty && dup2(fd, STDOUT_FILENO) < 0) {
                        log_error_errno(errno, "Failed to dup2 /dev/tty: %m");
                        _exit(EXIT_FAILURE);
                }

                if (!stderr_is_tty && dup2(fd, STDERR_FILENO) < 0) {
                        log_error_errno(errno, "Failed to dup2 /dev/tty: %m");
                        _exit(EXIT_FAILURE);
                }

                if (fd > STDERR_FILENO)
                        close(fd);
        }

        /* Count arguments */
        va_start(ap, path);
        for (n = 0; va_arg(ap, char*); n++)
                ;
        va_end(ap);

        /* Allocate strv */
        l = alloca(sizeof(char *) * (n + 1));

        /* Fill in arguments */
        va_start(ap, path);
        for (i = 0; i <= n; i++)
                l[i] = va_arg(ap, char*);
        va_end(ap);

        execv(path, l);
        _exit(EXIT_FAILURE);
}

bool in_initrd(void) {
        struct statfs s;

        if (saved_in_initrd >= 0)
                return saved_in_initrd;

        /* We make two checks here:
         *
         * 1. the flag file /etc/initrd-release must exist
         * 2. the root file system must be a memory file system
         *
         * The second check is extra paranoia, since misdetecting an
         * initrd can have bad consequences due the initrd
         * emptying when transititioning to the main systemd.
         */

        saved_in_initrd = access("/etc/initrd-release", F_OK) >= 0 &&
                          statfs("/", &s) >= 0 &&
                          is_temporary_fs(&s);

        return saved_in_initrd;
}

void in_initrd_force(bool value) {
        saved_in_initrd = value;
}

/* hey glibc, APIs with callbacks without a user pointer are so useless */
void *xbsearch_r(const void *key, const void *base, size_t nmemb, size_t size,
                 int (*compar) (const void *, const void *, void *), void *arg) {
        size_t l, u, idx;
        const void *p;
        int comparison;

        l = 0;
        u = nmemb;
        while (l < u) {
                idx = (l + u) / 2;
                p = (const char *) base + idx * size;
                comparison = compar(key, p, arg);
                if (comparison < 0)
                        u = idx;
                else if (comparison > 0)
                        l = idx + 1;
                else
                        return (void *)p;
        }
        return NULL;
}

int on_ac_power(void) {
        bool found_offline = false, found_online = false;
        _cleanup_closedir_ DIR *d = NULL;
        struct dirent *de;

        d = opendir("/sys/class/power_supply");
        if (!d)
                return errno == ENOENT ? true : -errno;

        FOREACH_DIRENT(de, d, return -errno) {
                _cleanup_close_ int fd = -1, device = -1;
                char contents[6];
                ssize_t n;

                device = openat(dirfd(d), de->d_name, O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOCTTY);
                if (device < 0) {
                        if (IN_SET(errno, ENOENT, ENOTDIR))
                                continue;

                        return -errno;
                }

                fd = openat(device, "type", O_RDONLY|O_CLOEXEC|O_NOCTTY);
                if (fd < 0) {
                        if (errno == ENOENT)
                                continue;

                        return -errno;
                }

                n = read(fd, contents, sizeof(contents));
                if (n < 0)
                        return -errno;

                if (n != 6 || memcmp(contents, "Mains\n", 6))
                        continue;

                safe_close(fd);
                fd = openat(device, "online", O_RDONLY|O_CLOEXEC|O_NOCTTY);
                if (fd < 0) {
                        if (errno == ENOENT)
                                continue;

                        return -errno;
                }

                n = read(fd, contents, sizeof(contents));
                if (n < 0)
                        return -errno;

                if (n != 2 || contents[1] != '\n')
                        return -EIO;

                if (contents[0] == '1') {
                        found_online = true;
                        break;
                } else if (contents[0] == '0')
                        found_offline = true;
                else
                        return -EIO;
        }

        return found_online || !found_offline;
}

int container_get_leader(const char *machine, pid_t *pid) {
        _cleanup_free_ char *s = NULL, *class = NULL;
        const char *p;
        pid_t leader;
        int r;

        assert(machine);
        assert(pid);

        if (!machine_name_is_valid(machine))
                return -EINVAL;

        p = strjoina("/run/systemd/machines/", machine);
        r = parse_env_file(p, NEWLINE, "LEADER", &s, "CLASS", &class, NULL);
        if (r == -ENOENT)
                return -EHOSTDOWN;
        if (r < 0)
                return r;
        if (!s)
                return -EIO;

        if (!streq_ptr(class, "container"))
                return -EIO;

        r = parse_pid(s, &leader);
        if (r < 0)
                return r;
        if (leader <= 1)
                return -EIO;

        *pid = leader;
        return 0;
}

int namespace_open(pid_t pid, int *pidns_fd, int *mntns_fd, int *netns_fd, int *userns_fd, int *root_fd) {
        _cleanup_close_ int pidnsfd = -1, mntnsfd = -1, netnsfd = -1, usernsfd = -1;
        int rfd = -1;

        assert(pid >= 0);

        if (mntns_fd) {
                const char *mntns;

                mntns = procfs_file_alloca(pid, "ns/mnt");
                mntnsfd = open(mntns, O_RDONLY|O_NOCTTY|O_CLOEXEC);
                if (mntnsfd < 0)
                        return -errno;
        }

        if (pidns_fd) {
                const char *pidns;

                pidns = procfs_file_alloca(pid, "ns/pid");
                pidnsfd = open(pidns, O_RDONLY|O_NOCTTY|O_CLOEXEC);
                if (pidnsfd < 0)
                        return -errno;
        }

        if (netns_fd) {
                const char *netns;

                netns = procfs_file_alloca(pid, "ns/net");
                netnsfd = open(netns, O_RDONLY|O_NOCTTY|O_CLOEXEC);
                if (netnsfd < 0)
                        return -errno;
        }

        if (userns_fd) {
                const char *userns;

                userns = procfs_file_alloca(pid, "ns/user");
                usernsfd = open(userns, O_RDONLY|O_NOCTTY|O_CLOEXEC);
                if (usernsfd < 0 && errno != ENOENT)
                        return -errno;
        }

        if (root_fd) {
                const char *root;

                root = procfs_file_alloca(pid, "root");
                rfd = open(root, O_RDONLY|O_NOCTTY|O_CLOEXEC|O_DIRECTORY);
                if (rfd < 0)
                        return -errno;
        }

        if (pidns_fd)
                *pidns_fd = pidnsfd;

        if (mntns_fd)
                *mntns_fd = mntnsfd;

        if (netns_fd)
                *netns_fd = netnsfd;

        if (userns_fd)
                *userns_fd = usernsfd;

        if (root_fd)
                *root_fd = rfd;

        pidnsfd = mntnsfd = netnsfd = usernsfd = -1;

        return 0;
}

int namespace_enter(int pidns_fd, int mntns_fd, int netns_fd, int userns_fd, int root_fd) {
        if (userns_fd >= 0) {
                /* Can't setns to your own userns, since then you could
                 * escalate from non-root to root in your own namespace, so
                 * check if namespaces equal before attempting to enter. */
                _cleanup_free_ char *userns_fd_path = NULL;
                int r;
                if (asprintf(&userns_fd_path, "/proc/self/fd/%d", userns_fd) < 0)
                        return -ENOMEM;

                r = files_same(userns_fd_path, "/proc/self/ns/user", 0);
                if (r < 0)
                        return r;
                if (r)
                        userns_fd = -1;
        }

        if (pidns_fd >= 0)
                if (setns(pidns_fd, CLONE_NEWPID) < 0)
                        return -errno;

        if (mntns_fd >= 0)
                if (setns(mntns_fd, CLONE_NEWNS) < 0)
                        return -errno;

        if (netns_fd >= 0)
                if (setns(netns_fd, CLONE_NEWNET) < 0)
                        return -errno;

        if (userns_fd >= 0)
                if (setns(userns_fd, CLONE_NEWUSER) < 0)
                        return -errno;

        if (root_fd >= 0) {
                if (fchdir(root_fd) < 0)
                        return -errno;

                if (chroot(".") < 0)
                        return -errno;
        }

        return reset_uid_gid();
}

uint64_t physical_memory(void) {
        _cleanup_free_ char *root = NULL, *value = NULL;
        uint64_t mem, lim;
        size_t ps;
        long sc;

        /* We return this as uint64_t in case we are running as 32bit process on a 64bit kernel with huge amounts of
         * memory.
         *
         * In order to support containers nicely that have a configured memory limit we'll take the minimum of the
         * physically reported amount of memory and the limit configured for the root cgroup, if there is any. */

        sc = sysconf(_SC_PHYS_PAGES);
        assert(sc > 0);

        ps = page_size();
        mem = (uint64_t) sc * (uint64_t) ps;

        if (cg_get_root_path(&root) < 0)
                return mem;

        if (cg_get_attribute("memory", root, "memory.limit_in_bytes", &value))
                return mem;

        if (safe_atou64(value, &lim) < 0)
                return mem;

        /* Make sure the limit is a multiple of our own page size */
        lim /= ps;
        lim *= ps;

        return MIN(mem, lim);
}

uint64_t physical_memory_scale(uint64_t v, uint64_t max) {
        uint64_t p, m, ps, r;

        assert(max > 0);

        /* Returns the physical memory size, multiplied by v divided by max. Returns UINT64_MAX on overflow. On success
         * the result is a multiple of the page size (rounds down). */

        ps = page_size();
        assert(ps > 0);

        p = physical_memory() / ps;
        assert(p > 0);

        m = p * v;
        if (m / p != v)
                return UINT64_MAX;

        m /= max;

        r = m * ps;
        if (r / ps != m)
                return UINT64_MAX;

        return r;
}

uint64_t system_tasks_max(void) {

#if SIZEOF_PID_T == 4
#define TASKS_MAX ((uint64_t) (INT32_MAX-1))
#elif SIZEOF_PID_T == 2
#define TASKS_MAX ((uint64_t) (INT16_MAX-1))
#else
#error "Unknown pid_t size"
#endif

        _cleanup_free_ char *value = NULL, *root = NULL;
        uint64_t a = TASKS_MAX, b = TASKS_MAX;

        /* Determine the maximum number of tasks that may run on this system. We check three sources to determine this
         * limit:
         *
         * a) the maximum value for the pid_t type
         * b) the cgroups pids_max attribute for the system
         * c) the kernel's configure maximum PID value
         *
         * And then pick the smallest of the three */

        if (read_one_line_file("/proc/sys/kernel/pid_max", &value) >= 0)
                (void) safe_atou64(value, &a);

        if (cg_get_root_path(&root) >= 0) {
                value = mfree(value);

                if (cg_get_attribute("pids", root, "pids.max", &value) >= 0)
                        (void) safe_atou64(value, &b);
        }

        return MIN3(TASKS_MAX,
                    a <= 0 ? TASKS_MAX : a,
                    b <= 0 ? TASKS_MAX : b);
}

uint64_t system_tasks_max_scale(uint64_t v, uint64_t max) {
        uint64_t t, m;

        assert(max > 0);

        /* Multiply the system's task value by the fraction v/max. Hence, if max==100 this calculates percentages
         * relative to the system's maximum number of tasks. Returns UINT64_MAX on overflow. */

        t = system_tasks_max();
        assert(t > 0);

        m = t * v;
        if (m / t != v) /* overflow? */
                return UINT64_MAX;

        return m / max;
}

int update_reboot_parameter_and_warn(const char *param) {
        int r;

        if (isempty(param)) {
                if (unlink("/run/systemd/reboot-param") < 0) {
                        if (errno == ENOENT)
                                return 0;

                        return log_warning_errno(errno, "Failed to unlink reboot parameter file: %m");
                }

                return 0;
        }

        RUN_WITH_UMASK(0022) {
                r = write_string_file("/run/systemd/reboot-param", param, WRITE_STRING_FILE_CREATE);
                if (r < 0)
                        return log_warning_errno(r, "Failed to write reboot parameter file: %m");
        }

        return 0;
}

int version(void) {
        puts(PACKAGE_STRING "\n"
             SYSTEMD_FEATURES);
        return 0;
}

int get_block_device(const char *path, dev_t *dev) {
        struct stat st;
        struct statfs sfs;

        assert(path);
        assert(dev);

        /* Get's the block device directly backing a file system. If
         * the block device is encrypted, returns the device mapper
         * block device. */

        if (lstat(path, &st))
                return -errno;

        if (major(st.st_dev) != 0) {
                *dev = st.st_dev;
                return 1;
        }

        if (statfs(path, &sfs) < 0)
                return -errno;

        if (F_TYPE_EQUAL(sfs.f_type, BTRFS_SUPER_MAGIC))
                return btrfs_get_block_device(path, dev);

        return 0;
}

int get_block_device_harder(const char *path, dev_t *dev) {
        _cleanup_closedir_ DIR *d = NULL;
        _cleanup_free_ char *p = NULL, *t = NULL;
        struct dirent *de, *found = NULL;
        const char *q;
        unsigned maj, min;
        dev_t dt;
        int r;

        assert(path);
        assert(dev);

        /* Gets the backing block device for a file system, and
         * handles LUKS encrypted file systems, looking for its
         * immediate parent, if there is one. */

        r = get_block_device(path, &dt);
        if (r <= 0)
                return r;

        if (asprintf(&p, "/sys/dev/block/%u:%u/slaves", major(dt), minor(dt)) < 0)
                return -ENOMEM;

        d = opendir(p);
        if (!d) {
                if (errno == ENOENT)
                        goto fallback;

                return -errno;
        }

        FOREACH_DIRENT_ALL(de, d, return -errno) {

                if (dot_or_dot_dot(de->d_name))
                        continue;

                if (!IN_SET(de->d_type, DT_LNK, DT_UNKNOWN))
                        continue;

                if (found) {
                        _cleanup_free_ char *u = NULL, *v = NULL, *a = NULL, *b = NULL;

                        /* We found a device backed by multiple other devices. We don't really support automatic
                         * discovery on such setups, with the exception of dm-verity partitions. In this case there are
                         * two backing devices: the data partition and the hash partition. We are fine with such
                         * setups, however, only if both partitions are on the same physical device. Hence, let's
                         * verify this. */

                        u = strjoin(p, "/", de->d_name, "/../dev");
                        if (!u)
                                return -ENOMEM;

                        v = strjoin(p, "/", found->d_name, "/../dev");
                        if (!v)
                                return -ENOMEM;

                        r = read_one_line_file(u, &a);
                        if (r < 0) {
                                log_debug_errno(r, "Failed to read %s: %m", u);
                                goto fallback;
                        }

                        r = read_one_line_file(v, &b);
                        if (r < 0) {
                                log_debug_errno(r, "Failed to read %s: %m", v);
                                goto fallback;
                        }

                        /* Check if the parent device is the same. If not, then the two backing devices are on
                         * different physical devices, and we don't support that. */
                        if (!streq(a, b))
                                goto fallback;
                }

                found = de;
        }

        if (!found)
                goto fallback;

        q = strjoina(p, "/", found->d_name, "/dev");

        r = read_one_line_file(q, &t);
        if (r == -ENOENT)
                goto fallback;
        if (r < 0)
                return r;

        if (sscanf(t, "%u:%u", &maj, &min) != 2)
                return -EINVAL;

        if (maj == 0)
                goto fallback;

        *dev = makedev(maj, min);
        return 1;

fallback:
        *dev = dt;
        return 1;
}
#endif /* NM_IGNORED */
