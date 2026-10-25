/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "drive-mapping.h"

#include <stdio.h>

#include <gio/gio.h>
#include <windows.h>

#define MAX_SHARED_FOLDER_NAME_SIZE 64
#define MAX_DRIVE_LETTER_SIZE 3

struct _SharedDrive
{
  gint ref_count;

  /* the letter of the mapped shared folder, or 0 when there is none;
   * shared with the service control handler and the mapping worker
   * thread, guarded by the mutex */
  gchar letter;
  GMutex mutex;

  /* the port of the HTTP server the shared folder is mapped over, set
   * once before the worker thread is started */
  guint port;

  /* cancels the mapping worker */
  GCancellable *cancel;
};

static SharedDrive *
shared_drive_ref (SharedDrive *drive)
{
  g_atomic_int_inc (&drive->ref_count);

  return drive;
}

static void
shared_drive_unref (SharedDrive *drive)
{
  if (g_atomic_int_dec_and_test (&drive->ref_count))
    {
      g_mutex_clear (&drive->mutex);
      g_object_unref (drive->cancel);
      g_free (drive);
    }
}

SharedDrive *
shared_drive_new (void)
{
  SharedDrive *drive = g_new0 (SharedDrive, 1);

  drive->ref_count = 1;
  g_mutex_init (&drive->mutex);
  drive->cancel = g_cancellable_new ();

  return drive;
}

void
shared_drive_free (SharedDrive *drive)
{
  /* wake up the mapping worker; it holds its own reference, the drive
   * is freed once it is done with it */
  g_cancellable_cancel (drive->cancel);
  shared_drive_unref (drive);
}

/**
 * shared_drive_get_letter:
 * @drive: a #SharedDrive
 *
 * Returns: the letter of the mapped shared folder, or 0 when there is
 * none
 */
gchar
shared_drive_get_letter (SharedDrive *drive)
{
  gchar letter;

  g_mutex_lock (&drive->mutex);
  letter = drive->letter;
  g_mutex_unlock (&drive->mutex);

  return letter;
}

static gchar
get_free_drive_letter (void)
{
  const guint32 max_mask = 1 << 25;
  guint32 drives;
  gint i;

  drives = GetLogicalDrives ();
  if (drives == 0)
    {
      g_warning ("%s", g_win32_error_message (GetLastError ()));
      return 0;
    }

  for (i = 0; i < 26; i++)
    {
      guint32 mask = max_mask >> i;
      if ((drives & mask) == 0)
        return 'z' - i;
    }

  return 0;
}

/* the letter of the already mapped shared folder, if there is one */
static gchar
get_spice_folder_letter (guint port)
{
  const guint32 max_mask = 1 << 25;
  gchar local_name[3] = "z:";
  g_autofree gchar *spice_folder_name = NULL;
  gchar spice_folder_letter = 0;
  guint32 drives;
  guint32 i;

  drives = GetLogicalDrives ();
  spice_folder_name = g_strdup_printf ("\\\\localhost@%d\\DavWWWRoot", port);

  for (i = 0; i < 26; i++)
    {
      gchar remote_name[MAX_SHARED_FOLDER_NAME_SIZE];
      guint32 size = sizeof (remote_name);
      guint32 mask = max_mask >> i;
      if (drives & mask)
        {
          local_name[0] = 'z' - i;
          if ((WNetGetConnection (local_name, remote_name, (LPDWORD) &size) == NO_ERROR) &&
              (g_strcmp0 (remote_name, spice_folder_name) == 0))
            {
              spice_folder_letter = local_name[0];
              g_debug ("Found Spice Shared Folder at %c drive", spice_folder_letter);
              break;
            }
        }
    }

  return spice_folder_letter;
}

/* map @drive_letter over the HTTP server of the daemon */
static guint32
map_drive (guint port, const gchar drive_letter)
{
  NETRESOURCE net_resource;
  guint32 errn;

  net_resource.dwType = RESOURCETYPE_DISK;
  net_resource.lpLocalName = g_strdup_printf ("%c:", drive_letter);
  net_resource.lpRemoteName = g_strdup_printf ("http://localhost:%d/", port);
  net_resource.lpProvider = NULL;

  errn = WNetAddConnection2 (&net_resource, NULL, NULL, CONNECT_TEMPORARY);

  g_free (net_resource.lpLocalName);
  g_free (net_resource.lpRemoteName);

  if (errn == NO_ERROR)
    {
      g_debug ("Shared folder mapped to %c succesfully", drive_letter);
    }
  else if (errn == ERROR_ALREADY_ASSIGNED)
    {
      g_debug ("Drive letter %c is already assigned", drive_letter);
    }
  else
    {
      g_warning ("map_drive error %d", errn);
    }

  return errn;
}

/* map the shared folder with a free drive letter, retrying with
 * another letter when the chosen one is already assigned. Runs in a
 * worker thread, as the mapping blocks. */
static void
map_drive_thread (GTask *task,
                  gpointer source_object,
                  gpointer task_data,
                  GCancellable *cancellable)
{
  SharedDrive *drive = task_data;
  const guint32 delay = 500; // half a second
  GPollFD cancel_pollfd;
  gchar drive_letter = 0;
  guint32 ret;

  if (!g_cancellable_make_pollfd (drive->cancel, &cancel_pollfd))
    {
      g_critical ("GPollFD failed to create.");
      return;
    }

  /* give the HTTP server of the daemon a moment to start listening,
   * before mapping the shared folder over it */
  ret = g_poll (&cancel_pollfd, 1, delay);
  g_cancellable_release_fd (drive->cancel);

  /* the daemon is stopping */
  if (ret != 0)
    return;

  while (TRUE)
    {
      drive_letter = get_free_drive_letter ();
      if (drive_letter == 0)
        {
          g_warning ("all drive letters already assigned.");
          break;
        }

      ret = map_drive (drive->port, drive_letter);
      if (ret == ERROR_ALREADY_ASSIGNED)
        {
          /* try again with another letter */
          continue;
        }
      if (ret != NO_ERROR)
        drive_letter = 0;
      break;
      // TODO: After mapping, rename network drive from \\localhost@PORT\DavWWWRoot
      //       to something like SPICE Shared Folder
    }

  g_mutex_lock (&drive->mutex);
  drive->letter = drive_letter;
  g_mutex_unlock (&drive->mutex);
}

/**
 * shared_drive_map:
 * @drive: a #SharedDrive
 * @port: the port of the HTTP server to map the shared folder over
 *
 * Maps the shared folder, unless it already is: the mapping blocks
 * (the network, the drive letters...), it happens in a worker
 * thread.
 */
void
shared_drive_map (SharedDrive *drive, guint port)
{
  g_autoptr(GTask) task = NULL;
  gchar letter = get_spice_folder_letter (port);

  g_mutex_lock (&drive->mutex);
  drive->letter = letter;
  g_mutex_unlock (&drive->mutex);

  if (letter != 0)
    return;

  drive->port = port;

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_source_tag (task, shared_drive_map);

  /* the task keeps the drive alive until the worker is done with it */
  g_task_set_task_data (task, shared_drive_ref (drive), (GDestroyNotify) shared_drive_unref);
  g_task_run_in_thread (task, map_drive_thread);
}

/**
 * shared_drive_unmap:
 * @drive: a #SharedDrive
 *
 * Unmaps the shared folder, if it is mapped.
 */
void
shared_drive_unmap (SharedDrive *drive)
{
  gchar local_name[MAX_DRIVE_LETTER_SIZE];
  guint32 errn;

  g_mutex_lock (&drive->mutex);

  if (drive->letter == 0)
    {
      g_mutex_unlock (&drive->mutex);
      return;
    }

  snprintf (local_name, sizeof (local_name), "%c:", drive->letter);
  errn = WNetCancelConnection2 (local_name, CONNECT_UPDATE_PROFILE, TRUE);

  if (errn == NO_ERROR)
    {
      g_debug ("Shared folder unmapped succesfully");
      drive->letter = 0;
    }
  else if (errn == ERROR_NOT_CONNECTED)
    {
      g_debug ("Drive %c is not connected", drive->letter);
    }
  else
    {
      g_warning ("unmap_drive error %d", errn);
    }

  g_mutex_unlock (&drive->mutex);
}
