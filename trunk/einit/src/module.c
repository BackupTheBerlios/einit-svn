/*
 *  module.c
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

#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <dlfcn.h>
#include <string.h>
#include <einit/bitch.h>
#include <einit/config.h>
#include <einit/module.h>
#include <einit/utility.h>
#include <einit/tree.h>
#include <pthread.h>
#include <errno.h>

#ifdef POSIXREGEX
#include <regex.h>
#endif

struct lmodule *mlist = NULL;

pthread_mutex_t mlist_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t modules_update_mutex = PTHREAD_MUTEX_INITIALIZER;

struct stree *service_usage = NULL;
pthread_mutex_t service_usage_mutex = PTHREAD_MUTEX_INITIALIZER;

int mod_scanmodules ( void ) {
 DIR *dir;
 struct dirent *entry;
 char *tmp;
 int mplen;
 void *sohandle;
 struct lmodule *cmod = NULL, *nmod;
#ifdef POSIXREGEX
 unsigned char freeallow = 0, freedisallow = 0;
 regex_t allowpattern, disallowpattern;
 unsigned char haveallowpattern = 0, havedisallowpattern = 0;
 char *spattern = NULL;
#endif

 pthread_mutex_lock (&modules_update_mutex);

 char *modulepath = cfg_getpath ("core-settings-module-path");
 if (!modulepath) {
  pthread_mutex_unlock (&modules_update_mutex);
  return -1;
 }
#ifdef SANDBOX
// override module path in sandbox-mode to be relative
 if (modulepath[0] == '/') modulepath++;
#endif

#ifdef POSIXREGEX
 if (spattern = cfg_getstring ("core-settings-module-load/pattern-allow", NULL)) {
  uint32_t err;

  if (!(err = regcomp (&allowpattern, spattern, REG_EXTENDED)))
   haveallowpattern = 1;
  else {
   char errorcode [1024];
   regerror (err, &allowpattern, errorcode, 1024);
   fputs (errorcode, stderr);
  }
 }

 if (spattern = cfg_getstring ("core-settings-module-load/pattern-disallow", NULL)) {
  uint32_t err;

  if (!(err = regcomp (&disallowpattern, spattern, REG_EXTENDED)))
   havedisallowpattern = 1;
  else {
   char errorcode [1024];
   regerror (err, &disallowpattern, errorcode, 1024);
   fputs (errorcode, stderr);
  }
 }
#endif

 mplen = strlen (modulepath) +4;
 dir = opendir (modulepath);
 if (dir != NULL) {
  while (entry = readdir (dir)) {
//   uint32_t el = 0;
// if we have posix regular expressions, match them against the filename, if not, exclude '.'-files
#ifdef POSIXREGEX
   if (haveallowpattern && regexec (&allowpattern, entry->d_name, 0, NULL, 0)) continue;
   if (havedisallowpattern && !regexec (&disallowpattern, entry->d_name, 0, NULL, 0)) continue;
#else
   if (entry->d_name[0] == '.') continue;
#endif

//   tmp = (char *)emalloc (el = (((mplen + strlen (entry->d_name))) & (~3))+4);
   tmp = (char *)emalloc (mplen + strlen (entry->d_name));
   struct stat sbuf;
   struct smodule *modinfo;
   struct lmodule *lm;
   *tmp = 0;
   strcat (tmp, modulepath);
   strcat (tmp, entry->d_name);
   dlerror ();
   if (stat (tmp, &sbuf) || !S_ISREG (sbuf.st_mode)) {
    goto cleanup_continue;
   }

   lm = mlist;
   while (lm) {
    if (lm->source && !strcmp(lm->source, tmp)) {
     lm = mod_update (lm);

// tell module to scan for changes if it's a module-loader
     if (lm->module && lm->sohandle && (lm->module->mode & EINIT_MOD_LOADER)) {
      int (*scanfunc)(struct lmodule *) = (int (*)(struct lmodule *)) dlsym (lm->sohandle, "scanmodules");
      if (scanfunc != NULL) {
       scanfunc (mlist);
      }
      else bitch(BTCH_ERRNO + BTCH_DL);
     }

     goto cleanup_continue;
    }
    lm = lm->next;
   }

//   printf ("module %s(%i) not found, loading.\n", tmp, el);
   sohandle = dlopen (tmp, RTLD_NOW);
   if (sohandle == NULL) {
    puts (dlerror ());
//    free (tmp);
//    continue;
    goto cleanup_continue;
   }

//   printf ("checking for self-identifier in module %s.\n", tmp);
   modinfo = (struct smodule *)dlsym (sohandle, "self");
   if (modinfo != NULL) {
    struct lmodule *new = mod_add (sohandle, modinfo);
    if (new) {
//     fprintf (stderr, "einit-module-loader: module added: %s\n", tmp);
     new->source = estrdup(tmp);
    }
   } else
    dlclose (sohandle);

   cleanup_continue:
   free (tmp);
  }
  closedir (dir);
 } else {
  fputs ("couldn't open module directory\n", stderr);

#ifdef POSIXREGEX
  if (haveallowpattern) { haveallowpattern = 0; regfree (&allowpattern); }
  if (havedisallowpattern) { havedisallowpattern = 0; regfree (&disallowpattern); }
#endif

  pthread_mutex_unlock (&modules_update_mutex);
  return bitch(BTCH_ERRNO);
 }

#ifdef POSIXREGEX
 if (haveallowpattern) { haveallowpattern = 0; regfree (&allowpattern); }
 if (havedisallowpattern) { havedisallowpattern = 0; regfree (&disallowpattern); }
#endif

 pthread_mutex_unlock (&modules_update_mutex);
 return 1;
}

void mod_freedesc (struct lmodule *m) {
 pthread_mutex_lock (&m->mutex);
 pthread_mutex_lock (&m->imutex);

 if (m->next != NULL)
  mod_freedesc (m->next);

 m->next = NULL;
 if (m->status & STATUS_ENABLED) {
  pthread_mutex_unlock (&m->imutex);
  mod (MOD_DISABLE | MOD_IGNORE_DEPENDENCIES | MOD_NOMUTEX, m);
  pthread_mutex_lock (&m->imutex);
 }

 if (m->cleanup)
  m->cleanup (m);

 m->status |= MOD_LOCKED;

 pthread_mutex_unlock (&m->mutex);
 pthread_mutex_destroy (&m->mutex);
 pthread_mutex_unlock (&m->imutex);
 pthread_mutex_destroy (&m->imutex);

// if (m->sohandle)
//  dlclose (m->sohandle);

 free (m);
}

int mod_freemodules ( void ) {
 if (mlist != NULL)
  mod_freedesc (mlist);
 mlist = NULL;
 return 1;
}

struct lmodule *mod_update (struct lmodule *module) {
 struct cfgnode *lnode = NULL;
 if (!module->module) return module;

 while (lnode = cfg_findnode ("services-override-module", 0, lnode))
  if (lnode->idattr && module->module->rid && !strcmp(lnode->idattr, module->module->rid)) {
   struct service_information *esi = emalloc (sizeof (struct service_information));
   uint32_t i = 0;

   esi->requires = module->si->requires;
   esi->provides = module->si->provides;
   esi->after = module->si->after;
   esi->before = module->si->before;

   for (; lnode->arbattrs[i]; i+=2) {
    if (!strcmp (lnode->arbattrs[i], "requires")) esi->requires = str2set (':', lnode->arbattrs[i+1]);
    else if (!strcmp (lnode->arbattrs[i], "provides")) esi->provides = str2set (':', lnode->arbattrs[i+1]);
    else if (!strcmp (lnode->arbattrs[i], "after")) esi->after = str2set (':', lnode->arbattrs[i+1]);
    else if (!strcmp (lnode->arbattrs[i], "before")) esi->before = str2set (':', lnode->arbattrs[i+1]);
   }
   module->si = esi;
   break;
  }
 return module;
}

struct lmodule *mod_add (void *sohandle, struct smodule *module) {
 struct lmodule *nmod, *cur;
 int (*scanfunc)(struct lmodule *);
 int (*ftload)  (void *, struct einit_event *);
 int (*configfunc)(struct lmodule *);

 nmod = ecalloc (1, sizeof (struct lmodule));

 pthread_mutex_lock (&mlist_mutex);
 nmod->next = mlist;
 mlist = nmod;
 pthread_mutex_unlock (&mlist_mutex);

 nmod->sohandle = sohandle;
 nmod->module = module;
 pthread_mutex_init (&nmod->mutex, NULL);
 pthread_mutex_init (&nmod->imutex, NULL);

 nmod->si = &module->si;

// this will do additional initialisation functions for certain module-types
 if (module && sohandle) {
// look for and execute any configure() functions in modules
  configfunc = (int (*)(struct lmodule *)) dlsym (sohandle, "configure");
  if (configfunc != NULL) {
   configfunc (nmod);
  }

// look for any cleanup() functions
  if (!nmod->cleanup) {
   configfunc = (int (*)(struct lmodule *)) dlsym (sohandle, "cleanup");
   if (configfunc != NULL) {
    nmod->cleanup = configfunc;
   }
  }
// EINIT_MOD_LOADER modules will usually want to provide a function to scan
//  for modules so they can be included in the dependency chain
  if (module->mode & EINIT_MOD_LOADER) {
   scanfunc = (int (*)(struct lmodule *)) dlsym (sohandle, "scanmodules");
   if (scanfunc != NULL) {
    scanfunc (mlist);
   }
   else bitch(BTCH_ERRNO + BTCH_DL);
  }
// we need to scan for load and unload functions if NULL was supplied for these
  if (!nmod->enable) {
   ftload = (int (*)(void *, struct einit_event *)) dlsym (sohandle, "enable");
   if (ftload != NULL) {
    nmod->enable = ftload;
   }
  }
  if (!nmod->disable) {
   ftload = (int (*)(void *, struct einit_event *)) dlsym (sohandle, "disable");
   if (ftload != NULL) {
    nmod->disable = ftload;
   }
  }
  if (!nmod->reset) {
   ftload = (int (*)(void *, struct einit_event *)) dlsym (sohandle, "reset");
   if (ftload != NULL) {
    nmod->reset = ftload;
   }
  }
  if (!nmod->reload) {
   ftload = (int (*)(void *, struct einit_event *)) dlsym (sohandle, "reload");
   if (ftload != NULL) {
    nmod->reload = ftload;
   }
  }
 }

 nmod = mod_update (nmod);

 return nmod;
}

int mod (unsigned int task, struct lmodule *module) {
 struct einit_event *fb;
 char providefeedback;
 struct smodule *t;
 int ti, errc;
 unsigned int ret;
 struct stree *ha;

 if (!module) return 0;
/* wait if the module is already being processed in a different thread */
 if ((task & MOD_NOMUTEX) || (errc = pthread_mutex_lock (&module->mutex))) {
  if (errno)
   perror ("locking mutex");
 }

 if (task & MOD_IGNORE_DEPENDENCIES) {
  notice (2, "module: skipping dependency-checks");
  task ^= MOD_IGNORE_DEPENDENCIES;
  goto skipdependencies;
 }

 if (module->status & MOD_LOCKED) { // this means the module is locked. maybe we're shutting down just now.
  if ((task & MOD_NOMUTEX) || (errc = pthread_mutex_unlock (&module->mutex))) {
   if (errno)
    perror ("unlocking mutex");
  }
  if (task & MOD_ENABLE)
   return STATUS_FAIL;
  else if (task & MOD_DISABLE)
   return STATUS_OK;
  else
   return STATUS_OK;
 }

 module->status |= STATUS_WORKING;

/* check if the task requested has already been done (or if it can be done at all) */
 if ((task & MOD_ENABLE) && (!module->enable || (module->status & STATUS_ENABLED))) {
  wontload:
#ifdef DEBUG
  {
   char tmp[2048];
   snprintf (tmp, 2048, "refusing to change state of %s\n", module->module->rid);
   notice (10, tmp);
  }
#endif
  module->status ^= STATUS_WORKING;
  if ((task & MOD_NOMUTEX) || (errc = pthread_mutex_unlock (&module->mutex))) {
   if (errno)
    perror ("unlocking mutex");
  }
  return STATUS_IDLE;
 }
 if ((task & MOD_DISABLE) && (!module->disable || (module->status & STATUS_DISABLED)))
  goto wontload;
 if ((task & MOD_RELOAD) && (module->status & STATUS_DISABLED))
  goto wontload;

 if (task & MOD_ENABLE) {
  if (!service_usage_query(SERVICE_REQUIREMENTS_MET, module, NULL))
   goto wontload;
 } else if (task & MOD_DISABLE) {
  if (!service_usage_query(SERVICE_NOT_IN_USE, module, NULL))
   goto wontload;
 }

 skipdependencies:

/* actual loading bit */
 {
  struct einit_event evmstatupdate = evstaticinit(EVE_MODULE_UPDATE);

  evmstatupdate.task = task;
  evmstatupdate.para = (void *)module;
  evmstatupdate.status = STATUS_WORKING;
  event_emit (&evmstatupdate, EINIT_EVENT_FLAG_BROADCAST | EINIT_EVENT_FLAG_SPAWN_THREAD | EINIT_EVENT_FLAG_DUPLICATE);

  fb = evinit (EVE_FEEDBACK_MODULE_STATUS);
  fb->para = (void *)module;
  fb->task = task | MOD_FEEDBACK_SHOW;
  fb->status = STATUS_WORKING;
  fb->flag = 0;
  fb->string = NULL;
  fb->integer = module->fbseq+1;
  status_update (fb);

  if (task & MOD_ENABLE) {
    ret = module->enable (module->param, fb);
    if (ret & STATUS_OK) {
     module->status = STATUS_ENABLED;
     fb->status = STATUS_OK | STATUS_ENABLED;
    } else {
     fb->status = STATUS_FAIL;
    }
  } else if (task & MOD_DISABLE) {
    ret = module->disable (module->param, fb);
    if (ret & STATUS_OK) {
     module->status = STATUS_DISABLED;
     fb->status = STATUS_OK | STATUS_DISABLED;
    } else {
     fb->status = STATUS_FAIL;
    }
  } else if (task & MOD_RESET) {
   if (module->reset) {
    ret = module->reset (module->param, fb);
    if (ret & STATUS_OK) {
     fb->status = STATUS_OK | module->status;
    } else
     fb->status = STATUS_FAIL;
   } else if (module->disable && module->enable) {
     ret = module->disable (module->param, fb);
    if (ret & STATUS_OK) {
     ret = module->enable (module->param, fb);
    } else
     fb->status = STATUS_FAIL;
   }
  } else if (task & MOD_RELOAD) {
   if (module->reload) {
    ret = module->reload (module->param, fb);
    if (ret & STATUS_OK) {
     fb->status = STATUS_OK | module->status;
    } else
     fb->status = STATUS_FAIL;
   }
  }

  module->fbseq = fb->integer + 1;

  evmstatupdate.status = fb->status;
  event_emit (&evmstatupdate, EINIT_EVENT_FLAG_BROADCAST | EINIT_EVENT_FLAG_SPAWN_THREAD | EINIT_EVENT_FLAG_DUPLICATE);

  status_update (fb);
  evdestroy (fb);

  service_usage_query(SERVICE_UPDATE, module, NULL);

  if ((task & MOD_NOMUTEX) || (errc = pthread_mutex_unlock (&module->mutex))) {
//  this is bad...
   if (errno)
    perror ("unlocking mutex");
  }

  evstaticdestroy (evmstatupdate);

 }
 return module->status;
}

uint16_t service_usage_query (uint16_t task, struct lmodule *module, char *service) {
 uint16_t ret = 0;
 struct stree *ha;
 char **t;
 uint32_t i;
 struct service_usage_item *item;

 if ((!module || !module->module) && !service) return 0;

 pthread_mutex_lock (&service_usage_mutex);
 if (task & SERVICE_NOT_IN_USE) {
  ret |= SERVICE_NOT_IN_USE;
/*  if (t = module->provides) {
   for (i = 0; t[i]; i++) {
    if ((ha = streefind (service_usage, t[i])) &&
        ((struct service_usage_item *)(ha->value))->users) {
     ret ^= SERVICE_NOT_IN_USE;
     break;
    }
   }
  }*/
  struct stree *ha = service_usage;

  while (ha) {
   if (((struct service_usage_item *)(ha->value))->users &&
       inset ((void **)(((struct service_usage_item *)(ha->value))->provider), module, -1)) {

#ifdef DEBUG
    char tmp[2048], tmp2[2048];
    snprintf (tmp, 2048, "module %s in use (via %s), by: %s", module->module->rid, ha->key, ((struct service_usage_item *)(ha->value))->users[0]->module->rid);
    for (i = 1; ((struct service_usage_item *)(ha->value))->users[i]; i++) {
     strcpy (tmp2, tmp);
     snprintf (tmp, 2048, "%s, %s", tmp2, ((struct service_usage_item *)(ha->value))->users[i]->module->rid);
    }
    notice(10, tmp);
#endif

    ret ^= SERVICE_NOT_IN_USE;
    break;
   }
   ha = streenext (ha);
  }
 } else if (task & SERVICE_REQUIREMENTS_MET) {
  ret |= SERVICE_REQUIREMENTS_MET;
  if (t = module->si->requires) {
   for (i = 0; t[i]; i++) {
    if (!(ha = streefind (service_usage, t[i], TREE_FIND_FIRST)) ||
        !((struct service_usage_item *)(ha->value))->provider) {
     ret ^= SERVICE_REQUIREMENTS_MET;
     break;
    }
   }
  }
 } else if (task & SERVICE_UPDATE) {
  if (module->status & STATUS_ENABLED) {
   if (t = module->si->requires) {
    for (i = 0; t[i]; i++) {
     if ((ha = streefind (service_usage, t[i], TREE_FIND_FIRST)) && (item = (struct service_usage_item *)ha->value)) {
      item->users = (struct lmodule **)setadd ((void **)item->users, (void *)module, SET_NOALLOC);
     }
    }
   }
   if (t = module->si->provides) {
    for (i = 0; t[i]; i++) {
     if ((ha = streefind (service_usage, t[i], TREE_FIND_FIRST)) && (item = (struct service_usage_item *)ha->value)) {
      item->provider = (struct lmodule **)setadd ((void **)item->provider, (void *)module, SET_NOALLOC);
     } else {
      struct service_usage_item nitem;
      memset (&nitem, 0, sizeof (struct service_usage_item));
      nitem.provider = (struct lmodule **)setadd ((void **)nitem.provider, (void *)module, SET_NOALLOC);
      service_usage = streeadd (service_usage, t[i], &nitem, sizeof (struct service_usage_item), NULL);
     }
    }
   }
  }

/* more cleanup code */
  ha = service_usage;
  while (ha) {
   item = (struct service_usage_item *)ha->value;

   if (!(module->status & STATUS_ENABLED)) {
     item->provider = (struct lmodule **)setdel ((void **)item->provider, (void *)module);
     item->users = (struct lmodule **)setdel ((void **)item->users, (void *)module);
   }

   if (!item->provider && !item->users) {
//    service_usage = streedel (service_usage, ha);
    service_usage = streedel (ha);
    ha = service_usage;
   } else
    ha = streenext (ha);
  }
 } else if (task & SERVICE_IS_REQUIRED) {
  if ((ha = streefind (service_usage, service, TREE_FIND_FIRST)) && (item = (struct service_usage_item *)ha->value) && (item->users))
   ret |= SERVICE_IS_REQUIRED;
 } else if (task & SERVICE_IS_PROVIDED) {
  if ((ha = streefind (service_usage, service, TREE_FIND_FIRST)) && (item = (struct service_usage_item *)ha->value) && (item->provider))
   ret |= SERVICE_IS_PROVIDED;
 }

 pthread_mutex_unlock (&service_usage_mutex);
 return ret;
}

uint16_t service_usage_query_group (uint16_t task, struct lmodule *module, char *service) {
 uint16_t ret = 0;
 struct stree *ha;
 char **t;
 uint32_t i;
 struct service_usage_item *item;

 if ((!module || !module->module) && !service) return 0;

 pthread_mutex_lock (&service_usage_mutex);
 if (task & SERVICE_ADD_GROUP_PROVIDER) {
  if (!(ha = streefind (service_usage, service, TREE_FIND_FIRST))) {
   struct service_usage_item nitem;
   memset (&nitem, 0, sizeof (struct service_usage_item));
   nitem.provider = (struct lmodule **)setadd ((void **)nitem.provider, (void *)module, SET_NOALLOC);
   service_usage = streeadd (service_usage, service, &nitem, sizeof (struct service_usage_item), NULL);
  }
 }

 pthread_mutex_unlock (&service_usage_mutex);
 return ret;
}

char **service_usage_query_cr (uint16_t task, struct lmodule *module, char *service) {
 pthread_mutex_lock (&service_usage_mutex);

 struct stree *ha = service_usage;
 char **ret = NULL;
 uint32_t i;

 if (task & SERVICE_GET_ALL_PROVIDED) {
  while (ha) {
//   puts (ha->key);
   ret = (char **)setadd ((void **)ret, (void *)ha->key, SET_TYPE_STRING);
   ha = streenext (ha);
  }
 } else if (task & SERVICE_GET_SERVICES_THAT_USE) {
  if (module) {
   while (ha) {
    if (((struct service_usage_item *)(ha->value))->users &&
        inset ((void **)(((struct service_usage_item*)ha->value)->provider), module, -1)) {
     for (i = 0; ((struct service_usage_item *)(ha->value))->users[i]; i++) {
      if (((struct service_usage_item *)(ha->value))->users[i]->module)
       ret = (char **)setcombine ((void **)ret, (void **)((struct service_usage_item *)(ha->value))->users[i]->si->provides, SET_TYPE_STRING);
     }
//     ret = (char **)setadd ((void **)ret, (void *)ha->key, SET_TYPE_STRING);
    }
    ha = streenext (ha);
   }
  }
 } else if (task & SERVICE_GET_SERVICES_USED_BY) {
  if (module) {
   while (ha) {
    if (inset ((void **)(((struct service_usage_item*)ha->value)->users), module, -1)) {
     ret = (char **)setadd ((void **)ret, (void *)ha->key, SET_TYPE_STRING);
    }
    ha = streenext (ha);
   }
  }
 }

 pthread_mutex_unlock (&service_usage_mutex);
 return ret;
}


/* ---------------------------------------------------- event handlers ----- */
#define STATUS2STRING(status)\
 (status == STATUS_IDLE ? "idle" : \
 (status & STATUS_WORKING ? "working" : \
 (status & STATUS_ENABLED ? "enabled" : "disabled")))
#define STATUS2STRING_SHORT(status)\
 (status == STATUS_IDLE ? "I" : \
 (status & STATUS_WORKING ? "W" : \
 (status & STATUS_ENABLED ? "E" : "D")))

void mod_event_handler(struct einit_event *event) {
 if (!event || !event->set) return;
 char **argv = (char **) event->set;
 int argc = setcount (event->set);
 uint32_t options = event->status;

 if (argc >= 2) {
  if (!strcmp (argv[0], "list")) {
   if (!strcmp (argv[1], "modules")) {
    char buffer[1024];
    struct lmodule *cur = mlist;

    if (!event->flag) event->flag = 1;

    while (cur) {
     if (cur->module && !(options & EIPC_ONLY_RELEVANT) || (cur->status != STATUS_IDLE)) {
      if (options & EIPC_OUTPUT_XML)
       snprintf (buffer, 2048, " <module id=\"%s\" name=\"%s\" status=\"%s\" />\n",
         (cur->module->rid ? cur->module->rid : "unknown"), (cur->module->name ? cur->module->name : "unknown"), STATUS2STRING(cur->status));
      else
       snprintf (buffer, 1024, "[%s] %s (%s)\n",
        STATUS2STRING_SHORT(cur->status), (cur->module->rid ? cur->module->rid : "unknown"), (cur->module->name ? cur->module->name : "unknown"));

      write (event->integer, buffer, strlen (buffer));
     }
     cur = cur->next;
    }
   } else if (!strcmp (argv[1], "services")) {
    char buffer[1024];
    struct stree *cur = service_usage;

    if (!event->flag) event->flag = 1;

    while (cur) {
	 struct service_usage_item *su = (struct service_usage_item *)cur->value;
     snprintf (buffer, 1024, "%s\n", cur->key);
     fdputs (buffer, event->integer);
     if (su->provider) {
      uint32_t i = 0;
      for (; su->provider[i]; i++) {
       if (su->provider[i]->module) {
        snprintf (buffer, 1024, " >> %s (%s)\n", su->provider[i]->module->rid ? su->provider[i]->module->rid : "unknown", su->provider[i]->module->name ? su->provider[i]->module->name : "unknown");
       } else
        snprintf (buffer, 1024, " >> unknown module\n");
	  }
      fdputs (buffer, event->integer);
     }
     cur = streenext (cur);
	}
   }
  }
 }
}

void module_loader_einit_event_handler (struct einit_event *ev) {
 if (ev->type == EVE_CONFIGURATION_UPDATE) {
//  notice (2, "should update modules now");
  mod_scanmodules();
 }
}
