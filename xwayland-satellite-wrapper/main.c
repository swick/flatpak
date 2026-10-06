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
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static char *socket_path = NULL;
static pid_t child_pid = -1;

static void
cleanup (void)
{
  if (socket_path)
    unlink (socket_path);
}

static void
signal_handler (int sig)
{
  if (child_pid > 0)
    kill (child_pid, sig);
}

static int
create_listen_socket (const char *path)
{
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  int fd;

  if (strlen (path) >= sizeof (addr.sun_path))
    {
      fprintf (stderr, "Socket path too long: %s\n", path);
      return -1;
    }

  strncpy (addr.sun_path, path, sizeof (addr.sun_path) - 1);

  fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    {
      perror ("Failed to create socket");
      return -1;
    }

  unlink (path);

  if (bind (fd, (struct sockaddr *) &addr, sizeof (addr)) < 0)
    {
      perror ("Failed to bind socket");
      close (fd);
      return -1;
    }

  if (listen (fd, 128) < 0)
    {
      perror ("Failed to listen on socket");
      close (fd);
      unlink (path);
      return -1;
    }

  return fd;
}

int
main (int    argc,
      char **argv)
{
  const char *xwayland_satellite;
  const char *display;
  int sync_fd = -1;
  int listen_fd;
  int i;
  int wait_status;
  char fd_str[32];

  socket_path = NULL;
  display = NULL;

  for (i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--fd") == 0 && i + 1 < argc)
        {
          sync_fd = atoi (argv[++i]);
        }
      else if (socket_path == NULL)
        {
          socket_path = argv[i];
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

  if (socket_path == NULL || display == NULL)
    {
      fprintf (stderr, "Usage: %s SOCKET-PATH DISPLAY [--fd FD]\n", argv[0]);
      return 1;
    }

  xwayland_satellite = getenv ("FLATPAK_XWAYLAND_SATELLITE");
  if (xwayland_satellite == NULL)
    xwayland_satellite = "xwayland-satellite";

  atexit (cleanup);

  listen_fd = create_listen_socket (socket_path);
  if (listen_fd < 0)
    return 1;

  /* Signal readiness */
  if (sync_fd >= 0)
    {
      ssize_t written = write (sync_fd, "x", 1);
      if (written < 0)
        fprintf (stderr, "Warning: failed to write to sync fd\n");
    }

  child_pid = fork ();
  if (child_pid < 0)
    {
      perror ("Failed to fork");
      return 1;
    }

  if (child_pid == 0)
    {
      /* Clear CLOEXEC so the child inherits the listen fd */
      int flags = fcntl (listen_fd, F_GETFD);
      fcntl (listen_fd, F_SETFD, flags & ~FD_CLOEXEC);

      if (sync_fd >= 0)
        close (sync_fd);

      snprintf (fd_str, sizeof (fd_str), "%d", listen_fd);
      execlp (xwayland_satellite, xwayland_satellite,
              display, "-listenfd", fd_str, NULL);
      perror ("Failed to exec xwayland-satellite");
      _exit (127);
    }

  close (listen_fd);

  /* Forward termination signals */
  signal (SIGTERM, signal_handler);
  signal (SIGINT, signal_handler);

  if (sync_fd >= 0)
    {
      /* Watch for sync fd closure (app exited) in parallel with child */
      fd_set rfds;
      while (1)
        {
          int ret;
          FD_ZERO (&rfds);
          FD_SET (sync_fd, &rfds);

          ret = select (sync_fd + 1, &rfds, NULL, NULL, NULL);
          if (ret < 0)
            {
              if (errno == EINTR)
                {
                  /* Check if child exited due to signal forwarding */
                  if (waitpid (child_pid, &wait_status, WNOHANG) > 0)
                    return WIFEXITED (wait_status) ? WEXITSTATUS (wait_status) : 1;
                  continue;
                }
              break;
            }

          if (FD_ISSET (sync_fd, &rfds))
            {
              char buf;
              if (read (sync_fd, &buf, 1) <= 0)
                {
                  /* Sync fd closed — app exited */
                  kill (child_pid, SIGTERM);
                  break;
                }
            }
        }
    }

  waitpid (child_pid, &wait_status, 0);
  return WIFEXITED (wait_status) ? WEXITSTATUS (wait_status) : 1;
}
