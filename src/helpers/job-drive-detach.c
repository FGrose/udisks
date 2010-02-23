/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-*/
/*
 * Copyright (C) 2009 David Zeuthen <david@fubar.dk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

#include <scsi/sg_lib.h>
#include <scsi/sg_cmds.h>

#define LIBUDEV_I_KNOW_THE_API_IS_SUBJECT_TO_CHANGE
#include <libudev.h>

#include <glib.h>

static void
usage (void)
{
  g_printerr ("incorrect usage\n");
}

int
main (int argc,
      char *argv[])
{
  int ret;
  int sg_fd;
  const gchar *device;
  struct udev *udev;
  struct udev_device *udevice;
  struct udev_device *udevice_usb_interface;
  struct udev_device *udevice_usb_device;
  gchar *unbind_path;
  gchar *power_level_path;
  gchar *usb_interface_name;
  size_t usb_interface_name_len;
  FILE *f;
  struct stat statbuf;

  udev = NULL;
  udevice = NULL;
  udevice_usb_interface = NULL;
  udevice_usb_device = NULL;
  usb_interface_name = NULL;
  unbind_path = NULL;
  power_level_path = NULL;

  ret = 1;
  sg_fd = -1;

  if (argc != 2)
    {
      usage ();
      goto out;
    }

  device = argv[1];

  if (stat (device, &statbuf) != 0)
    {
      g_printerr ("Error statting %s: %m\n", device);
      goto out;
    }
  if (statbuf.st_rdev == 0)
    {
      g_printerr ("%s is not a special device file\n", device);
      goto out;
    }

  sg_fd = sg_cmds_open_device (device, 1 /* read_only */, 1);
  if (sg_fd < 0)
    {
      g_printerr ("Cannot open %s: %m\n", device);
      goto out;
    }

  g_printerr ("Detaching device: %s\n", device);

  g_printerr ("SYNCHRONIZE CACHE: ");
  if (sg_ll_sync_cache_10 (sg_fd, 0, /* sync_nv */
                           0, /* immed */
                           0, /* group */
                           0, /* lba */
                           0, /* count */
                           1, /* noisy */
                           0 /* verbose */
                           ) != 0)
    {
      g_printerr ("FAILED: %m\n");
      /* this is not a catastrophe, carry on */
      g_printerr ("(Continuing despite SYNCHRONIZE CACHE failure.)\n");
    }
  else
    {
      g_printerr ("OK\n");
    }

  g_printerr ("STOP UNIT: ");
  if (sg_ll_start_stop_unit (sg_fd, 0, /* immed */
                             0, /* pc_mod__fl_num */
                             0, /* power_cond */
                             0, /* noflush__fl */
                             0, /* loej */
                             0, /* start */
                             1, /* noisy */
                             0 /* verbose */
                             ) != 0)
    {
      g_printerr ("FAILED: %m\n");
      goto out;
    }
  else
    {
      g_printerr ("OK\n");
    }

  /* OK, close the device */
  sg_cmds_close_device (sg_fd);
  sg_fd = -1;

  /* Now unbind the usb-storage driver from the usb interface */
  udev = udev_new ();
  if (udev == NULL)
    {
      g_printerr ("Error initializing libudev: %m\n");
      goto out;
    }

  udevice = udev_device_new_from_devnum (udev, 'b', statbuf.st_rdev);
  if (udevice == NULL)
    {
      g_printerr ("No udev device for device %s (devnum 0x%08x): %m\n", device, (gint) statbuf.st_rdev);
      goto out;
    }

  /* unbind the mass storage driver (e.g. usb-storage) */
  udevice_usb_interface = udev_device_get_parent_with_subsystem_devtype (udevice, "usb", "usb_interface");
  if (udevice_usb_interface == NULL)
    {
      g_printerr ("No usb parent interface for %s: %m\n", device);
      goto out;
    }

  usb_interface_name = g_path_get_basename (udev_device_get_devpath (udevice_usb_interface));
  usb_interface_name_len = strlen (usb_interface_name);

  g_printerr ("Unbind USB interface driver: ");
  unbind_path = g_strdup_printf ("%s/driver/unbind", udev_device_get_syspath (udevice_usb_interface));
  f = fopen (unbind_path, "w");
  if (f == NULL)
    {
      g_printerr ("Cannot open %s for writing: %m\n", unbind_path);
      goto out;
    }
  if (fwrite (usb_interface_name, sizeof(char), usb_interface_name_len, f) < usb_interface_name_len)
    {
      g_printerr ("Error writing %s to %s: %m\n", unbind_path, usb_interface_name);
      fclose (f);
      goto out;
    }
  fclose (f);
  g_printerr ("OK\n");


  /* If this is the only USB interface on the device, also suspend the
   * USB device to e.g.  make the lights on the device power off.
   *
   * Failing to do so is not an error, the user may not have
   * CONFIG_USB_SUSPEND enabled in their kernel.
   */
  ret = 0;

  g_printerr ("Suspend USB Device: ");

  udevice_usb_device = udev_device_get_parent_with_subsystem_devtype (udevice, "usb", "usb_device");
  if (udevice_usb_device != NULL)
    {
      const char *bNumInterfaces;
      char *endp;
      int num_interfaces;
      gchar * power_level_path;
      gchar suspend_str[] = "suspend";

      bNumInterfaces = udev_device_get_sysattr_value (udevice_usb_device, "bNumInterfaces");
      num_interfaces = strtol (bNumInterfaces, &endp, 0);
      if (endp != NULL && num_interfaces == 1)
        {
          power_level_path = g_strdup_printf ("%s/power/level", udev_device_get_syspath (udevice_usb_device));
          f = fopen (power_level_path, "w");
          if (f == NULL)
            {
              g_printerr ("FAILED: Cannot open %s for writing: %m\n", unbind_path);
            }
          else
            {
              if (fwrite (suspend_str, sizeof(char), strlen (suspend_str), f) < strlen (suspend_str))
                {
                  g_printerr ("FAILED: Error writing %s to %s: %m\n", power_level_path, suspend_str);
                }
              else
                {
                  g_printerr ("OK\n");
                }
              fclose (f);
            }
        }
      else
        {
          g_printerr ("Skipping since multiple USB interfaces exist.\n");
        }
    }

 out:
  g_free (usb_interface_name);
  g_free (unbind_path);
  g_free (power_level_path);
  if (sg_fd > 0)
    sg_cmds_close_device (sg_fd);
  if (udevice != NULL)
    udev_device_unref (udevice);
  if (udev != NULL)
    udev_unref (udev);
  return ret;
}
