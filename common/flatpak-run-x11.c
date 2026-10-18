/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2014-2019 Red Hat, Inc
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
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"
#include "flatpak-run-x11-private.h"

#include <glib/gi18n-lib.h>
#include <sys/utsname.h>

#ifdef ENABLE_XAUTH
#include <X11/Xauth.h>
#endif

#include "flatpak-run-wayland-private.h"
#include "flatpak-utils-private.h"

/* This is part of the X11 protocol, so we can safely hard-code it here */
#define FamilyInternet6 (6)

#ifdef ENABLE_XAUTH
static gboolean
auth_streq (const char *str,
            const char *au_str,
            size_t      au_len)
{
  return au_len == strlen (str) && memcmp (str, au_str, au_len) == 0;
}

static gboolean
xauth_entry_should_propagate (const Xauth *xa,
                              int          family,
                              const char  *remote_hostname,
                              const char  *local_hostname,
                              const char  *number)
{
  /* ensure entry isn't for a different type of access */
  if (family != FamilyWild && xa->family != family && xa->family != FamilyWild)
    return FALSE;

  /* ensure entry isn't for remote access, except that if remote_hostname
   * is specified, then remote access to that hostname is OK */
  if (xa->family != FamilyWild && xa->family != FamilyLocal &&
      (remote_hostname == NULL ||
       !auth_streq (remote_hostname, xa->address, xa->address_length)))
    return FALSE;

  /* ensure entry is for this machine */
  if (xa->family == FamilyLocal && !auth_streq (local_hostname, xa->address, xa->address_length))
    {
      /* OpenSUSE inherits the hostname value from DHCP without updating
       * its X11 authentication cookie. The old hostname value can still
       * be found in the environment variable XAUTHLOCALHOSTNAME.
       * For reference:
       * https://bugzilla.opensuse.org/show_bug.cgi?id=262309
       * For this reason if we have a cookie whose address is equal to the
       * variable XAUTHLOCALHOSTNAME, we still need to propagate it, but
       * we also need to change its address to `unames.nodename`.
       */
      const char *xauth_local_hostname;
      xauth_local_hostname = g_getenv ("XAUTHLOCALHOSTNAME");
      if (xauth_local_hostname == NULL)
        return FALSE;

      if (!auth_streq ((char *) xauth_local_hostname, xa->address, xa->address_length))
        return FALSE;
    }

  /* ensure entry is for this session */
  if (xa->number != NULL && !auth_streq (number, xa->number, xa->number_length))
    return FALSE;

  return TRUE;
}

static void
write_xauth (int family,
             const char *remote_host,
             const char *number,
             FILE       *output)
{
  Xauth *xa, local_xa;
  char *filename;
  FILE *f;
  struct utsname unames;

  if (uname (&unames))
    {
      g_warning ("uname failed");
      return;
    }

  filename = XauFileName ();
  f = fopen (filename, "rb");
  if (f == NULL)
    return;

  while (TRUE)
    {
      xa = XauReadAuth (f);
      if (xa == NULL)
        break;
      if (xauth_entry_should_propagate (xa, family, remote_host,
                                        unames.nodename, number))
        {
          local_xa = *xa;

          if (local_xa.family == FamilyLocal &&
              !auth_streq (unames.nodename, local_xa.address, local_xa.address_length))
            {
              /* If we decided to propagate this cookie, but its address
               * doesn't match `unames.nodename`, we need to change it or
               * inside the container it will not work.
               */
              local_xa.address = unames.nodename;
              local_xa.address_length = strlen (local_xa.address);
            }

          if (!XauWriteAuth (output, &local_xa))
            g_warning ("xauth write error");
        }

      XauDisposeAuth (xa);
    }

  fclose (f);
}
#else /* !ENABLE_XAUTH */

/* When not doing Xauth, any distinct values will do, but use the same
 * ones Xauth does so that we can refer to them in our unit test. */
#define FamilyLocal (256)
#define FamilyWild (65535)

#endif /* !ENABLE_XAUTH */

/*
 * @family: (out) (not optional):
 * @x11_socket: (out) (not optional):
 * @display_nr_out: (out) (not optional):
 */
gboolean
flatpak_run_parse_x11_display (const char  *display,
                               int         *family,
                               char       **x11_socket,
                               char       **remote_host,
                               char       **display_nr_out,
                               GError     **error)
{
  const char *colon;
  const char *display_nr;
  const char *display_nr_end;

  /* Use the last ':', not the first, to cope with [::1]:0 */
  colon = strrchr (display, ':');

  if (colon == NULL)
    return glnx_throw (error, "No colon found in DISPLAY=%s", display);

  if (!g_ascii_isdigit (colon[1]))
    return glnx_throw (error, "Colon not followed by a digit in DISPLAY=%s", display);

  display_nr = &colon[1];
  display_nr_end = display_nr;

  while (g_ascii_isdigit (*display_nr_end))
    display_nr_end++;

  *display_nr_out = g_strndup (display_nr, display_nr_end - display_nr);

  if (display == colon || g_str_has_prefix (display, "unix:"))
    {
      *family = FamilyLocal;
      *x11_socket = g_strdup_printf ("/tmp/.X11-unix/X%s", *display_nr_out);
    }
  else if (display[0] == '[' && display[colon - display - 1] == ']')
    {
      *family = FamilyInternet6;
      *remote_host = g_strndup (display + 1, colon - display - 2);
    }
  else
    {
      *family = FamilyWild;
      *remote_host = g_strndup (display, colon - display);
    }

  return TRUE;
}

static char *
create_proxy_socket_path (void)
{
  g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();
  g_autofree char *socket_dir = g_build_filename (user_runtime_dir, ".xwayland-satellite", NULL);
  g_autofree char *socket_path = g_build_filename (socket_dir, "x11-XXXXXX", NULL);
  int fd;

  if (!glnx_shutil_mkdir_p_at (AT_FDCWD, socket_dir, 0755, NULL, NULL))
    return NULL;

  fd = g_mkstemp (socket_path);
  if (fd < 0)
    return NULL;

  close (fd);

  return g_steal_pointer (&socket_path);
}

static gboolean
start_xwayland_satellite (FlatpakBwrap  *app_bwrap,
                          const char    *app_info_path,
                          GError       **error)
{
  g_autofree char *socket_path = NULL;
  g_autofree char *socket_dir = NULL;
  g_autofree char *commandline = NULL;
  g_autoptr(FlatpakBwrap) wrapper_bwrap = NULL;
  int sync_fd_app = -1;
  g_autofd int sync_fd_wrapper = -1;
  char x;

  socket_path = create_proxy_socket_path ();
  if (socket_path == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Failed to create xwayland-satellite socket path"));
      return FALSE;
    }

  socket_dir = g_path_get_dirname (socket_path);

  flatpak_bwrap_set_env (app_bwrap, "DISPLAY", ":99", TRUE);
  flatpak_bwrap_unset_env (app_bwrap, "XAUTHORITY");
  flatpak_bwrap_add_args (app_bwrap,
                          "--ro-bind", socket_path, "/tmp/.X11-unix/X99",
                          NULL);

  if (!flatpak_bwrap_add_sync_fd (app_bwrap, &sync_fd_wrapper, &sync_fd_app))
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Unable to create sync pipe"));
      return FALSE;
    }

  wrapper_bwrap = flatpak_bwrap_new (NULL);
  if (!flatpak_bwrap_add_wrapper (wrapper_bwrap, app_info_path, socket_dir, error))
    return FALSE;

  flatpak_bwrap_add_arg (wrapper_bwrap, XWAYLAND_SATELLITE_WRAPPER);

  flatpak_bwrap_add_arg (wrapper_bwrap, socket_path);
  flatpak_bwrap_add_arg (wrapper_bwrap, ":99");

  flatpak_bwrap_add_arg (wrapper_bwrap, "--fd");
  flatpak_bwrap_add_arg_printf (wrapper_bwrap, "%d", sync_fd_wrapper);
  flatpak_bwrap_add_fd (wrapper_bwrap, g_steal_fd (&sync_fd_wrapper));

  flatpak_bwrap_finish (wrapper_bwrap);

  commandline = flatpak_quote_argv ((const char **) wrapper_bwrap->argv->pdata, -1);
  g_info ("Starting xwayland-satellite wrapper: %s", commandline);

  if (!g_spawn_async (NULL,
                      (char **) wrapper_bwrap->argv->pdata,
                      NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                      flatpak_bwrap_child_setup_cb, wrapper_bwrap->fds,
                      NULL, error))
    return FALSE;

  g_clear_pointer (&wrapper_bwrap, flatpak_bwrap_free);

  if (read (sync_fd_app, &x, 1) != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Failed to sync with xwayland-satellite"));
      return FALSE;
    }

  g_info ("xwayland-satellite listening on display :99");

  return TRUE;
}

gboolean
flatpak_run_add_x11_args (FlatpakBwrap         *bwrap,
                          FlatpakContextShares  shares,
                          const char           *app_info_path,
                          GError              **error)
{
  if (flatpak_run_has_wayland ())
    {
      g_info ("Starting isolated xwayland-satellite");
      return start_xwayland_satellite (bwrap, app_info_path, error);
    }

  g_info ("Wayland not available, falling back to host X11 passthrough");
  flatpak_run_add_host_x11_args (bwrap, shares);

  return TRUE;
}

void
flatpak_run_add_host_x11_args (FlatpakBwrap         *bwrap,
                               FlatpakContextShares  shares)
{
  g_autofree char *x11_socket = NULL;
  g_autofree char *remote_host = NULL;
  g_autofree char *display_nr = NULL;
  g_autoptr(GError) local_error = NULL;
  const char *display;
  int family = -1;
#ifdef ENABLE_XAUTH
  g_auto(GLnxTmpfile) xauth_tmpf = { 0, };
#endif

  display = g_getenv ("DISPLAY");
  if (display == NULL)
    {
      flatpak_bwrap_unset_env (bwrap, "DISPLAY");
      return;
    }

  g_info ("Allowing host x11 access");

  if (!flatpak_run_parse_x11_display (display, &family, &x11_socket,
                                      &remote_host, &display_nr,
                                      &local_error))
    {
      g_warning ("%s", local_error->message);
      flatpak_bwrap_unset_env (bwrap, "DISPLAY");
      return;
    }

  g_assert (display_nr != NULL);

  if (x11_socket != NULL
      && g_file_test (x11_socket, G_FILE_TEST_EXISTS))
    {
      g_assert (g_str_has_prefix (x11_socket, "/tmp/.X11-unix/X"));
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", x11_socket, x11_socket,
                              NULL);
      flatpak_bwrap_set_env (bwrap, "DISPLAY", display, TRUE);
    }
  else if ((shares & FLATPAK_CONTEXT_SHARED_NETWORK) == 0)
    {
      if (x11_socket != NULL)
        g_warning ("X11 socket %s does not exist in filesystem.",
                   x11_socket);
      else
        g_warning ("Remote X11 display detected.");

      g_warning ("X11 access will require --share=network permission.");
    }
  else if (x11_socket != NULL)
    {
      g_warning ("X11 socket %s does not exist in filesystem, "
                 "trying to use abstract socket instead.",
                 x11_socket);
    }
  else
    {
      g_debug ("Assuming --share=network gives access to remote X11");
    }

#ifdef ENABLE_XAUTH
  if (glnx_open_anonymous_tmpfile_full (O_RDWR | O_CLOEXEC, "/tmp", &xauth_tmpf, NULL))
    {
      FILE *output = fdopen (xauth_tmpf.fd, "wb");

      if (output != NULL)
        {
          int tmp_fd = dup (g_steal_fd (&xauth_tmpf.fd));

          if (tmp_fd != -1)
            {
              static const char dest[] = "/run/flatpak/Xauthority";

              write_xauth (family, remote_host, display_nr, output);
              flatpak_bwrap_add_args_data_fd (bwrap, "--ro-bind-data", tmp_fd, dest);

              flatpak_bwrap_set_env (bwrap, "XAUTHORITY", dest, TRUE);
            }

          fclose (output);

          if (tmp_fd != -1)
            lseek (tmp_fd, 0, SEEK_SET);
        }
    }
#endif
}
