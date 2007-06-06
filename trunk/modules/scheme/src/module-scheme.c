/*
 *  module-scheme.c
 *  einit
 *
 *  Created on 05/06/2007.
 *  Copyright 2007 Magnus Deininger. All rights reserved.
 *
 */

/*
Copyright (c) 2007, Magnus Deininger
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
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <einit/module.h>
#include <einit/config.h>
#include <einit/bitch.h>
#include <einit/utility.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <einit-modules/configuration.h>
#include <einit/configuration.h>

#include <einit-bundle/scheme.h>

#define EXPECTED_EIV 1

#if EXPECTED_EIV != EINIT_VERSION
#warning "This module was developed for a different version of eINIT, you might experience problems"
#endif

int module_scheme_configure (struct lmodule *);

#if defined(EINIT_MODULE) || defined(EINIT_MODULE_HEADER)

const struct smodule module_scheme_self = {
 .eiversion = EINIT_VERSION,
 .eibuild   = BUILDNUMBER,
 .version   = 1,
 .mode      = einit_module_loader,
 .name      = "Module Support (.scheme)",
 .rid       = "module-scheme",
 .si        = {
  .provides = NULL,
  .requires = NULL,
  .after    = NULL,
  .before   = NULL
 },
 .configure = module_scheme_configure
};

module_register(module_scheme_self);

#endif

int module_scheme_scanmodules (struct lmodule *);

int module_scheme_cleanup (struct lmodule *pa) {
 return 0;
}

int module_scheme_scanmodules ( struct lmodule *modchain ) {
 char **modules = NULL;
 char *initfile = cfg_getstring ("subsystem-scheme-init-file", NULL);
 char *init = initfile ? readfile (initfile) : "";

 modules = readdirfilter(cfg_getnode ("subsystem-scheme-import", NULL),
                                "/lib/einit/bootstrap-scheme/",
								".*\\.scheme$", NULL, 0);

 if (modules) {
  uint32_t i = 0;

  for (; modules[i]; i++) {
   char *data = readfile(modules[i]);
   if (data) {
    scheme *interpreter = scheme_init_new();
    scheme_init (interpreter);

    scheme_set_input_port_file(interpreter, stdin);
    scheme_set_output_port_file(interpreter, stderr);

	if (init) {
     scheme_load_string(interpreter, init);
	}

    notice (3, "have new scheme file: %s\n%s\n", modules[i], data);

    scheme_load_string(interpreter, data);
    free (data);

    scheme_deinit (interpreter);
   }
  }
 } else {
  notice (2, "no scheme modules found.");
 }

 return 1;
}

int module_scheme_configure (struct lmodule *pa) {
 module_init (pa);

 pa->scanmodules = module_scheme_scanmodules;
 pa->cleanup = module_scheme_cleanup;

 return 0;
}
