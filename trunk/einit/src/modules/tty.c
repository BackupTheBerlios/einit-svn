/*
 *  einit-tty.c
 *  einit
 *
 *  Created by Magnus Deininger on 20/04/2006.
 *  Renamed from tty.c on 11/10/2006.
 *  Copyright 2006, 2007 Magnus Deininger. All rights reserved.
 *
 */

/*
Copyright (c) 2006, 2007, Magnus Deininger
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

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <einit/module.h>
#include <einit/config.h>
#include <einit/utility.h>
#include <einit/bitch.h>
#include <einit-modules/scheduler.h>
#include <einit/event.h>
#include <string.h>
#include <einit-modules/process.h>
#include <einit-modules/exec.h>
#include <einit-modules/utmp.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <signal.h>
#include <pthread.h>
#include <utmp.h>
#include <fcntl.h>

#ifdef LINUX
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
/* okay, i think i found the proper file now */
#include <asm/ioctls.h>
#include <linux/vt.h>
#endif

#define EXPECTED_EIV 1

#if EXPECTED_EIV != EINIT_VERSION
#warning "This module was developed for a different version of eINIT, you might experience problems"
#endif

struct ttyst {
 pid_t pid;
 int restart;
 struct ttyst *next;
 struct cfgnode *node;
};

int _einit_tty_configure (struct lmodule *);

#if defined(_EINIT_MODULE) || defined(_EINIT_MODULE_HEADER)

char * _einit_tty_provides[] = {"tty", NULL};
char * _einit_tty_requires[] = {"mount/system", NULL};
const struct smodule _einit_tty_self = {
 .eiversion = EINIT_VERSION,
 .eibuild   = BUILDNUMBER,
 .version   = 1,
 .mode      = 0,
 .options   = 0,
 .name      = "TTY-Configuration",
 .rid       = "einit-tty",
 .si        = {
  .provides = _einit_tty_provides,
  .requires = _einit_tty_requires,
  .after    = NULL,
  .before   = NULL
 },
 .configure = _einit_tty_configure
};

module_register(_einit_tty_self);

#endif

struct ttyst *ttys = NULL;
char _einit_tty_do_utmp;
pthread_mutex_t ttys_mutex = PTHREAD_MUTEX_INITIALIZER;

int _einit_tty_texec (struct cfgnode *);

int _einit_tty_cleanup (struct lmodule *this) {
 exec_cleanup(this);
 utmp_cleanup(this);
 sched_configure(this);

 return 0;
}

void *_einit_tty_watcher (struct spidcb *spid) {
 pid_t pid = spid->pid;
 emutex_lock (&ttys_mutex);
 struct ttyst *cur = ttys;
 struct ttyst *prev = NULL;
 struct cfgnode *node = NULL;
 while (cur) {
  if (cur->pid == pid) {
   if (_einit_tty_do_utmp) {
    create_utmp_record(utmprecord, DEAD_PROCESS, spid->pid, NULL, NULL, NULL, NULL, 0, 0, spid->pid);

    update_utmp (UTMP_MODIFY,&utmprecord);
   }

   killpg (pid, SIGHUP); // send a SIGHUP to the getty's process group

   if (cur->restart)
    node = cur->node;
   if (prev != NULL) {
    prev->next = cur->next;
   } else {
    ttys = cur->next;
   }
   free (cur);
   break;
  }
  prev = cur;
  cur = cur->next;
 }
 emutex_unlock (&ttys_mutex);

 if (node) {
  if (node->id) {
   char tmp[BUFFERSIZE];
   esprintf (tmp, BUFFERSIZE, "einit-tty: restarting: %s\n", node->id);
   notice (6, tmp);
  }
  _einit_tty_texec (node);
 }

 return 0;
}

int _einit_tty_texec (struct cfgnode *node) {
 int i = 0, restart = 0;
 char *device = NULL, *command = NULL;
 char **environment = (char **)setdup((const void **)einit_global_environment, SET_TYPE_STRING);
 char **variables = NULL;

 for (; node->arbattrs[i]; i+=2) {
  if (strmatch("dev", node->arbattrs[i]))
   device = node->arbattrs[i+1];
  else if (strmatch("command", node->arbattrs[i]))
   command = node->arbattrs[i+1];
  else if (strmatch("restart", node->arbattrs[i]))
   restart = strmatch(node->arbattrs[i+1], "yes");
  else if (strmatch("variables", node->arbattrs[i])) {
   variables = str2set (':', node->arbattrs[i+1]);
  } else {
   environment = straddtoenviron (environment, node->arbattrs[i], node->arbattrs[i+1]);
  }
 }

 environment = create_environment(environment, (const char **)variables);
 if (variables) free (variables);

 if (command) {
  char **cmds = str2set (' ', command);
  pid_t cpid;
  if (cmds && cmds[0]) {
   struct stat statbuf;
   if (lstat (cmds[0], &statbuf)) {
    char cret [BUFFERSIZE];
    esprintf (cret, BUFFERSIZE, "%s: not forking, %s: %s", ( node->id ? node->id : "unknown node" ), cmds[0], strerror (errno));
    notice (2, cret);
   } else if (!(cpid = fork())) {
    if (device) {
     int newfd = eopen(device, O_RDWR);
     if (newfd) {
      eclose(0);
      eclose(1);
      eclose(2);
      dup2 (newfd, 0);
      dup2 (newfd, 1);
      dup2 (newfd, 2);
     }
    }
    execve (cmds[0], cmds, environment);
    bitch (BITCH_STDIO, 0, "execve() failed.");
    exit(-1);
   } else if (cpid != -1) {
    int ctty = -1;
    pid_t curpgrp;

    if (_einit_tty_do_utmp) {
     create_utmp_record(utmprecord, INIT_PROCESS, cpid, device, "etty", NULL, NULL, 0, 0, cpid);

     update_utmp (UTMP_ADD, &utmprecord);
    }

    sched_watch_pid (cpid, _einit_tty_watcher);

    setpgid (cpid, cpid);  // create a new process group for the new process
    if (((curpgrp = tcgetpgrp(ctty = 2)) < 0) ||
        ((curpgrp = tcgetpgrp(ctty = 0)) < 0) ||
        ((curpgrp = tcgetpgrp(ctty = 1)) < 0)) tcsetpgrp(ctty, cpid); // set foreground group

    struct ttyst *new = ecalloc (1, sizeof (struct ttyst));
    new->pid = cpid;
    new->node = node;
    new->restart = restart;
    emutex_lock (&ttys_mutex);
    new->next = ttys;
    ttys = new;
    emutex_unlock (&ttys_mutex);
   }
  }
 }

 if (environment) {
  free (environment);
  environment = NULL;
 }
 if (variables) {
  free (variables);
  variables = NULL;
 }

 return 0;
}

int _einit_tty_enable (void *pa, struct einit_event *status) {
 struct cfgnode *node = NULL;
 char **ttys = NULL;
 int i = 0;

 if (!(ttys = str2set (':', cfg_getstring("ttys", NULL)))) {
  status->string = "I've no idea what to start, really.";
  return STATUS_FAIL;
 }

 status->string = "creating environment";

 status->string = "commencing";
 status_update (status);

 for (i = 0; ttys[i]; i++) {
  char *tmpnodeid = emalloc (strlen(ttys[i])+20);
  status->string = ttys[i];
  status_update (status);

  memcpy (tmpnodeid, "configuration-tty-", 19);
  strcat (tmpnodeid, ttys[i]);

  node = cfg_getnode (tmpnodeid, NULL);
  if (node && node->arbattrs) {
   _einit_tty_texec (node);
  } else {
   char warning[BUFFERSIZE];
   esprintf (warning, BUFFERSIZE, "einit-tty: node %s not found", tmpnodeid);
   notice (3, warning);
  }

  free (tmpnodeid);
 }

 status->string="all ttys up";
 status_update (status);
 free (ttys);
 return STATUS_OK;
}

int _einit_tty_disable (void *pa, struct einit_event *status) {
 struct ttyst *cur = ttys;
 emutex_lock (&ttys_mutex);
#ifdef LINUX
 uint32_t vtn = parse_integer(cfg_getstring ("configuration-feedback-visual-std-io/activate-vt", NULL));
 int tfd = 0;
 errno = 0;
 if ((tfd = eopen ("/dev/tty1", O_RDWR)))
  ioctl (tfd, VT_ACTIVATE, vtn);
 if (errno)
  perror ("einit-tty: activate terminal");
 if (tfd > 0) eclose (tfd);
#endif

 while (cur) {
  cur->restart = 0;
  killpg (cur->pid, SIGHUP); // send a SIGHUP to the getty's process group
  kill (cur->pid, SIGTERM);
  cur = cur->next;
 }
 emutex_unlock (&ttys_mutex);
 return STATUS_OK;
}

int _einit_tty_reset (void *pa, struct einit_event *status) {
 return STATUS_OK;
}

int _einit_tty_configure (struct lmodule *this) {
 module_init (this);
 sched_configure(this);

 thismodule->cleanup = _einit_tty_cleanup;
 thismodule->enable = _einit_tty_enable;
 thismodule->disable = _einit_tty_disable;
 thismodule->reset = _einit_tty_reset;

 utmp_configure(this);
 exec_configure(this);

 struct cfgnode *utmpnode = cfg_getnode ("configuration-tty-manage-utmp", NULL);
 if (utmpnode)
  _einit_tty_do_utmp = utmpnode->flag;

 return 0;
}