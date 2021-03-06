/*
 *  fqdn.c
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

int einit_fqdn_configure (struct lmodule *);

#if defined(EINIT_MODULE) || defined(EINIT_MODULE_HEADER)

struct einit_cfgvar_info
  einit_fqdn_cfgvar_hostname = {
   .options = eco_optional | eco_warn_if_default,
   .variable = "configuration-network-hostname",
   .description = "Your Machine's Hostname.",
   .default_value = "localhost" },
  einit_fqdn_cfgvar_domainname = {
   .options = eco_optional | eco_warn_if_default,
   .variable = "configuration-network-domainname",
   .description = "Your Machine's Domainname.",
   .default_value = "local" },
  *einit_fqdn_cfgvar_configuration[] = { &einit_fqdn_cfgvar_hostname, &einit_fqdn_cfgvar_domainname, NULL };

const struct smodule einit_fqdn_self = {
 .eiversion = EINIT_VERSION,
 .eibuild   = BUILDNUMBER,
 .version   = 1,
 .mode      = 0,
 .name      = "FQDN",
 .rid       = "einit-fqdn",
 .si        = {
  .provides = NULL,
  .requires = NULL,
  .after    = NULL,
  .before   = NULL
 },
 .configure = einit_fqdn_configure,
 .configuration = einit_fqdn_cfgvar_configuration
};

module_register(einit_fqdn_self);

#endif

void einit_fqdn_boot_event_handler (struct einit_event *);
int einit_fqdn_usage = 0;

int einit_fqdn_cleanup (struct lmodule *this) {
 event_ignore (einit_event_subsystem_boot, einit_fqdn_boot_event_handler);

 return 0;
}

void einit_fqdn_set () {
 char *hname, *dname;
 if ((hname = cfg_getstring ("configuration-network-hostname", NULL)))
  sethostname (hname, strlen (hname));
 if ((dname = cfg_getstring ("configuration-network-domainname", NULL)))
  setdomainname (dname, strlen (dname));
 notice (4, "hostname set to: %s.%s", hname, dname);
}

void einit_fqdn_boot_event_handler (struct einit_event *ev) {
 einit_fqdn_usage++;
 switch (ev->type) {
  case einit_boot_early:
   einit_fqdn_set();
   break;

/*  case einit_boot_devices_available:
   break;*/
/* this latter hook is gonna be needed for domainname setting via proc */

  default: break;
 }
 einit_fqdn_usage--;
}

int einit_fqdn_suspend (struct lmodule *irr) {
 if (!einit_fqdn_usage) {
  event_wakeup (einit_boot_early, irr);
  event_ignore (einit_event_subsystem_boot, einit_fqdn_boot_event_handler);

  return status_ok;
 } else
  return status_failed;
}

int einit_fqdn_resume (struct lmodule *irr) {
 event_wakeup_cancel (einit_boot_early, irr);

 return status_ok;
}

int einit_fqdn_configure (struct lmodule *irr) {
 module_init (irr);

 thismodule->cleanup = einit_fqdn_cleanup;
 thismodule->suspend = einit_fqdn_suspend;
 thismodule->resume  = einit_fqdn_resume;

 event_listen (einit_event_subsystem_boot, einit_fqdn_boot_event_handler);

 return 0;
}
