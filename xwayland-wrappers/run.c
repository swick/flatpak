/*
 * Copyright © 2026 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define XWAYLAND_EXT_DIR "/usr/lib/extensions/xwayland"

static pid_t child_pid = -1;

static void
signal_handler (int sig)
{
  if (child_pid > 0)
    kill (-child_pid, sig);
}

static void
prepend_env (const char *var,
             const char *value)
{
  const char *old = getenv (var);

  if (old != NULL && *old != '\0')
    {
      char buf[4096];

      snprintf (buf, sizeof (buf), "%s:%s", value, old);
      setenv (var, buf, 1);
    }
  else
    {
      setenv (var, value, 1);
    }
}

static void
setup_extension_env (void)
{
  prepend_env ("PATH", XWAYLAND_EXT_DIR "/bin");
  prepend_env ("LD_LIBRARY_PATH", XWAYLAND_EXT_DIR "/lib");
  prepend_env ("PYTHONPATH", XWAYLAND_EXT_DIR "/lib/python3.14/site-packages");
}

static int
wait_for_socket (const char *path,
                 int         timeout_ms)
{
  int elapsed = 0;
  struct stat st;

  while (elapsed < timeout_ms)
    {
      if (stat (path, &st) == 0 && S_ISSOCK (st.st_mode))
        return 0;

      usleep (10000);
      elapsed += 10;
    }

  return -1;
}

int
main (int    argc,
      char **argv)
{
  const char *xwayland_run;
  const char *socket_dir = NULL;
  const char *display = NULL;
  int sync_fd = -1;
  int i;
  int wait_status;
  char socket_path[PATH_MAX];

  for (i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--fd") == 0 && i + 1 < argc)
        {
          sync_fd = atoi (argv[++i]);
        }
      else if (socket_dir == NULL)
        {
          socket_dir = argv[i];
        }
      else if (display == NULL)
        {
          display = argv[i];
        }
      else
        {
          fprintf (stderr, "Unexpected argument: %s\n", argv[i]);
          return 1;
        }
    }

  if (socket_dir == NULL || display == NULL)
    {
      fprintf (stderr, "Usage: %s SOCKET-DIR DISPLAY [--fd FD]\n", argv[0]);
      return 1;
    }

  if (display[0] != ':')
    {
      fprintf (stderr, "Display must start with ':'\n");
      return 1;
    }

  setup_extension_env ();

  xwayland_run = getenv ("FLATPAK_XWAYLAND_RUN");
  if (xwayland_run == NULL)
    xwayland_run = XWAYLAND_EXT_DIR "/bin/xwayland-run";

  snprintf (socket_path, sizeof (socket_path), "%s/X%s", socket_dir, display + 1);

  child_pid = fork ();
  if (child_pid < 0)
    {
      perror ("Failed to fork");
      return 1;
    }

  if (child_pid == 0)
    {
      setpgid (0, 0);

      if (sync_fd >= 0)
        close (sync_fd);

      execlp (xwayland_run, xwayland_run,
              "-ac", display, "--", "sleep", "infinity", NULL);
      perror ("Failed to exec xwayland-run");
      _exit (127);
    }

  setpgid (child_pid, child_pid);

  /* Forward termination signals */
  signal (SIGTERM, signal_handler);
  signal (SIGINT, signal_handler);

  if (wait_for_socket (socket_path, 10000) < 0)
    {
      fprintf (stderr, "Timed out waiting for X socket %s\n", socket_path);
      kill (-child_pid, SIGTERM);
      waitpid (child_pid, &wait_status, 0);
      return 1;
    }

  /* Signal readiness */
  if (sync_fd >= 0)
    {
      ssize_t written;

      written = write (sync_fd, "x", 1);
      if (written < 0)
        fprintf (stderr, "Warning: failed to write to sync fd\n");
    }

  if (sync_fd >= 0)
    {
      struct pollfd pfd = { .fd = sync_fd, .events = 0 };

      while (1)
        {
          int ret;

          ret = poll (&pfd, 1, -1);
          if (ret < 0)
            {
              if (errno == EINTR)
                {
                  if (waitpid (child_pid, &wait_status, WNOHANG) > 0)
                    return WIFEXITED (wait_status) ? WEXITSTATUS (wait_status) : 1;
                  continue;
                }
              break;
            }

          if (pfd.revents & (POLLERR | POLLHUP))
            {
              kill (-child_pid, SIGTERM);
              break;
            }
        }
    }

  waitpid (child_pid, &wait_status, 0);
  return WIFEXITED (wait_status) ? WEXITSTATUS (wait_status) : 1;
}
