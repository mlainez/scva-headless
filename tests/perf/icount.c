/* SPDX-License-Identifier: CC0-1.0
 *
 * Runs a command and prints the user-space instructions it retired, which,
 * unlike time, barely moves between runs on one machine:
 *
 *   icount CMD ARGS...    prints "instructions N" on stderr, exits as CMD
 */
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
  struct perf_event_attr a;
  int go[2], status, fd;
  uint64_t n = 0;
  pid_t pid;
  char c;

  if (argc < 2) { fprintf(stderr, "usage: icount CMD...\n"); return 2; }
  if (pipe(go)) return 2;
  pid = fork();
  if (pid == 0) {                       /* wait until the counter is attached */
    close(go[1]);
    if (read(go[0], &c, 1) != 1) _exit(127);
    execvp(argv[1], argv + 1);
    _exit(127);
  }
  close(go[0]);
  memset(&a, 0, sizeof a);
  a.size = sizeof a;
  a.type = PERF_TYPE_HARDWARE;
  a.config = PERF_COUNT_HW_INSTRUCTIONS;
  a.disabled = 1;
  a.enable_on_exec = 1;
  a.exclude_kernel = 1;
  a.exclude_hv = 1;
  a.inherit = 1;
  fd = (int)syscall(SYS_perf_event_open, &a, pid, -1, -1, 0);
  if (fd < 0) { perror("perf_event_open"); kill(pid, 9); return 2; }
  if (write(go[1], "x", 1) != 1) return 2;
  waitpid(pid, &status, 0);
  if (read(fd, &n, sizeof n) != sizeof n) return 2;
  fprintf(stderr, "instructions %llu\n", (unsigned long long)n);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
