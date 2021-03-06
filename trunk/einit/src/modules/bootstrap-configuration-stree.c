/*
 *  bootstrap-configuration-stree.c
 *  einit
 *
 *  Created by Magnus Deininger on 06/02/2006.
 *  Split from config-xml-expat.c on 22/10/2006
 *  Renamed/moved from config.c on 20/03/2007
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
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <einit/bitch.h>
#include <einit/config.h>
#include <einit/utility.h>
#include <einit/tree.h>
#include <einit/event.h>

#ifdef POSIXREGEX
#include <regex.h>
#endif

int bootstrap_einit_configuration_stree_configure (struct lmodule *);

#if defined(EINIT_MODULE) || defined(EINIT_MODULE_HEADER)
const struct smodule bootstrap_einit_configuration_stree_self = {
 .eiversion = EINIT_VERSION,
 .eibuild   = BUILDNUMBER,
 .version   = 1,
 .mode      = 0,
 .name      = "Core Configuration Storage and Retrieval (stree-based)",
 .rid       = "einit-bootstrap-configuration-stree",
 .si        = {
  .provides = NULL,
  .requires = NULL,
  .after    = NULL,
  .before   = NULL
 },
 .configure = bootstrap_einit_configuration_stree_configure
};

module_register(bootstrap_einit_configuration_stree_self);

#endif

int bootstrap_einit_configuration_stree_usage = 0;

struct {
 void **chunks;
} cfg_stree_garbage = {
 .chunks = NULL
};

pthread_mutex_t cfg_stree_garbage_mutex = PTHREAD_MUTEX_INITIALIZER;

void cfg_stree_garbage_add_chunk (void *chunk) {
 if (!chunk) return;
 emutex_lock (&cfg_stree_garbage_mutex);
 if (!cfg_stree_garbage.chunks || (!inset ((const void **)cfg_stree_garbage.chunks, chunk, SET_NOALLOC)))
  cfg_stree_garbage.chunks = setadd (cfg_stree_garbage.chunks, chunk, SET_NOALLOC);
 emutex_unlock (&cfg_stree_garbage_mutex);
}

void cfg_stree_garbage_free () {
 emutex_lock (&cfg_stree_garbage_mutex);
 if (cfg_stree_garbage.chunks) {
  int i = 0;

  for (; cfg_stree_garbage.chunks[i]; i++) {
   free (cfg_stree_garbage.chunks[i]);
  }

  free (cfg_stree_garbage.chunks);
  cfg_stree_garbage.chunks = NULL;
 }
 emutex_unlock (&cfg_stree_garbage_mutex);
}

int cfg_free () {
 bootstrap_einit_configuration_stree_usage++;

 struct stree *cur = hconfiguration;
 struct cfgnode *node = NULL;
 while (cur) {
  if ((node = (struct cfgnode *)cur->value)) {
   if (node->id)
    free (node->id);
   if (node->path)
    free (node->path);
  }
  cur = streenext (cur);
 }
 streefree (hconfiguration);
 hconfiguration = NULL;

 bootstrap_einit_configuration_stree_usage--;
 return 1;
}

int cfg_addnode_f (struct cfgnode *node) {
 bootstrap_einit_configuration_stree_usage++;

 if (!node || !node->id) {
  bootstrap_einit_configuration_stree_usage--;
  return -1;
 }
 struct stree *cur = hconfiguration;
 char doop = 1;

 if (node->arbattrs) {
  uint32_t r = 0;
  for (; node->arbattrs[r]; r+=2) {
   if (strmatch ("id", node->arbattrs[r])) node->idattr = node->arbattrs[r+1];
  }
 }

 if (node->type & einit_node_mode) {
/* mode definitions only need to be modified -- it doesn't matter if there's more than one, but
  only the first one would be used anyway. */
  if (cur) cur = streefind (cur, node->id, tree_find_first);
  while (cur) {
   if (cur->value && !(((struct cfgnode *)cur->value)->type ^ einit_node_mode)) {
// this means we found something that looks like it
    void *bsl = cur->luggage;

// we risk not being atomic at this point but... it really is unlikely to go weird.
    ((struct cfgnode *)cur->value)->arbattrs = node->arbattrs;
    cur->luggage = node->arbattrs;

    free (bsl);

    doop = 0;

    break;
   }
//   cur = streenext (cur);
   cur = streefind (cur, node->id, tree_find_next);
  }
 } else {
/* look for other definitions that are exactly the same, only marginally different or that sport a
   matching id="" attribute */

  if (cur) cur = streefind (cur, node->id, tree_find_first);
  while (cur) {
// this means we found a node wit the same path
   if (cur->value && ((struct cfgnode *)cur->value)->idattr && node->idattr &&
       strmatch (((struct cfgnode *)cur->value)->idattr, node->idattr)) {
// NTS: implement checks to figure out if the node is similar

// this means we found something that looks like it
    cfg_stree_garbage_add_chunk (cur->luggage);
    cfg_stree_garbage_add_chunk (((struct cfgnode *)cur->value)->arbattrs);

    ((struct cfgnode *)cur->value)->arbattrs = node->arbattrs;
    cur->luggage = node->arbattrs;

    ((struct cfgnode *)cur->value)->type        = node->type;
    ((struct cfgnode *)cur->value)->mode        = node->mode;
    ((struct cfgnode *)cur->value)->flag        = node->flag;
    ((struct cfgnode *)cur->value)->value       = node->value;
    ((struct cfgnode *)cur->value)->svalue      = node->svalue;
    ((struct cfgnode *)cur->value)->idattr      = node->idattr;
    ((struct cfgnode *)cur->value)->path        = node->path;

    doop = 0;

    break;
   }
//   cur = streenext (cur);
   cur = streefind (cur, node->id, tree_find_next);
  }
 }

 if (doop) {
  hconfiguration = streeadd (hconfiguration, node->id, node, sizeof(struct cfgnode), node->arbattrs);

  einit_new_node = 1;
 }

/* hmmm.... */
/* cfg_stree_garbage_add_chunk (node->arbattrs);*/
 cfg_stree_garbage_add_chunk (node->id);

 bootstrap_einit_configuration_stree_usage--;
 return 0;
}

struct cfgnode *cfg_findnode_f (const char *id, enum einit_cfg_node_options type, const struct cfgnode *base) {
 bootstrap_einit_configuration_stree_usage++;

 struct stree *cur = hconfiguration;
 if (!id) {
  bootstrap_einit_configuration_stree_usage--;
  return NULL;
 }

 if (base) {
  if (cur) cur = streefind (cur, id, tree_find_first);
  while (cur) {
   if (cur->value == base) {
    cur = streefind (cur, id, tree_find_next);
    break;
   }
//   cur = streenext (cur);
    cur = streefind (cur, id, tree_find_next);
  }
 } else if (cur) {
  cur = streefind (cur, id, tree_find_first);
 }

 while (cur) {
  if (cur->value && (!type || !(((struct cfgnode *)cur->value)->type ^ type))) {
   bootstrap_einit_configuration_stree_usage--;
   return cur->value;
  }
  cur = streefind (cur, id, tree_find_next);
 }

 bootstrap_einit_configuration_stree_usage--;
 return NULL;
}

// get string (by id)
char *cfg_getstring_f (const char *id, const struct cfgnode *mode) {
 bootstrap_einit_configuration_stree_usage++;
 struct cfgnode *node = NULL;
 char *ret = NULL, **sub;
 uint32_t i;

 if (!id) {
  bootstrap_einit_configuration_stree_usage--;
  return NULL;
 }
 mode = mode ? mode : cmode;

 if (strchr (id, '/')) {
  char f = 0;
  sub = str2set ('/', id);
  if (!sub[1]) {
   node = cfg_getnode (id, mode);
   if (node)
    ret = node->svalue;

   free (sub);
   bootstrap_einit_configuration_stree_usage--;
   return ret;
  }

  node = cfg_getnode (sub[0], mode);
  if (node && node->arbattrs && node->arbattrs[0]) {
   if (node->arbattrs)

   for (i = 0; node->arbattrs[i]; i+=2) {
    if ((f = (strmatch(node->arbattrs[i], sub[1])))) {
     ret = node->arbattrs[i+1];
     break;
    }
   }
  }

  free (sub);
 } else {
  node = cfg_getnode (id, mode);
  if (node)
   ret = node->svalue;
 }

 bootstrap_einit_configuration_stree_usage--;
 return ret;
}

// get node (by id)
struct cfgnode *cfg_getnode_f (const char *id, const struct cfgnode *mode) {
 bootstrap_einit_configuration_stree_usage++;
 struct cfgnode *node = NULL;
 struct cfgnode *ret = NULL;

 if (!id) {
  bootstrap_einit_configuration_stree_usage--;
  return NULL;
 }
 mode = mode ? mode : cmode;

 if (mode) {
  char *tmpnodename = NULL;
  tmpnodename = emalloc (6+strlen (id));
  *tmpnodename = 0;

  strcat (tmpnodename, "mode-");
  strcat (tmpnodename, id);

  while ((node = cfg_findnode (tmpnodename, 0, node))) {
   if (node->mode == mode) {
    ret = node;
    break;
   }
  }

  free (tmpnodename);

  tmpnodename = emalloc (16+strlen (id));
  *tmpnodename = 0;

  strcat (tmpnodename, "mode-overrides-");
  strcat (tmpnodename, id);

  while ((node = cfg_findnode (tmpnodename, 0, node))) {
   if (node->mode == mode) {
    ret = node;
    break;
   }
  }

  free (tmpnodename);
 }

 if (!ret && (node = cfg_findnode (id, 0, NULL)))
  ret = node;

 bootstrap_einit_configuration_stree_usage--;
 return ret;
}

// return a new stree with the filter applied
struct stree *cfg_filter_f (const char *filter, enum einit_cfg_node_options type) {
 bootstrap_einit_configuration_stree_usage++;
 struct stree *retval = NULL;

#ifdef POSIXREGEX
 if (filter) {
  struct stree *cur = hconfiguration;
  regex_t pattern;
  if (!eregcomp(&pattern, filter)) {
   while (cur) {
    if (!regexec (&pattern, cur->key, 0, NULL, 0) &&
        (!type || (((struct cfgnode *)(cur->value))->type & type))) {
     retval = streeadd (retval, cur->key, cur->value, SET_NOALLOC, NULL);
    }
    cur = streenext (cur);
   }

   regfree (&pattern);
  }
 }
#endif

 bootstrap_einit_configuration_stree_usage--;
 return retval;
}

// return a new stree with a certain prefix applied
struct stree *cfg_prefix_f (const char *prefix) {
 bootstrap_einit_configuration_stree_usage++;
 struct stree *retval = NULL;

#ifdef POSIXREGEX
 if (prefix) {
  struct stree *cur = hconfiguration;
  while (cur) {
   if (strstr(cur->key, prefix) == cur->key) {
    retval = streeadd (retval, cur->key, cur->value, SET_NOALLOC, NULL);
   }
   cur = streenext (cur);
  }
 }
#endif

 bootstrap_einit_configuration_stree_usage--;
 return retval;
}

/* those i-could've-sworn-there-were-library-functions-for-that functions */
char *cfg_getpath_f (const char *id) {
 bootstrap_einit_configuration_stree_usage++;
 int mplen;
 char *svpath = cfg_getstring (id, NULL);
 if (!svpath) {
  bootstrap_einit_configuration_stree_usage--;
  return NULL;
 }
 mplen = strlen (svpath) +1;
 if (svpath[mplen-2] != '/') {
//  if (svpath->path) return svpath->path;
  char *tmpsvpath = (char *)emalloc (mplen+1);
  tmpsvpath[0] = 0;

  strcat (tmpsvpath, svpath);
  tmpsvpath[mplen-1] = '/';
  tmpsvpath[mplen] = 0;
//  svpath->svalue = tmpsvpath;
//  svpath->path = tmpsvpath;
  bootstrap_einit_configuration_stree_usage--;
  return tmpsvpath;
 }
 bootstrap_einit_configuration_stree_usage--;
 return svpath;
}

void bootstrap_einit_configuration_stree_einit_event_handler (struct einit_event *ev) {
 bootstrap_einit_configuration_stree_usage++;
 if (ev->type == einit_core_configuration_update) {
// update global environment here
  char **env = einit_global_environment;
  einit_global_environment = NULL;
  struct cfgnode *node = NULL;
  free (env);

  env = NULL;
  while ((node = cfg_findnode ("configuration-environment-global", 0, node))) {
   if (node->idattr && node->svalue) {
    env = straddtoenviron (env, node->idattr, node->svalue);
   }
  }
  einit_global_environment = env;
 }
 bootstrap_einit_configuration_stree_usage--;
}

void bootstrap_einit_configuration_stree_ipc_event_handler (struct einit_event *ev) {
 if ((ev->argc >= 2) && (ev->ipc_options & einit_ipc_output_xml)) {
  if (strmatch (ev->argv[0], "list") && strmatch (ev->argv[1], "modes")) {
   struct stree *cur = hconfiguration;
   struct cfgnode **modes = NULL;

   ev->implemented = 1;

/* first, we need to find all the modes we have */
   while (cur) {
    struct cfgnode *node = cur->value;

    if (node && node->mode) {
     if (!inset ((const void **)modes, (const void *)node->mode, SET_NOALLOC)) {
      modes = (struct cfgnode **)setadd ((void **)modes, (void *)node->mode, SET_NOALLOC);
     }
    }

    cur = streenext(cur);
   }

   if (modes) {
    uint32_t i = 0;

    for (; modes[i]; i++) {
     if (!(modes[i]->arbattrs)) {
      fprintf (ev->output, " <mode id=\"%s\">", modes[i]->id);
     } else {
      uint32_t y = 0;
//      fprintf (ev->output, " <mode", modes[i]->id);
      eputs (" <mode", ev->output);

      for (; modes[i]->arbattrs[y]; y+=2) {
//       char *escapedn = escape_xml (modes[i]->arbattrs[y]);
       char *escapeda = escape_xml (modes[i]->arbattrs[y+1]);

       fprintf (ev->output, " %s=\"%s\"", modes[i]->arbattrs[y], escapeda);

       free (escapeda);
      }

      eputs (">\n", ev->output);
     }

     char *tmp;

     if ((tmp = cfg_getstring ("enable/services", modes[i]))) {
      char *escaped_s = escape_xml (tmp);
      char *tmpx = cfg_getstring ("enable/critical", modes[i]);

      if (tmpx) {
       char *escaped_x = escape_xml (tmpx);
       fprintf (ev->output, "  <enable services=\"%s\" critical=\"%s\" />\n", escaped_s, escaped_x);

       free (tmpx);
      } else {
       fprintf (ev->output, "  <enable services=\"%s\" />\n", escaped_s);
      }

      free (escaped_s);
     }

     if ((tmp = cfg_getstring ("disable/services", modes[i]))) {
      char *escaped_s = escape_xml (tmp);
      char *tmpx = cfg_getstring ("disable/critical", modes[i]);

      if (tmpx) {
       char *escaped_x = escape_xml (tmpx);
       fprintf (ev->output, "  <disable services=\"%s\" critical=\"%s\" />\n", escaped_s, escaped_x);

       free (tmpx);
      } else {
       fprintf (ev->output, "  <disable services=\"%s\" />\n", escaped_s);
      }

      free (escaped_s);
     }

     eputs (" </mode>\n", ev->output);
    }
   }
  }
 }
}

int bootstrap_einit_configuration_stree_suspend (struct lmodule *this) {
 if (!bootstrap_einit_configuration_stree_usage) {
//  cfg_stree_garbage_free();
  sleep (1);

  if (!bootstrap_einit_configuration_stree_usage) {
   event_ignore (einit_event_subsystem_core, bootstrap_einit_configuration_stree_einit_event_handler);
   event_ignore (einit_event_subsystem_ipc, bootstrap_einit_configuration_stree_ipc_event_handler);

   event_wakeup (einit_core_configuration_update, this);
   event_wakeup (einit_event_subsystem_ipc, this);

/*   function_unregister ("einit-configuration-node-add", 1, cfg_addnode_f);
   function_unregister ("einit-configuration-node-get", 1, cfg_getnode_f);
   function_unregister ("einit-configuration-node-get-string", 1, cfg_getstring_f);
   function_unregister ("einit-configuration-node-get-find", 1, cfg_findnode_f);
   function_unregister ("einit-configuration-node-get-filter", 1, cfg_filter_f);
   function_unregister ("einit-configuration-node-get-path", 1, cfg_getpath_f);
   function_unregister ("einit-configuration-node-get-prefix", 1, cfg_prefix_f);*/

   sleep (1);
  } else {
   return status_failed;
  }

  return status_ok;
 } else {
  return status_failed;
 }
}

int bootstrap_einit_configuration_stree_resume (struct lmodule *this) {
 return status_ok;
}

int bootstrap_einit_configuration_stree_cleanup (struct lmodule *tm) {
 cfg_free();

 event_ignore (einit_event_subsystem_core, bootstrap_einit_configuration_stree_einit_event_handler);
 event_ignore (einit_event_subsystem_ipc, bootstrap_einit_configuration_stree_ipc_event_handler);

 function_unregister ("einit-configuration-node-add", 1, cfg_addnode_f);
 function_unregister ("einit-configuration-node-get", 1, cfg_getnode_f);
 function_unregister ("einit-configuration-node-get-string", 1, cfg_getstring_f);
 function_unregister ("einit-configuration-node-get-find", 1, cfg_findnode_f);
 function_unregister ("einit-configuration-node-get-filter", 1, cfg_filter_f);
 function_unregister ("einit-configuration-node-get-path", 1, cfg_getpath_f);
 function_unregister ("einit-configuration-node-get-prefix", 1, cfg_prefix_f);

 return 0;
}

int bootstrap_einit_configuration_stree_configure (struct lmodule *tm) {
 module_init (tm);

 thismodule->cleanup = bootstrap_einit_configuration_stree_cleanup;
// disable suspend/resume for this module for the time being...
/*
 thismodule->suspend = bootstrap_einit_configuration_stree_suspend;
 thismodule->resume = bootstrap_einit_configuration_stree_resume;
*/

 event_listen (einit_event_subsystem_ipc, bootstrap_einit_configuration_stree_ipc_event_handler);
 event_listen (einit_event_subsystem_core, bootstrap_einit_configuration_stree_einit_event_handler);

 function_register ("einit-configuration-node-add", 1, cfg_addnode_f);
 function_register ("einit-configuration-node-get", 1, cfg_getnode_f);
 function_register ("einit-configuration-node-get-string", 1, cfg_getstring_f);
 function_register ("einit-configuration-node-get-find", 1, cfg_findnode_f);
 function_register ("einit-configuration-node-get-filter", 1, cfg_filter_f);
 function_register ("einit-configuration-node-get-path", 1, cfg_getpath_f);
 function_register ("einit-configuration-node-get-prefix", 1, cfg_prefix_f);

 return 0;
}
