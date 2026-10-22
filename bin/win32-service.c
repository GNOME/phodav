/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "win32-service.h"

#include <windows.h>

#define SERVICE_NAME "spice-webdavd"

typedef struct _Win32Service
{
  SharedDrive *drive;
  Win32RunFunc run;
  Win32StopFunc stop;
  gpointer user_data;
} Win32Service;

/* the parameters of the service, set before the control dispatcher
 * starts service_main() on its own thread */
static Win32Service service_params;

static SERVICE_STATUS service_status;
static SERVICE_STATUS_HANDLE service_status_handle;

static DWORD WINAPI
service_ctrl_handler (DWORD ctrl, DWORD type, LPVOID data, LPVOID ctx)
{
  Win32Service *service = ctx;
  DWORD ret = NO_ERROR;

  switch (ctrl)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      /* unmap the shared folder before the daemon stops */
      shared_drive_unmap (service->drive);

      service->stop (service->user_data);

      service_status.dwCurrentState = SERVICE_STOP_PENDING;
      SetServiceStatus (service_status_handle, &service_status);
      break;

    default:
      ret = ERROR_CALL_NOT_IMPLEMENTED;
    }

  return ret;
}

static VOID WINAPI
service_main (DWORD argc, TCHAR *argv[])
{
  service_status_handle =
    RegisterServiceCtrlHandlerEx (SERVICE_NAME, service_ctrl_handler, &service_params);

  g_return_if_fail (service_status_handle != 0);

  service_status.dwServiceType = SERVICE_WIN32;
  service_status.dwCurrentState = SERVICE_RUNNING;
  service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  service_status.dwWin32ExitCode = NO_ERROR;
  service_status.dwServiceSpecificExitCode = NO_ERROR;
  service_status.dwCheckPoint = 0;
  service_status.dwWaitHint = 0;
  SetServiceStatus (service_status_handle, &service_status);

  /* run the daemon until the service is stopped */
  service_params.run (service_params.user_data);

  service_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus (service_status_handle, &service_status);
}

/**
 * win32_service_run:
 * @drive: the shared folder to unmap when the service is stopped
 * @run: the daemon main loop
 * @stop: asks the daemon to stop
 * @user_data: passed to @run and @stop
 *
 * Runs the daemon as the "spice-webdavd" service: the service control
 * manager calls service_main(), which runs @run until the service is
 * stopped (its control handler calls @stop and unmaps @drive).
 *
 * This blocks until the service is stopped.
 */
void
win32_service_run (SharedDrive *drive,
                   Win32RunFunc run,
                   Win32StopFunc stop,
                   gpointer user_data)
{
  SERVICE_TABLE_ENTRY service_table[] = {
    { SERVICE_NAME, service_main },
    { NULL, NULL }
  };

  service_params.drive = drive;
  service_params.run = run;
  service_params.stop = stop;
  service_params.user_data = user_data;

  if (!StartServiceCtrlDispatcher (service_table))
    g_error ("%s", g_win32_error_message (GetLastError ()));
}
