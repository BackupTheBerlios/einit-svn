/*
 *  module-transformations.c
 *  einit
 *
 *  Created by Magnus Deininger on 10/03/2007.
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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

#include <einit/module.h>
#include <einit/config.h>
#include <einit/event.h>
#include <einit/utility.h>
#include <einit/tree.h>
#include <einit/bitch.h>

#include <string.h>
#include <regex.h>

#define EXPECTED_EIV 1

#if EXPECTED_EIV != EINIT_VERSION
#warning "This module was developed for a different version of eINIT, you might experience problems"
#endif

int einit_module_transformations_configure (struct lmodule *);

#if defined(EINIT_MODULE) || defined(EINIT_MODULE_HEADER)

const struct smodule einit_module_transformations_self = {
 .eiversion = EINIT_VERSION,
 .eibuild   = BUILDNUMBER,
 .version   = 1,
 .mode      = 0,
 .name      = "Module Transformations",
 .rid       = "einit-module-transformations",
 .si        = {
  .provides = NULL,
  .requires = NULL,
  .after    = NULL,
  .before   = NULL
 },
 .configure = einit_module_transformations_configure
};

module_register(einit_module_transformations_self);

#endif

struct stree *service_aliases = NULL;

#ifdef POSIXREGEX
struct stree *service_transformations = NULL;

#define SVT_STRIP_PROVIDES  0x00000001
#define SVT_STRIP_REQUIRES  0x00000002
#define SVT_STRIP_AFTER     0x00000004
#define SVT_STRIP_BEFORE    0x00000008
#define SVT_STRIP_SD_AFTER  0x00000010
#define SVT_STRIP_SD_BEFORE 0x00000020

struct service_transformation {
 char *in, *out;
 regex_t *id_pattern;
 uint32_t options;
};
#endif

struct {
 void **chunks;
} einit_module_transformations_garbage = {
 .chunks = NULL
};

pthread_mutex_t einit_module_transformations_garbage_mutex = PTHREAD_MUTEX_INITIALIZER;
int einit_module_transformations_usage = 0;

void einit_module_transformations_garbage_add_chunk (void *chunk) {
 if (!chunk) return;
 emutex_lock (&einit_module_transformations_garbage_mutex);
 einit_module_transformations_garbage.chunks = setadd (einit_module_transformations_garbage.chunks, chunk, SET_NOALLOC);
 emutex_unlock (&einit_module_transformations_garbage_mutex);
}

void einit_module_transformations_garbage_free () {
 emutex_lock (&einit_module_transformations_garbage_mutex);
 if (einit_module_transformations_garbage.chunks) {
  int i = 0;

  for (; einit_module_transformations_garbage.chunks[i]; i++) {
   free (einit_module_transformations_garbage.chunks[i]);
  }

  free (einit_module_transformations_garbage.chunks);
  einit_module_transformations_garbage.chunks = NULL;
 }
 emutex_unlock (&einit_module_transformations_garbage_mutex);
}

void einit_module_transformations_einit_event_handler (struct einit_event *ev) {
 einit_module_transformations_usage++;

 if (ev->type == einit_core_configuration_update) {
  struct stree *new_aliases = NULL, *ca = NULL;
  struct cfgnode *node = NULL;
#ifdef POSIXREGEX
  struct stree *new_transformations = NULL;
#endif

  while ((node = cfg_findnode ("services-alias", 0, node))) {
   if (node->idattr && node->svalue) {
    new_aliases = streeadd (new_aliases, node->svalue, node->idattr, SET_TYPE_STRING, NULL);
    new_aliases = streeadd (new_aliases, node->idattr, node->svalue, SET_TYPE_STRING, NULL);
   }
  }

  ca = service_aliases;
  service_aliases = new_aliases;
  if (ca)
   streefree (ca);

  node = NULL;

#ifdef POSIXREGEX
  while ((node = cfg_findnode ("services-transform", 0, node))) {
   if (node->arbattrs) {
    struct service_transformation new_transformation;
    ssize_t sti = 0;
    char have_pattern = 0;

    memset (&new_transformation, 0, sizeof(struct service_transformation));

    for (; node->arbattrs[sti]; sti+=2) {
     if (strmatch (node->arbattrs[sti], "in")) {
      new_transformation.in = node->arbattrs[sti+1];
     } else if (strmatch (node->arbattrs[sti], "out")) {
      new_transformation.out = node->arbattrs[sti+1];
     } else if (strmatch (node->arbattrs[sti], "strip-from")) {
      char **tmp = str2set (':', node->arbattrs[sti+1]);

      if (tmp) {
       if (inset ((const void **)tmp, (void *)"provides", SET_TYPE_STRING))
        new_transformation.options |= SVT_STRIP_PROVIDES;
       if (inset ((const void **)tmp, (void *)"requires", SET_TYPE_STRING))
        new_transformation.options |= SVT_STRIP_REQUIRES;
       if (inset ((const void **)tmp, (void *)"after", SET_TYPE_STRING))
        new_transformation.options |= SVT_STRIP_AFTER;
       if (inset ((const void **)tmp, (void *)"before", SET_TYPE_STRING))
        new_transformation.options |= SVT_STRIP_BEFORE;
       if (inset ((const void **)tmp, (void *)"shutdown-before", SET_TYPE_STRING))
        new_transformation.options |= SVT_STRIP_SD_BEFORE;
       if (inset ((const void **)tmp, (void *)"shutdown-after", SET_TYPE_STRING))
        new_transformation.options |= SVT_STRIP_SD_AFTER;

       free (tmp);
      }
     } else if (strmatch (node->arbattrs[sti], "module-id")) {
      regex_t *buffer = emalloc (sizeof (regex_t));

      if ((have_pattern = !eregcomp (buffer, node->arbattrs[sti+1]))) {
       new_transformation.id_pattern = buffer;
      }
     }
    }

    if (have_pattern && new_transformation.in) {
     new_transformations =
       streeadd (new_transformations, new_transformation.in, (void *)(&new_transformation), sizeof(new_transformation), new_transformation.id_pattern);
    }
   }
  }

  ca = service_transformations;
  service_transformations = new_transformations;
  if (ca)
   streefree (ca);
#endif
 } else if (ev->type == einit_core_update_module) {
  struct lmodule *module = ev->para;
  struct cfgnode *lnode = NULL;

/* for the record: setting a pointer is apparently atomic, so we don't need any locks */

  while ((lnode = cfg_findnode ("services-override-module", 0, lnode)))
   if (lnode->idattr && module->module->rid && strmatch(lnode->idattr, module->module->rid)) {
    struct service_information *esi = ecalloc (1, sizeof (struct service_information));
    uint32_t i = 0;

    if (module->si) {
     esi->requires = module->si->requires;
     esi->provides = module->si->provides;
     esi->after = module->si->after;
     esi->before = module->si->before;
     esi->shutdown_after = module->si->shutdown_after;
     esi->shutdown_before = module->si->shutdown_before;
    }

    for (; lnode->arbattrs[i]; i+=2) {
     if (strmatch (lnode->arbattrs[i], "requires")) esi->requires = str2set (':', lnode->arbattrs[i+1]);
     else if (strmatch (lnode->arbattrs[i], "provides")) esi->provides = str2set (':', lnode->arbattrs[i+1]);
     else if (strmatch (lnode->arbattrs[i], "after")) esi->after = str2set (':', lnode->arbattrs[i+1]);
     else if (strmatch (lnode->arbattrs[i], "before")) esi->before = str2set (':', lnode->arbattrs[i+1]);
     else if (strmatch (lnode->arbattrs[i], "shutdown-before")) esi->shutdown_before = str2set (':', lnode->arbattrs[i+1]);
     else if (strmatch (lnode->arbattrs[i], "shutdown-after")) esi->shutdown_after = str2set (':', lnode->arbattrs[i+1]);
    }

    if (module->si) {
     einit_module_transformations_garbage_add_chunk (module->si);
     einit_module_transformations_garbage_add_chunk (module->si->provides);
     einit_module_transformations_garbage_add_chunk (module->si->requires);
     einit_module_transformations_garbage_add_chunk (module->si->after);
     einit_module_transformations_garbage_add_chunk (module->si->before);
     einit_module_transformations_garbage_add_chunk (module->si->shutdown_before);
     einit_module_transformations_garbage_add_chunk (module->si->shutdown_after);
    }

    module->si = esi;
    break;
   }

   if (service_aliases && module->si &&module->si->provides) {
    uint32_t i = 0;
    char **np = (char **)setdup ((const void **)module->si->provides, SET_TYPE_STRING);
    for (; module->si->provides[i]; i++) {
     struct stree *x = streefind (service_aliases, module->si->provides[i], tree_find_first);

     while (x) {
      if (x->value) {
       if (!inset ((const void **)np, x->value, SET_TYPE_STRING)) {
        np = (char **)setadd ((void **)np, x->value, SET_TYPE_STRING);
       }
      }
      x = streefind (x, module->si->provides[i], tree_find_next);
     }
    }

    einit_module_transformations_garbage_add_chunk (module->si->provides);
    module->si->provides = np;
   }

#ifdef POSIXREGEX
   if (service_transformations && module->si) {
    uint32_t i;

    if (module->si->provides) {
     char **np = NULL;

     for (i = 0; module->si->provides[i]; i++) {
      char hit = 0;
      struct stree *x = streefind (service_transformations, module->si->provides[i], tree_find_first);

      while (x) {
       struct service_transformation *trans =
         (struct service_transformation *)x->value;

       if (regexec (trans->id_pattern, module->module->rid, 0, NULL, 0)) {
        x = streefind (x, module->si->provides[i], tree_find_next);
        continue;
       }

       hit = 1;

       if (trans->options & SVT_STRIP_PROVIDES) break;

       np = (char **)setadd ((void **)np, trans->out, SET_TYPE_STRING);
       break;
      }

      if (hit == 0)
       np = (char **)setadd ((void **)np, module->si->provides[i], SET_TYPE_STRING);
     }

     einit_module_transformations_garbage_add_chunk (module->si->provides);
     module->si->provides = np;
    }

    if (module->si->requires) {
     char **np = NULL;

     for (i = 0; module->si->requires[i]; i++) {
      char hit = 0;
      struct stree *x = streefind (service_transformations, module->si->requires[i], tree_find_first);

      while (x) {
       struct service_transformation *trans =
         (struct service_transformation *)x->value;

       if (regexec (trans->id_pattern, module->module->rid, 0, NULL, 0)) {
        x = streefind (x, module->si->requires[i], tree_find_next);
        continue;
       }

       hit = 1;

       if (trans->options & SVT_STRIP_REQUIRES) break;

       np = (char **)setadd ((void **)np, trans->out, SET_TYPE_STRING);
       break;
      }

      if (hit == 0)
       np = (char **)setadd ((void **)np, module->si->requires[i], SET_TYPE_STRING);
     }

     einit_module_transformations_garbage_add_chunk (module->si->requires);
     module->si->requires = np;
    }

    if (module->si->after) {
     char **np = NULL;

     for (i = 0; module->si->after[i]; i++) {
      char hit = 0;
      struct stree *x = streefind (service_transformations, module->si->after[i], tree_find_first);

      while (x) {
       struct service_transformation *trans =
         (struct service_transformation *)x->value;

       if (regexec (trans->id_pattern, module->module->rid, 0, NULL, 0)) {
        x = streefind (x, module->si->after[i], tree_find_next);
        continue;
       }

       hit = 1;

       if (trans->options & SVT_STRIP_AFTER) break;

       np = (char **)setadd ((void **)np, trans->out, SET_TYPE_STRING);
       break;
      }

      if (hit == 0)
       np = (char **)setadd ((void **)np, module->si->after[i], SET_TYPE_STRING);
     }

     einit_module_transformations_garbage_add_chunk (module->si->after);
     module->si->after = np;
    }

    if (module->si->before) {
     char **np = NULL;

     for (i = 0; module->si->before[i]; i++) {
      char hit = 0;
      struct stree *x = streefind (service_transformations, module->si->before[i], tree_find_first);

      while (x) {
       struct service_transformation *trans =
         (struct service_transformation *)x->value;

       if (regexec (trans->id_pattern, module->module->rid, 0, NULL, 0)) {
        x = streefind (x, module->si->before[i], tree_find_next);
        continue;
       }

       hit = 1;

       if (trans->options & SVT_STRIP_BEFORE) break;

       np = (char **)setadd ((void **)np, trans->out, SET_TYPE_STRING);
       break;
      }

      if (hit == 0)
       np = (char **)setadd ((void **)np, module->si->before[i], SET_TYPE_STRING);
     }

     einit_module_transformations_garbage_add_chunk (module->si->before);
     module->si->before = np;
    }

    if (module->si->shutdown_before) {
     char **np = NULL;

     for (i = 0; module->si->shutdown_before[i]; i++) {
      char hit = 0;
      struct stree *x = streefind (service_transformations, module->si->shutdown_before[i], tree_find_first);

      while (x) {
       struct service_transformation *trans =
         (struct service_transformation *)x->value;

       if (regexec (trans->id_pattern, module->module->rid, 0, NULL, 0)) {
        x = streefind (x, module->si->shutdown_before[i], tree_find_next);
        continue;
       }

       hit = 1;

       if (trans->options & SVT_STRIP_SD_BEFORE) break;

       np = (char **)setadd ((void **)np, trans->out, SET_TYPE_STRING);
       break;
      }

      if (hit == 0)
       np = (char **)setadd ((void **)np, module->si->shutdown_before[i], SET_TYPE_STRING);
     }

     einit_module_transformations_garbage_add_chunk (module->si->shutdown_before);
     module->si->shutdown_before = np;
    }

    if (module->si->shutdown_after) {
     char **np = NULL;

     for (i = 0; module->si->shutdown_after[i]; i++) {
      char hit = 0;
      struct stree *x = streefind (service_transformations, module->si->shutdown_after[i], tree_find_first);

      while (x) {
       struct service_transformation *trans =
         (struct service_transformation *)x->value;

       if (regexec (trans->id_pattern, module->module->rid, 0, NULL, 0)) {
        x = streefind (x, module->si->shutdown_after[i], tree_find_next);
        continue;
       }

       hit = 1;

       if (trans->options & SVT_STRIP_SD_AFTER) break;

       np = (char **)setadd ((void **)np, trans->out, SET_TYPE_STRING);
       break;
      }

      if (hit == 0)
       np = (char **)setadd ((void **)np, module->si->shutdown_after[i], SET_TYPE_STRING);
     }

     einit_module_transformations_garbage_add_chunk (module->si->shutdown_after);
     module->si->shutdown_after = np;
    }
   }
#endif
 }

 einit_module_transformations_usage--;
}

int einit_module_transformations_cleanup (struct lmodule *r) {
 event_ignore (einit_event_subsystem_core, einit_module_transformations_einit_event_handler);

 return 0;
}

int einit_module_transformations_suspend (struct lmodule *r) {
 if (!einit_module_transformations_usage) {
  event_wakeup (einit_core_configuration_update, r);
  event_wakeup (einit_core_update_module, r);
  event_ignore (einit_event_subsystem_core, einit_module_transformations_einit_event_handler);

  einit_module_transformations_garbage_free();

  return status_ok;
 } else {
  return status_failed;
 }
}

int einit_module_transformations_resume (struct lmodule *r) {
 event_wakeup_cancel (einit_core_configuration_update, r);
 event_wakeup_cancel (einit_core_update_module, r);

 return status_ok;
}

int einit_module_transformations_configure (struct lmodule *r) {
 module_init (r);

 thismodule->cleanup = einit_module_transformations_cleanup;
 thismodule->suspend = einit_module_transformations_suspend;
 thismodule->resume = einit_module_transformations_resume;

 event_listen (einit_event_subsystem_core, einit_module_transformations_einit_event_handler);

 return 0;
}
