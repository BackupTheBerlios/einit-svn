/*
 *  einit.c
 *  einit
 *
 *  Created by Magnus Deininger on 06/02/2006.
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

#include <stdio.h>
#include <unistd.h>
#include <einit/bitch.h>
#include <einit/config.h>
#include <einit/module.h>
#include <einit/utility.h>
#include <einit/scheduler.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>

int main(int, char **);
int print_usage_info ();
int ipc_process (char *);
int ipc_wait ();
int cleanup ();

int sock;
struct sockaddr_un saddr;
char *currentmode = "void";
char *newmode = "void";

int print_usage_info () {
 fputs ("eINIT " EINIT_VERSION_LITERAL "\nCopyright (c) 2006, Magnus Deininger\nUsage:\n einit [-c configfile] [-v] [-h]\n", stderr);
 return -1;
}

int ipc_process (char *cmd) {
 char **argv = str2set (' ', cmd);
 int argc = setcount ((void **)argv);
 if (!argv) return bitch (BTCH_ERRNO);
 if (!argv[0]) {
  free (argv);
  return bitch (BTCH_ERRNO);
 }

 if (!strcmp (argv[0], "power") && (argc > 1)) {
  if (!strcmp (argv[1], "off")) {
   sched_queue (SCHEDULER_SWITCH_MODE, "power-off");
   sched_queue (SCHEDULER_POWER_OFF, NULL);
  }
  if (!strcmp (argv[1], "reset")) {
   sched_queue (SCHEDULER_SWITCH_MODE, "power-reset");
   sched_queue (SCHEDULER_POWER_RESET, NULL);
  }
 }
}

int ipc_wait () {
 struct cfgnode *node = cfg_findnode ("control-socket", 0, NULL);
 int nfd;
 pthread_t **cthreads;
 pthread_attr_t threadattr;
 sock = socket (AF_UNIX, SOCK_STREAM, 0);
 if (sock == -1)
  return bitch (BTCH_ERRNO);

 saddr.sun_family = AF_UNIX;
 if (!node || !node->svalue) strncpy (saddr.sun_path, "/etc/einit-control", sizeof(saddr.sun_path) - 1);
 else strncpy (saddr.sun_path, node->svalue, sizeof(saddr.sun_path) - 1);

 if (bind(sock, (struct sockaddr *) &saddr, sizeof(struct sockaddr_un))) {
  close (sock);
  return bitch (BTCH_ERRNO);
 }

 if (listen (sock, 5)) {
  close (sock);
  return bitch (BTCH_ERRNO);
 }

 pthread_attr_init (&threadattr);

/* i was originally intending to create one thread per connection, but i think one thread in total should
   be sufficcient */
 while ((nfd = accept (sock, NULL, NULL)) != -1) {
//  pthread_t *thread = ecalloc (1, sizeof (pthread_t));
//  pthread_create (thread, &threadattr, (void *(*)(void *))ipc_process, (void *)&nfd);
//  pthread_detach (*thread);
  ssize_t br;
  ssize_t ic = 0;
  ssize_t i;
  char buf[BUFFERSIZE+1];
  char lbuf[BUFFERSIZE+1];

  while (br = read (nfd, buf, BUFFERSIZE)) {
   if ((br < 0) && (errno != EAGAIN) && (errno != EINTR)) {
    bitch (BTCH_ERRNO);
    break;
   }
   for (i = 0; i < br; i++) {
    if ((buf[i] == '\n') || (buf[i] == '\0')) {
     lbuf[ic] = 0;
     if (lbuf[0])
      ipc_process (lbuf);
     ic = -1;
     lbuf[0] = 0;
	} else {
     if (ic >= BUFFERSIZE) {
      lbuf[ic] = 0;
      if (lbuf[0])
       ipc_process (lbuf);
      ic = 0;
     }
     lbuf[ic] = buf[i];
    }
    ic++;
   }
  }
  lbuf[ic] = 0;
  if (lbuf[0])
   ipc_process (lbuf);

  close (nfd);
//  close (nfd);
 }

 pthread_attr_destroy (&threadattr);

 if (nfd == -1)
  bitch (BTCH_ERRNO);

 close (sock);
 if (unlink (saddr.sun_path)) bitch (BTCH_ERRNO);
 return 0;
}

int cleanup () {
 if (sock) {
  close (sock);
  if (unlink (saddr.sun_path)) bitch (BTCH_ERRNO);
 }
 mod_freemodules ();
 cfg_free ();
 bitch (BTCH_DL + BTCH_ERRNO);
}

int main(int argc, char **argv) {
 int i;

 for (i = 1; i < argc; i++) {
  if (argv[i][0] == '-')
   switch (argv[i][1]) {
	case 'c':
	 if ((++i) < argc)
      configfile = argv[i];
	 else
	  return print_usage_info ();
	 break;
	case 'h':
     return print_usage_info ();
	 break;
	case 'v':
     puts("eINIT " EINIT_VERSION_LITERAL "\nCopyright (c) 2006, Magnus Deininger");
     return 0;
   }
 }
 puts("eINIT " EINIT_VERSION_LITERAL ": booting");
 if (cfg_load () == -1) {
  fputs ("ERROR: cfg_load() failed\n", stderr);
  return -1;
 }
 mod_scanmodules ();
#ifdef DEBUG
 mod_ls ();
#endif

 sched_queue (SCHEDULER_SWITCH_MODE, "default");

 ipc_wait();

 cleanup ();

 return 0;
}
