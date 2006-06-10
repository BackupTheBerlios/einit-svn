/*
 *  linux-mount.c
 *  einit
 *
 *  Created by Magnus Deininger on 27/05/2006.
 *  Copyright 2006 Magnus Deininger. All rights reserved.
 *
 */

/*
Copyright (c) 2006, Magnus Deininger
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
    * Neither the name of the project nor the names of its contributors may be
	  used to endorse or promote products derived from this software without
	  specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define _MODULE

#include <einit/module.h>
#include <einit/config.h>
#include <einit/common-mount.h>
#include <sys/mount.h>
#include <linux/fs.h>
#include <errno.h>
#include <string.h>

/* filesystem header files */
#include <linux/ext2_fs.h>

#define MOUNT_SUPPORT_EXT2

#define EXPECTED_EIV 1

#if EXPECTED_EIV != EINIT_VERSION
#warning "This module was developed for a different version of eINIT, you might experience problems"
#endif

enum mounttask {
 MOUNT_ROOT = 1,
 MOUNT_DEV = 2,
 MOUNT_PROC = 3,
 MOUNT_SYS = 4,
 MOUNT_LOCALMOUNT = 5,
 MOUNT_REMOTEMOUNT = 6,
};

/* variable definitions */
char *defaultblockdevicesource[] = {"dev", NULL};
char *defaultfstabsource[] = {"label", "configuration", "fstab", NULL};
char *defaultfilesystems[] = {"linux", NULL};
struct uhash *blockdevices = NULL;
struct uhash *fstab = NULL;
struct uhash *blockdevicesupdatefunctions = NULL;
struct uhash *fstabupdatefunctions = NULL;
struct uhash *filesystemlabelupdaterfunctions = NULL;

/* module definitions */
char *provides[] = {"mount", NULL};
const struct smodule self = {
 EINIT_VERSION, 1, EINIT_MOD_LOADER, 0, "Linux-specific Filesystem mounter", "linux-mount", provides, NULL, NULL
};

char *provides_localmount[] = {"localmount", NULL};
char *requires_localmount[] = {"/", "/dev", NULL};
struct smodule sm_localmount = {
 EINIT_VERSION, 1, EINIT_MOD_EXEC, 0, "linux-mount [localmount]", "linux-mount-localmount", provides_localmount, requires_localmount, NULL
};

char *provides_dev[] = {"/dev", NULL};
char *requires_dev[] = {"/", NULL};
struct smodule sm_dev = {
 EINIT_VERSION, 1, EINIT_MOD_EXEC, 0, "linux-mount [/dev]", "linux-mount-dev", provides_dev, requires_dev, NULL
};

char *provides_sys[] = {"/sys", NULL};
char *requires_sys[] = {"/", NULL};
struct smodule sm_sys = {
 EINIT_VERSION, 1, EINIT_MOD_EXEC, 0, "linux-mount [/sys]", "linux-mount-sys", provides_sys, requires_sys, NULL
};

char *provides_proc[] = {"/proc", NULL};
char *requires_proc[] = {"/", NULL};
struct smodule sm_proc = {
 EINIT_VERSION, 1, EINIT_MOD_EXEC, 0, "linux-mount [/proc]", "linux-mount-proc", provides_proc, requires_proc, NULL
};

char *provides_root[] = {"/", NULL};
struct smodule sm_root = {
 EINIT_VERSION, 1, EINIT_MOD_EXEC, 0, "linux-mount [/]", "linux-mount-root", provides_root, NULL, NULL
};

/* function declarations */
unsigned char read_label_linux (void *);
int scanmodules (struct lmodule *);
int configure (struct lmodule *);
int cleanup (struct lmodule *);
int enable (enum mounttask, struct mfeedback *);
int disable (enum mounttask, struct mfeedback *);
int mountwrapper (char *, struct mfeedback *);

/* function definitions */
unsigned char read_label_linux (void *na) {
 struct uhash *element = blockdevices;
 struct ext2_super_block ext2_sb;
 struct bd_info *bdi;
 while (element) {
  bdi = (struct bd_info *)element->value;

  FILE *device = NULL;
  device = fopen (element->key, "r");
  if (device) {
   if (fseek (device, 1024, SEEK_SET) || (fread (&ext2_sb, sizeof(struct ext2_super_block), 1, device) < 1)) {
//    perror (element->key);
    bdi->fs_status = FS_STATUS_ERROR | FS_STATUS_ERROR_IO;
   } else {
    if (ext2_sb.s_magic == EXT2_SUPER_MAGIC) {
     __u8 uuid[16];
     char c_uuid[38];

     bdi->fs_type = FILESYSTEM_EXT2;
     bdi->fs_status = FS_STATUS_OK;

     memcpy (uuid, ext2_sb.s_uuid, 16);
     if (ext2_sb.s_volume_name[0])
      bdi->label = estrdup (ext2_sb.s_volume_name);
     snprintf (c_uuid, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", /*((char)ext2_sb.s_uuid)*/ uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
     bdi->uuid = estrdup (c_uuid);
    }
   }
   fclose (device);
  } else {
   bdi->fs_status = FS_STATUS_ERROR | FS_STATUS_ERROR_IO;
//   perror (element->key);
  }
  errno = 0;
  element = hashnext (element);
 }
 return 0;
}

int scanmodules (struct lmodule *modchain) {
 mod_add (NULL, (int (*)(void *, struct mfeedback *))enable,
	        (int (*)(void *, struct mfeedback *))disable,
	        (void *)MOUNT_DEV, &sm_dev);
 mod_add (NULL, (int (*)(void *, struct mfeedback *))enable,
	        (int (*)(void *, struct mfeedback *))disable,
	        (void *)MOUNT_ROOT, &sm_root);
 mod_add (NULL, (int (*)(void *, struct mfeedback *))enable,
	        (int (*)(void *, struct mfeedback *))disable,
	        (void *)MOUNT_LOCALMOUNT, &sm_localmount);
 mod_add (NULL, (int (*)(void *, struct mfeedback *))enable,
	        (int (*)(void *, struct mfeedback *))disable,
	        (void *)MOUNT_SYS, &sm_sys);
 mod_add (NULL, (int (*)(void *, struct mfeedback *))enable,
	        (int (*)(void *, struct mfeedback *))disable,
	        (void *)MOUNT_PROC, &sm_proc);
}

int configure (struct lmodule *this) {
 blockdevicesupdatefunctions = hashadd (blockdevicesupdatefunctions, "dev", (void *)find_block_devices_recurse_path);
 fstabupdatefunctions = hashadd (fstabupdatefunctions, "label", (void *)forge_fstab_by_label);
 fstabupdatefunctions = hashadd (fstabupdatefunctions, "configuration", (void *)read_fstab_from_configuration);
 fstabupdatefunctions = hashadd (fstabupdatefunctions, "fstab", (void *)read_fstab);
 filesystemlabelupdaterfunctions = hashadd (filesystemlabelupdaterfunctions, "linux", (void *)read_label_linux);

 update_block_devices ();
 update_filesystem_labels ();
 update_fstab();
}

int cleanup (struct lmodule *this) {
}

int mountwrapper (char *mountpoint, struct mfeedback *status) {
}

int enable (enum mounttask p, struct mfeedback *status) {
 struct uhash *he = NULL;
 struct fstab_entry *fse = NULL;
 struct bd_info *bdi = NULL;
 char *fstype;
 void *fsdata = NULL;
 char verbosebuffer [1024];
 switch (p) {
  case MOUNT_ROOT:
   if ((he = hashfind (fstab, "/")) &&
       (fse = (struct fstab_entry *)he->value) &&
       (he = hashfind (blockdevices, fse->device)) &&
       (bdi = (struct bd_info *)he->value)) {
    if (bdi->label)
     snprintf (verbosebuffer, 1023, "remounting root filesystem (%s=%s) r/w", fse->device, bdi->label);
    else
     snprintf (verbosebuffer, 1023, "remounting root filesystem (%s) r/w", fse->device);
    status->verbose = verbosebuffer;
    status_update (status);
    switch (bdi->fs_type) {
     case FILESYSTEM_EXT2:
      fstype = "ext2";
      break;
     default:
      status->verbose = "device filesystem type not known";
      status_update (status);
      return STATUS_FAIL;
    }
    if (mount (fse->device, fse->mountpoint, fstype, MS_REMOUNT, fsdata) == -1) {
     if (errno < sys_nerr)
      status->verbose = (char *)sys_errlist[errno];
     else
      status->verbose = "an unknown error occured while trying to mount the root filesystem";
     status_update (status);
     return STATUS_FAIL;
    }
    return STATUS_OK;
   } else {
    status->verbose = "nothing known about the root filesystem; bailing out.";
    status_update (status);
    return STATUS_FAIL;
   }
   break;
  default:
   status->verbose = "I'm clueless?";
   status_update (status);
   return STATUS_FAIL;
   break;
 }
}

int disable (enum mounttask p, struct mfeedback *status) {
 switch (p) {
  case MOUNT_ROOT:
   status->verbose = "remounting root filesystem r/o";
   status_update (status);
   return STATUS_OK;
   break;
  default:
   status->verbose = "I'm clueless?";
   status_update (status);
   return STATUS_FAIL;
   break;
 }
}
