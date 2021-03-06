/*
 *  tty.c
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

#include <sys/syscall.h>
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

int einit_tty_configure (struct lmodule *);

#if defined(EINIT_MODULE) || defined(EINIT_MODULE_HEADER)

const struct smodule einit_tty_self = {
 .eiversion = EINIT_VERSION,
 .eibuild   = BUILDNUMBER,
 .version   = 1,
 .mode      = 0,
 .name      = "TTY-Configuration",
 .rid       = "einit-tty",
 .si        = {
  .provides = NULL,
  .requires = NULL,
  .after    = NULL,
  .before   = NULL
 },
 .configure = einit_tty_configure
};

module_register(einit_tty_self);

#endif

struct ttyst *ttys = NULL;
char einit_tty_do_utmp;
pthread_mutex_t ttys_mutex = PTHREAD_MUTEX_INITIALIZER;

int einit_tty_texec (struct cfgnode *);

void einit_tty_process_event_handler (struct einit_event *);

int einit_tty_cleanup (struct lmodule *this) {
 exec_cleanup(this);
 utmp_cleanup(this);
 sched_configure(this);

 event_ignore (einit_event_subsystem_process, einit_tty_process_event_handler);

 return 0;
}

void *einit_tty_watcher (struct spidcb *spid) {
 pid_t pid = spid->pid;
 emutex_lock (&ttys_mutex);
 struct ttyst *cur = ttys;
 struct ttyst *prev = NULL;
 struct cfgnode *node = NULL;
 while (cur) {
  if (cur->pid == pid) {
   if (einit_tty_do_utmp) {
    create_utmp_record(utmprecord, DEAD_PROCESS, spid->pid, NULL, NULL, NULL, NULL, 0, 0, spid->pid);

    update_utmp (utmp_modify,&utmprecord);
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
  einit_tty_texec (node);
 }

 return 0;
}

void einit_tty_process_event_handler (struct einit_event *ev) {
 if (ev->type == einit_process_died) {
  struct spidcb *spid = ecalloc (1, sizeof (struct spidcb));
  spid->pid = ev->integer;
  spid->status = ev->status;

  einit_tty_watcher(spid);

  free (spid);
 }
}

int einit_tty_texec (struct cfgnode *node) {
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
   } else
#ifdef LINUX
   if ((cpid = syscall(__NR_clone, SIGCHLD, 0, NULL, NULL, NULL)) == 0)
#else
   if (!(cpid = fork()))
#endif
   {
    nice (-einit_core_niceness_increment);

    disable_core_dumps ();

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
    bitch (bitch_stdio, 0, "execve() failed.");
    exit(-1);
   } else if (cpid != -1) {
    int ctty = -1;
    pid_t curpgrp;

    if (einit_tty_do_utmp) {
     create_utmp_record(utmprecord, INIT_PROCESS, cpid, device, "etty", NULL, NULL, 0, 0, cpid);

     update_utmp (utmp_add, &utmprecord);
    }

//    sched_watch_pid (cpid, einit_tty_watcher);
    sched_watch_pid (cpid);

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

void einit_tty_disable_unused (char **enab_ttys) {
 emutex_lock (&ttys_mutex);
 struct ttyst *cur = ttys;

 while (cur) {
  if (cur->node && (!enab_ttys || !inset ((const void **)enab_ttys, cur->node->id + 18, SET_TYPE_STRING))) {
   notice (4, "disabling tty %s (not used in new mode)", cur->node->id + 18);
   cur->restart = 0;
   killpg (cur->pid, SIGHUP); // send a SIGHUP to the getty's process group
   kill (cur->pid, SIGTERM);
  }
  cur = cur->next;
 }
 emutex_unlock (&ttys_mutex);
}

char einit_tty_is_present (char *ttyname) {
 char present = 0;

 emutex_lock (&ttys_mutex);
 struct ttyst *cur = ttys;

 while (cur) {
  if (cur->node && strmatch (ttyname, cur->node->id + 18)) {
   present = 1;
   break;
  }
  cur = cur->next;
 }
 emutex_unlock (&ttys_mutex);

 return present;
}

void einit_tty_update() {
 struct cfgnode *node = NULL;
 char **enab_ttys = NULL, *blocked_tty = cfg_getstring ("configuration-feedback-visual-std-io/stdio", NULL);
 int i = 0;

 enab_ttys = str2set (':', cfg_getstring("ttys", NULL));

 notice (4, "reconfiguring ttys");

 einit_tty_disable_unused (enab_ttys);

 if (!enab_ttys || strmatch (enab_ttys[0], "none")) {
  notice (4, "no ttys to bring up");
 } else for (i = 0; enab_ttys[i]; i++) if (einit_tty_is_present (enab_ttys[i])) {
  notice (4, "not enabling tty %s (already enabled)", enab_ttys[i]);
 } else {
  char *tmpnodeid = emalloc (strlen(enab_ttys[i])+20);
  notice (4, "enabling tty %s (new)", enab_ttys[i]);

  memcpy (tmpnodeid, "configuration-tty-", 19);
  strcat (tmpnodeid, enab_ttys[i]);

  node = cfg_getnode (tmpnodeid, NULL);
  if (node && node->arbattrs) {
   if (blocked_tty) {
    char ttyblocked = 0;
    uint32_t i = 0;
    for (; node->arbattrs[i]; i+=2) {
     if (strmatch (node->arbattrs[i], "dev")) {
      if (strmatch (node->arbattrs[i+1], blocked_tty))
       ttyblocked = 1;
      break;
     }
    }

    if (ttyblocked) {
     notice (2, "refusing to put a getty on the feedback tty (%s)", blocked_tty);
    } else {
     einit_tty_texec (node);
    }
   } else {
    einit_tty_texec (node);
   }
  } else {
   notice (4, "einit-tty: node %s not found", tmpnodeid);
  }

  free (tmpnodeid);
 }

 free (enab_ttys);
}

void einit_tty_boot_event_handler (struct einit_event *ev) {
 switch (ev->type) {
  case einit_boot_devices_available:
   einit_tty_update();
   break;

   default: break;
 }
}

void einit_tty_core_event_handler (struct einit_event *ev) {
 switch (ev->type) {
  case einit_core_mode_switching:
   einit_tty_update();
   break;

   default: break;
 }
}

int einit_tty_configure (struct lmodule *this) {
 module_init (this);
 sched_configure(this);

 thismodule->cleanup = einit_tty_cleanup;

 utmp_configure(this);
 exec_configure(this);

 event_listen (einit_event_subsystem_process, einit_tty_process_event_handler);

 event_listen (einit_event_subsystem_core, einit_tty_core_event_handler);
 event_listen (einit_event_subsystem_boot, einit_tty_boot_event_handler);

 struct cfgnode *utmpnode = cfg_getnode ("configuration-tty-manage-utmp", NULL);
 if (utmpnode)
  einit_tty_do_utmp = utmpnode->flag;

 return 0;
}
