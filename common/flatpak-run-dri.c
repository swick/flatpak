/*
 * Copyright © 2025 Red Hat, Inc
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

#include "config.h"
#include "flatpak-run-dri-private.h"

#include <glib/gi18n-lib.h>
#include <sys/vfs.h>

#include "flatpak-utils-private.h"

#define VIRGL_SERVER_EXECUTABLE "virgl_test_server"

static char *
create_server_socket (const char *template)
{
  g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();
  g_autofree char *proxy_socket_dir = g_build_filename (user_runtime_dir, ".flatpak-virgl-server", NULL);
  g_autofree char *proxy_socket = g_build_filename (proxy_socket_dir, template, NULL);
  int fd;

  if (!glnx_shutil_mkdir_p_at (AT_FDCWD, proxy_socket_dir, 0755, NULL, NULL))
    return NULL;

  fd = g_mkstemp (proxy_socket);
  if (fd == -1)
    return NULL;

  close (fd);

  return g_steal_pointer (&proxy_socket);
}

/* This wraps the argv in a bwrap call, primary to allow the
   command to be run with a proper /.flatpak-info with data
   taken from app_info_path */
static gboolean
add_bwrap_wrapper (FlatpakBwrap *bwrap,
                   const char   *app_info_path,
                   GError      **error)
{
  glnx_autofd int app_info_fd = -1;
  g_auto(GLnxDirFdIterator) dir_iter = { 0 };
  struct dirent *dent;
  g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();
  g_autofree char *proxy_socket_dir = g_build_filename (user_runtime_dir, ".flatpak-virgl-server/", NULL);

  app_info_fd = open (app_info_path, O_RDONLY | O_CLOEXEC);
  if (app_info_fd == -1)
    return glnx_throw_errno_prefix (error, _("Failed to open app info file"));

  if (!glnx_dirfd_iterator_init_at (AT_FDCWD, "/", FALSE, &dir_iter, error))
    return FALSE;

  flatpak_bwrap_add_arg (bwrap, flatpak_get_bwrap ());

  while (TRUE)
    {
      glnx_autofd int o_path_fd = -1;
      struct statfs stfs;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dir_iter, &dent, NULL, error))
        return FALSE;

      if (dent == NULL)
        break;

      if (strcmp (dent->d_name, ".flatpak-info") == 0)
        continue;

      /* O_PATH + fstatfs is the magic that we need to statfs without automounting the target */
      o_path_fd = openat (dir_iter.fd, dent->d_name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
      if (o_path_fd == -1 || fstatfs (o_path_fd, &stfs) != 0 || stfs.f_type == AUTOFS_SUPER_MAGIC)
        continue; /* AUTOFS mounts are risky and can cause us to block (see issue #1633), so ignore it. Its unlikely the proxy needs such a directory. */

      if (dent->d_type == DT_DIR)
        {
          if (strcmp (dent->d_name, "tmp") == 0 ||
              strcmp (dent->d_name, "var") == 0 ||
              strcmp (dent->d_name, "run") == 0)
            flatpak_bwrap_add_arg (bwrap, "--bind");
          else
            flatpak_bwrap_add_arg (bwrap, "--ro-bind");

          flatpak_bwrap_add_arg_printf (bwrap, "/%s", dent->d_name);
          flatpak_bwrap_add_arg_printf (bwrap, "/%s", dent->d_name);
        }
      else if (dent->d_type == DT_LNK)
        {
          g_autofree gchar *target = NULL;

          target = glnx_readlinkat_malloc (dir_iter.fd, dent->d_name,
                                           NULL, error);
          if (target == NULL)
            return FALSE;
          flatpak_bwrap_add_args (bwrap, "--symlink", target, NULL);
          flatpak_bwrap_add_arg_printf (bwrap, "/%s", dent->d_name);
        }
    }

  flatpak_bwrap_add_args (bwrap, "--bind", proxy_socket_dir, proxy_socket_dir, NULL);

  /* This is a file rather than a bind mount, because it will then
     not be unmounted from the namespace when the namespace dies. */
  flatpak_bwrap_add_args (bwrap, "--perms", "0600", NULL);
  flatpak_bwrap_add_args_data_fd (bwrap, "--file", g_steal_fd (&app_info_fd), "/.flatpak-info");

  if (!flatpak_bwrap_bundle_args (bwrap, 1, -1, FALSE, error))
    return FALSE;

  /* End of options: the next argument will be the executable name */
  flatpak_bwrap_add_arg (bwrap, "--");

  return TRUE;
}

gboolean
flatpak_run_maybe_start_virgl_server (FlatpakBwrap           *app_bwrap,
                                      const char             *app_info_path,
                                      FlatpakContextDevices   devices,
                                      GError                **error)
{
  g_autoptr(FlatpakBwrap) server_bwrap = NULL;
  const char *virgl_server;
  int sync_fd_app = -1;
  g_autofd int sync_fd_server = -1;
  g_autofree char *virlg_server_socket = create_server_socket ("virgl-server-XXXXXX");
  g_autofree char *commandline = NULL;
  char x = 'x';

  if (!(devices & FLATPAK_CONTEXT_DEVICE_DRI))
    return TRUE;

  server_bwrap = flatpak_bwrap_new (NULL);

  /* FIXME: somehow the sandbox is too much for virgl
   * if (!add_bwrap_wrapper (server_bwrap, app_info_path, error))
   *  return FALSE;
   */

  virgl_server = g_getenv ("FLATPAK_VIRGL_SERVER");
  if (virgl_server == NULL)
    virgl_server = VIRGL_SERVER_EXECUTABLE;

  flatpak_bwrap_add_arg (server_bwrap, virgl_server);

  if (!flatpak_bwrap_add_sync_fd (app_bwrap, &sync_fd_server, &sync_fd_app))
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Unable to create sync pipe"));
      return FALSE;
    }

  //flatpak_bwrap_add_arg (server_bwrap, "--no-fork");
  //flatpak_bwrap_add_arg (server_bwrap, "--multi-clients");
  flatpak_bwrap_add_arg (server_bwrap, "--venus");
  flatpak_bwrap_add_arg (server_bwrap, "--use-gles");
  flatpak_bwrap_add_arg_printf (server_bwrap, "--sync-fd=%d", sync_fd_server);
  flatpak_bwrap_add_fd (server_bwrap, g_steal_fd (&sync_fd_server));

  if (virlg_server_socket == NULL)
    return FALSE;

  flatpak_bwrap_add_arg_printf (server_bwrap,
                                "--socket-path=%s", virlg_server_socket);

  flatpak_bwrap_finish (server_bwrap);

  commandline = flatpak_quote_argv ((const char **) server_bwrap->argv->pdata, -1);
  g_info ("Running '%s'", commandline);

  /* We use LEAVE_DESCRIPTORS_OPEN and close them in the child_setup
   * to work around a deadlock in GLib < 2.60 */
  if (!g_spawn_async (NULL,
                      (char **) server_bwrap->argv->pdata,
                      NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                      flatpak_bwrap_child_setup_cb, server_bwrap->fds,
                      NULL, error))
    return FALSE;

  /* The write end can be closed now, otherwise the read below will hang of
   * and the virgl server fails to start. */
  g_clear_pointer (&server_bwrap, flatpak_bwrap_free);

  /* Sync with proxy, i.e. wait until its listening on the sockets */
  if (read (sync_fd_app, &x, 1) != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Failed to sync with the virgl server"));
      return FALSE;
    }

  flatpak_bwrap_add_args (app_bwrap,
                          "--ro-bind",
                          virlg_server_socket,
                          "/run/flatpak/virgl-server",
                          NULL);

  flatpak_bwrap_set_env (app_bwrap, "VN_DEBUG", "vtest", TRUE);
  flatpak_bwrap_set_env (app_bwrap,
                         "VTEST_SOCKET_NAME",
                         "/run/flatpak/virgl-server",
                         TRUE);

  flatpak_bwrap_add_runtime_dir_member (app_bwrap, "virgl-server");

  return TRUE;
}
