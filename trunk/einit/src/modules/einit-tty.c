/*
 *  einit-tty.c
 *  einit
 *
 *  Created by Magnus Deininger on 20/04/2006.
 *  Renamed from tty.c on 11/10/2006.
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

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <einit/module.h>
#include <einit/config.h>
#include <einit/utility.h>
#include <einit/bitch.h>
#include <einit/scheduler.h>

#include <signal.h>
#include <pthread.h>
#include <utmp.h>
#include <fcntl.h>

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

char * provides[] = {"tty", NULL};
char * requires[] = {"mount/system", NULL};
const struct smodule self = {
	.eiversion	= EINIT_VERSION,
	.version	= 1,
	.mode		= 0,
	.options	= 0,
	.name		= "tty-configuration module",
	.rid		= "einit-tty",
	.provides	= provides,
	.requires	= requires,
	.notwith	= NULL
};

struct ttyst *ttys = NULL;
char **local_environment = NULL;
char do_utmp;
pthread_mutex_t ttys_mutex = PTHREAD_MUTEX_INITIALIZER;

int examine_configuration (struct lmodule *irr) {
 int pr = 0;

 if (!cfg_getnode("shell", NULL)) {
  fputs (" * configuration variable \"shell\" not found.\n", stderr);
  pr++;
 }

 return pr;
}

int configure (struct lmodule *this) {
 struct cfgnode *utmpnode = cfg_findnode ("tty-manage-utmp", 0, NULL);
 if (utmpnode)
  do_utmp = utmpnode->flag;
}

int cleanup (struct lmodule *this) {
}

void *watcher (struct spidcb *spid) {
 pid_t pid = spid->pid;
 int status = spid->status;
 pthread_mutex_lock (&ttys_mutex);
 struct ttyst *cur = ttys;
 struct ttyst *prev = NULL;
 struct cfgnode *node = NULL;
 while (cur) {
  if (cur->pid == pid) {
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
 pthread_mutex_unlock (&ttys_mutex);

 if (node) {
  if (node->id) {
   char tmp[2048];
   snprintf (tmp, 2048, "einit-tty: restarting: %s\n", node->id);
   notice (6, tmp);
  }
  texec (node);
 }
}

int texec (struct cfgnode *node) {
 int i = 0, restart = 0;
 char *device, *command;
 char **environment = NULL;
 char **variables = NULL;

 for (; node->arbattrs[i]; i+=2) {
  if (!strcmp("dev", node->arbattrs[i]))
   device = node->arbattrs[i+1];
  else if (!strcmp("command", node->arbattrs[i]))
   command = node->arbattrs[i+1];
  else if (!strcmp("restart", node->arbattrs[i]))
   restart = !strcmp(node->arbattrs[i+1], "yes");
  else if (!strcmp("variables", node->arbattrs[i])) {
   int i = 0;
   for (variables = str2set (':', node->arbattrs[i+1]); variables[i]; i++) {
    struct cfgnode *node = cfg_findnode (variables[i], 0, NULL);
    if (node)
     environment = straddtoenviron (environment, variables[i], node->svalue);
   }
  } else {
   environment = straddtoenviron (environment, node->arbattrs[i], node->arbattrs[i+1]);
  }
 }

 if (command) {
  char **cmds = str2set (' ', command);
  pid_t cpid;
  if (cmds && cmds[0]) {
   if (!(cpid = fork())) {
    if (device) {
     int newfd = open(device, O_RDWR, 0);
     if (newfd) {
      close(0);
      close(1);
      close(2);
      dup2 (newfd, 0);
      dup2 (newfd, 1);
      dup2 (newfd, 2);
     }
    }
    execve (cmds[0], cmds, environment);
    bitch (BTCH_ERRNO);
    exit(-1);
   } else if (cpid != -1) {
    sched_watch_pid (cpid, watcher);
    struct ttyst *new = ecalloc (1, sizeof (struct ttyst));
    new->pid = cpid;
    new->node = node;
    new->restart = restart;
    pthread_mutex_lock (&ttys_mutex);
    new->next = ttys;
    ttys = new;
    pthread_mutex_unlock (&ttys_mutex);
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
}

int enable (void *pa, struct einit_event *status) {
 struct cfgnode *node = NULL;
 char **ttys = NULL;
 int i = 0;

 if (!(ttys = str2set (':', cfg_getstring("ttys", NULL)))) {
  status->string = "I've no idea what to start, really.";
  return STATUS_FAIL;
 }

 node = cfg_getnode("shell", NULL);

 status->string = "commencing";
 status_update (status);

 for (; ttys[i]; i++) {
  status->string = ttys[i];
  status_update (status);
  node = cfg_getnode (ttys[i], NULL);
  if (node && node->arbattrs) {
   texec (node);
  }
 }

 status->string="all ttys up";
 status_update (status);
 free (ttys);
 return STATUS_OK;
}

int disable (void *pa, struct einit_event *status) {
 struct ttyst *cur = ttys;
 pthread_mutex_lock (&ttys_mutex);
 while (cur) {
  cur->restart = 0;
  kill (cur->pid, SIGTERM);
  cur = cur->next;
 }
 pthread_mutex_unlock (&ttys_mutex);
 return STATUS_OK;
}

int reset (void *pa, struct einit_event *status) {
 return STATUS_OK;
}
