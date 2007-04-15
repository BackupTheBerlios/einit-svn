/*
 *  network.c
 *  einit
 *
 *  Created by Magnus Deininger on 06/04/2006.
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
#include <expat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <einit/bitch.h>
#include <einit/config.h>
#include <einit/utility.h>
#include <einit/tree.h>
#include <einit/event.h>
#include <dirent.h>
#include <sys/stat.h>
#include <einit-modules/configuration.h>
#include <einit-modules/exec.h>

#define ECXE_MASTERTAG 0x00000001
#define IF_OK          0x1

struct interface_template_item {
 struct stree *action;
 char **variables;
 char **environment;
 char *pidfile;
};

struct interface_descriptor {
 char *interface_name;
 struct interface_template_item **controller;
 struct interface_template_item **ip_manager;

 uint32_t ci, pi;
 char **variables;
 char *kernel_module;

 struct cfgnode *interface;
};

int network_scanmodules (struct lmodule *);
int network_interface_enable (struct interface_descriptor *, struct einit_event *);
int network_interface_disable (struct interface_descriptor *, struct einit_event *);
int network_interface_custom (struct interface_descriptor *, char *, struct einit_event *);
int network_interface_cleanup (struct lmodule *);
int network_interface_configure (struct lmodule *);
int network_cleanup (struct lmodule *);
int network_configure (struct lmodule *);

struct interface_descriptor *network_import_interface_descriptor (struct lmodule *);
void network_free_interface_descriptor (struct interface_descriptor *);
struct interface_template_item **network_import_templates (char *, char *, struct interface_descriptor *);

#if defined(EINIT_MODULE) || defined(EINIT_MODULE_HEADER)
const struct smodule einit_network_self = {
 .eiversion = EINIT_VERSION,
 .eibuild   = BUILDNUMBER,
 .version   = 1,
 .mode      = EINIT_MOD_LOADER,
 .options   = 0,
 .name      = "Network Interface Configuration",
 .rid       = "network",
 .si        = {
  .provides = NULL,
  .requires = NULL,
  .after    = NULL,
  .before   = NULL
 },
 .configure = network_configure
};

module_register(einit_network_self);

#endif

#if 0
void network_einit_event_handler (struct einit_event *ev) {
 if (ev->type == einit_core_update_configuration) {
  if (configuration_network_parse_and_add_nodes()) {
   ev->chain_type = einit_core_configuration_update;
  }
 }
}
#endif

int network_scanmodules (struct lmodule *mainlist) {
 struct stree *network_nodes = cfg_prefix("configuration-network-interfaces-");

 if (network_nodes) {
  struct stree *cur = network_nodes;

  while (cur) {
   if (cur->value) {
    struct cfgnode *nn = cur->value;

    if (nn->arbattrs) {
     char *interfacename = cur->key+33; /* 33 = strlen("configuration-network-interfaces-") */
     struct smodule *newmodule = emalloc (sizeof (struct smodule));
     char tmp[BUFFERSIZE];
     struct lmodule *lm;

     memset (newmodule, 0, sizeof (struct smodule));

     lm = mainlist;
     while (lm) {
      if (lm->source && strmatch(lm->source, cur->key)) {
       lm = mod_update (lm);

       emutex_lock (&(lm->mutex));

       if (lm->param) {
        uint32_t ci = ((struct interface_descriptor *)lm->param)->ci,
                 pi = ((struct interface_descriptor *)lm->param)->pi;

        network_free_interface_descriptor (lm->param);
        lm->param = network_import_interface_descriptor (lm);

        ((struct interface_descriptor *)lm->param)->ci = ci;
        ((struct interface_descriptor *)lm->param)->pi = pi;
       }

       emutex_unlock (&(lm->mutex));

       goto do_next;
      }

      lm = lm->next;
     }

     newmodule->configure = network_interface_configure;
     esprintf (tmp, BUFFERSIZE, "interface-%s", interfacename);
     newmodule->rid = estrdup (tmp);
     newmodule->eiversion = EINIT_VERSION;
     newmodule->eibuild = BUILDNUMBER;
     newmodule->version = 1;
     newmodule->mode = EINIT_MOD_EXEC;
     newmodule->options = 0;

     esprintf (tmp, BUFFERSIZE, "Network Interface (%s)", interfacename);
     newmodule->name = estrdup (tmp);

     esprintf (tmp, BUFFERSIZE, "net-%s", interfacename);
     newmodule->si.provides = (char **)setadd ((void **)newmodule->si.provides, (void *)tmp, SET_TYPE_STRING);
     newmodule->si.requires = (char **)setadd ((void **)newmodule->si.requires, (void *)"mount-critical", SET_TYPE_STRING);

     lm = mod_add (NULL, newmodule);
    }
   }

   do_next:

   cur = streenext (cur);
  }

  streefree (network_nodes);
 }

 return 0;
}

void network_free_interface_descriptor (struct interface_descriptor *id) {
 if (!id) return;

 if (id->variables) free (id->variables);
 if (id->controller) {
  uint32_t i = 0;
  for (; id->controller[i]; i++) {
   if (id->controller[i]->variables) free (id->controller[i]->variables);
   if (id->controller[i]->environment) free (id->controller[i]->environment);
   if (id->controller[i]->action) streefree (id->controller[i]->action);
  }
 }
 if (id->ip_manager) {
  uint32_t i = 0;
  for (; id->ip_manager[i]; i++) {
   if (id->ip_manager[i]->variables) free (id->ip_manager[i]->variables);
   if (id->ip_manager[i]->environment) free (id->ip_manager[i]->environment);
   if (id->ip_manager[i]->action) streefree (id->ip_manager[i]->action);
  }
 }

 free (id);
}

struct interface_template_item **network_import_templates (char *type, char *list, struct interface_descriptor *id) {
 if (!type || !list || strmatch (list, "none")) return NULL;

 struct interface_template_item **retval = NULL;

 char nodename[BUFFERSIZE];
 char **ll = str2set (':', list);
 uint32_t i = 0;

 if (!ll) return NULL;

 char **if_vars = (char **)setdup ((const void **)id->interface->arbattrs, SET_TYPE_STRING);

 if_vars = (char **)setadd((void **)if_vars, (void *)"interface", SET_TYPE_STRING);
 if_vars = (char **)setadd((void **)if_vars, (void *)id->interface_name, SET_TYPE_STRING);

 esprintf (nodename, BUFFERSIZE, "services-virtual-network-%s", type);

 for (; ll[i]; i++) {
  struct cfgnode *node = NULL;

  while ((node = cfg_findnode (nodename, 0, node))) {
   if (node->idattr && node->arbattrs && strmatch(node->idattr, ll[i])) {
    uint32_t y = 0;
    struct interface_template_item ni;
    memset (&ni, 0, sizeof (struct interface_template_item));

    for (; node->arbattrs[y]; y+=2) {
     char *name = NULL;
     if (((strmatch (node->arbattrs[y], "enable") || strmatch (node->arbattrs[y], "disable")) && (name = node->arbattrs[y])) ||
         ((strstr (node->arbattrs[y], "execute:") == node->arbattrs[y]) && (name = node->arbattrs[y]+8))) {
      char *tmp = apply_variables (node->arbattrs[y+1], (const char **)if_vars);
      struct stree *streeadd (const struct stree *stree, const char *key, const void *value, int32_t vlen, const void *luggage);

      if (tmp) {
       ni.action = streeadd (ni.action, name, tmp, SET_TYPE_STRING, NULL);

       free (tmp);
      }
     } else if (strmatch (node->arbattrs[y], "variables")) {
      char *tmp = apply_variables (node->arbattrs[y+1], (const char **)if_vars);
      ni.variables = str2set (':', tmp);
      free (tmp);
     } else if (strmatch (node->arbattrs[y], "pid")) {
      char *tmp = apply_variables (node->arbattrs[y+1], (const char **)if_vars);
      ni.pidfile = tmp;
      ni.environment = straddtoenviron(ni.environment, "pidfile", tmp);
     } else {
      char *tmp = apply_variables (node->arbattrs[y+1], (const char **)if_vars);
      ni.environment = straddtoenviron(ni.environment, node->arbattrs[y], tmp);
      free (tmp);
     }
    }

    if (ni.action) {
     retval = (struct interface_template_item **)setadd ((void **)retval, (void *)&ni, sizeof (struct interface_template_item));
    }
   }
  }
 }

 free (if_vars);
 free (ll);

 return retval;
}

struct interface_descriptor *network_import_interface_descriptor (struct lmodule *lm) {
 struct interface_descriptor *id = ecalloc (1, sizeof (struct interface_descriptor));
 char nodename[BUFFERSIZE];


 id->interface_name = lm->module->rid+10;

 esprintf (nodename, BUFFERSIZE, "configuration-network-interfaces-%s", id->interface_name);

// eputs (nodename, stderr);
 id->interface = cfg_getnode (nodename, NULL);

 if (id->interface && id->interface->arbattrs) {
  uint32_t i = 0;

  for (; id->interface->arbattrs[i]; i+=2) {
   if (strmatch (id->interface->arbattrs[i], "ip")) {
    id->ip_manager = network_import_templates ("ip", id->interface->arbattrs[i+1], id);
   } else if (strmatch (id->interface->arbattrs[i], "control")) {
    id->controller = network_import_templates ("ifctl", id->interface->arbattrs[i+1], id);
   } else if (strmatch (id->interface->arbattrs[i], "kernel-module")) {
    id->kernel_module = id->interface->arbattrs[i+1];
   }
  }
 } else {
  free (id);
  return NULL;
 }

 if (!id->ip_manager) {
  network_free_interface_descriptor (id);
  return NULL;
 }

 return id;
}

// pexec(command, variables, uid, gid, user, group, local_environment, status)

int network_interface_enable (struct interface_descriptor *id, struct einit_event *status) {
 uint32_t ci = 0, pi = 0;

 if (!id && !(id = network_import_interface_descriptor(status->para))) return STATUS_FAIL;

 if (id->kernel_module) {
  char *command = cfg_getstring ("configuration-command-modprobe/with-env", NULL),
       tmp[BUFFERSIZE], *nc, **cx = NULL;

  if (command) {
   cx = (char **)setadd ((void **)cx, (void *)"module", SET_TYPE_STRING);
   cx = (char **)setadd ((void **)cx, (void *)id->kernel_module, SET_TYPE_STRING);

   if (cx) {
    nc = apply_variables (command, (const char **)cx);
    if (nc) {

     esprintf (tmp, BUFFERSIZE, "Loading Kernel Module (%s)", id->kernel_module);
     status->string = tmp;
     status_update (status);

     if (!(pexec (nc, NULL, 0, 0, NULL, NULL, NULL, status) & STATUS_OK))
      return STATUS_FAIL;

     free (nc);
    }
    free (cx);
   }
  }
 }

 if (id->controller) { // == NULL if ip not mentioned or ="none"
  for (ci = 0; id->controller[ci]; ci++) {
   if (id->controller[ci]->pidfile) unlink (id->controller[ci]->pidfile);
   struct stree *t = streefind (id->controller[ci]->action, "enable", tree_find_first);

   if (t) {
    if (pexec (t->value, (const char **)id->controller[ci]->variables, 0, 0, NULL, NULL, id->controller[ci]->environment, status) & STATUS_OK) {
     id->ci = ci;
     goto ip;
    }
   }
  }

  return STATUS_FAIL;
 }

 ip:

 for (pi = 0; id->ip_manager[pi]; pi++) {
  if (id->ip_manager[pi]->pidfile) unlink (id->ip_manager[pi]->pidfile);
  struct stree *t = streefind (id->ip_manager[pi]->action, "enable", tree_find_first);

  if (t) {
   if (pexec (t->value, (const char **)id->ip_manager[pi]->variables, 0, 0, NULL, NULL, id->ip_manager[pi]->environment, status) & STATUS_OK) {
    id->pi = pi;
    return STATUS_OK;
   }
  }
 }


 return STATUS_FAIL;
}

int network_interface_disable (struct interface_descriptor *id, struct einit_event *status) {
 if (!id && !(id = network_import_interface_descriptor(status->para))) return STATUS_FAIL;

 if (id->ip_manager[id->pi]) {
  struct stree *t = streefind (id->ip_manager[id->pi]->action, "disable", tree_find_first);

  if (t) {
   if (pexec (t->value, (const char **)id->ip_manager[id->pi]->variables, 0, 0, NULL, NULL, id->ip_manager[id->pi]->environment, status) & STATUS_OK) {
    if (id->ip_manager[id->pi]->pidfile) unlink (id->ip_manager[id->pi]->pidfile);

    goto controller;
   }
  }
 }

 return STATUS_FAIL;

 controller:
 if (id->controller && id->controller[id->ci]) { // == NULL if ip not mentioned or ="none"
  struct stree *t = streefind (id->controller[id->ci]->action, "disable", tree_find_first);

  if (t) {
   if (pexec (t->value, (const char **)id->controller[id->ci]->variables, 0, 0, NULL, NULL, id->controller[id->ci]->environment, status) & STATUS_OK) {
    if (id->controller[id->ci]->pidfile) unlink (id->controller[id->ci]->pidfile);

    return STATUS_OK;
   }
  }

  return STATUS_FAIL;
 }

 return STATUS_OK;
}

int network_interface_custom (struct interface_descriptor *id, char *action, struct einit_event *status) {
 if (!id && !(id = network_import_interface_descriptor(status->para))) return STATUS_FAIL;

 if (id->ip_manager && id->ip_manager[id->pi]) {
  struct stree *t = streefind (id->ip_manager[id->pi]->action, action, tree_find_first);

  if (t) {
   pexec (t->value, (const char **)id->ip_manager[id->pi]->variables, 0, 0, NULL, NULL, id->ip_manager[id->pi]->environment, status);
  }
 }

 if (id->controller && id->controller[id->ci]) {
  struct stree *t = streefind (id->controller[id->ci]->action, action, tree_find_first);

  if (t) {
   pexec (t->value, (const char **)id->controller[id->ci]->variables, 0, 0, NULL, NULL, id->controller[id->ci]->environment, status);
  }
 }

 return STATUS_OK | STATUS_ENABLED;
}

int network_cleanup (struct lmodule *this) {
 exec_cleanup (this);

 return 0;
}

int network_interface_cleanup (struct lmodule *this) {
 return 0;
}

int network_interface_configure (struct lmodule *tm) {
 if (!tm->module || !tm->module->rid) return 1;

// eprintf (stderr, "new network module: %s\n", tm->module->rid);

 tm->cleanup = network_interface_cleanup;
 tm->enable = (int (*)(void *, struct einit_event *))network_interface_enable;
 tm->disable = (int (*)(void *, struct einit_event *))network_interface_disable;
 tm->custom = (int (*)(void *, char *, struct einit_event *))network_interface_custom;

 tm->param = NULL;

 tm->source = estrdup(tm->module->rid);

 return 0;
}

int network_configure (struct lmodule *this) {
 module_init(this);
 exec_configure (this);

 thismodule->cleanup = network_cleanup;
 thismodule->scanmodules = network_scanmodules;

#if 0
 event_listen (einit_event_subsystem_core, network_einit_event_handler);
#endif

 return 0;
}
