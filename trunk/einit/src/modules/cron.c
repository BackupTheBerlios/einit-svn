/*
 *  cron.c
 *  einit
 *
 *  Created by Magnus Deininger on 05/09/2006.
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <einit/module.h>
#include <einit/config.h>
#include <einit/bitch.h>
#include <errno.h>
#include <string.h>

#define EXPECTED_EIV 1

#if EXPECTED_EIV != EINIT_VERSION
#warning "This module was developed for a different version of eINIT, you might experience problems"
#endif

int einit_cron_configure (struct lmodule *);

#if defined(EINIT_MODULE) || defined(EINIT_MODULE_HEADER)

char * einit_cron_provides[] = {"cron", "ecron", NULL};

const struct smodule einit_cron_self = {
 .eiversion = EINIT_VERSION,
 .eibuild   = BUILDNUMBER,
 .version   = 1,
 .mode      = 0,
 .name      = "eINIT Cron",
 .rid       = "cron",
 .si        = {
  .provides = einit_cron_provides,
  .requires = NULL,
  .after    = NULL,
  .before   = NULL
 },
 .configure = einit_cron_configure
};

module_register(einit_cron_self);

#endif

void einit_cron_timer_event_handler (struct einit_event *ev) {
 notice (1, "timer PING");
}

int einit_cron_cleanup (struct lmodule *this) {
 event_ignore (einit_event_subsystem_timer, einit_cron_timer_event_handler);

 return 0;
}

int einit_cron_enable (void *pa, struct einit_event *status) {
 event_timer_register_timeout (5);
 event_timer_register_timeout (10);
 event_timer_register_timeout (30);
 event_timer_register_timeout (60);

 return status_ok;
}

int einit_cron_disable (void *pa, struct einit_event *status) {
 return status_ok;
}

int einit_cron_configure (struct lmodule *irr) {
 module_init (irr);

 thismodule->cleanup = einit_cron_cleanup;
 thismodule->enable = einit_cron_enable;
 thismodule->disable = einit_cron_disable;

 event_listen (einit_event_subsystem_timer, einit_cron_timer_event_handler);

 return 0;
}
