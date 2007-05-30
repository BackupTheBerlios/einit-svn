/*
 *  module-logic-v4.c
 *  einit
 *
 *  Created by Magnus Deininger on 09/04/2007.
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
#include <unistd.h>
#include <stdio.h>
#include <einit/config.h>
#include <einit/module-logic.h>
#include <einit/module.h>
#include <einit/tree.h>
#include <pthread.h>
#include <string.h>
#include <einit/bitch.h>
#include <einit-modules/ipc.h>
#include <einit-modules/configuration.h>

#ifdef _POSIX_PRIORITY_SCHEDULING
#include <sched.h>
#endif

#define MAX_ITERATIONS 1000

int einit_module_logic_v3_configure (struct lmodule *);

#if defined(EINIT_MODULE) || defined(EINIT_MODULE_HEADER)
const struct smodule einit_module_logic_v3_self = {
 .eiversion = EINIT_VERSION,
 .eibuild   = BUILDNUMBER,
 .version   = 1,
 .mode      = 0,
 .name      = "Module Logic Core (V3)",
 .rid       = "module-logic-v3",
 .si        = {
  .provides = NULL,
  .requires = NULL,
  .after    = NULL,
  .before   = NULL
 },
 .configure = einit_module_logic_v3_configure
};

module_register(einit_module_logic_v3_self);

#endif

char shutting_down = 0;

void module_logic_ipc_event_handler (struct einit_event *);
void module_logic_einit_event_handler (struct einit_event *);
double mod_get_plan_progress_f (struct mloadplan *);
char initdone = 0;
char mod_isbroken (char *service);
char mod_mark (char *service, char task);
struct group_data *mod_group_get_data (char *group);
char mod_isprovided(char *service);
void module_logic_update_init_d ();

/* new functions: */
char mod_examine_group (char *);
void mod_examine_module (struct lmodule *);
void mod_examine (char *);

void mod_commit_and_wait (char **, char **);

void mod_defer_notice (struct lmodule *, char **);

struct module_taskblock
  current = { NULL, NULL, NULL },
  target_state = { NULL, NULL, NULL };

struct stree *module_logics_service_list = NULL; // value is a (struct lmodule **)
struct stree *module_logics_group_data = NULL;

pthread_mutex_t
  ml_tb_current_mutex = PTHREAD_MUTEX_INITIALIZER,
  ml_tb_target_state_mutex = PTHREAD_MUTEX_INITIALIZER,
  ml_service_list_mutex = PTHREAD_MUTEX_INITIALIZER,
  ml_group_data_mutex = PTHREAD_MUTEX_INITIALIZER,
  ml_unresolved_mutex = PTHREAD_MUTEX_INITIALIZER,
  ml_currently_provided_mutex = PTHREAD_MUTEX_INITIALIZER,
  ml_service_update_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t
  ml_cond_service_update = PTHREAD_COND_INITIALIZER;

char **unresolved_services = NULL;
char **broken_services = NULL;

struct group_data {
 char **members;
 uint32_t options;
};

#define MOD_PLAN_GROUP_SEQ_ANY     0x00000001
#define MOD_PLAN_GROUP_SEQ_ALL     0x00000002
#define MOD_PLAN_GROUP_SEQ_ANY_IOP 0x00000004
#define MOD_PLAN_GROUP_SEQ_MOST    0x00000008

#define MARK_BROKEN                0x01
#define MARK_UNRESOLVED            0x02

/* module header functions */

int einit_module_logic_v3_cleanup (struct lmodule *this) {
 function_unregister ("module-logic-get-plan-progress", 1, mod_get_plan_progress_f);

 event_ignore (einit_event_subsystem_core, module_logic_einit_event_handler);
 event_ignore (einit_event_subsystem_ipc, module_logic_ipc_event_handler);

 return 0;
}

int einit_module_logic_v3_configure (struct lmodule *this) {
 module_init(this);

 thismodule->cleanup = einit_module_logic_v3_cleanup;

 event_listen (einit_event_subsystem_ipc, module_logic_ipc_event_handler);
 event_listen (einit_event_subsystem_core, module_logic_einit_event_handler);

 function_register ("module-logic-get-plan-progress", 1, mod_get_plan_progress_f);

 return 0;
}

/* end module header */
/* start common functions with v3 */

char mod_isbroken (char *service) {
 int retval = 0;

 emutex_lock (&ml_unresolved_mutex);

 retval = inset ((const void **)broken_services, (void *)service, SET_TYPE_STRING) ||
   inset ((const void **)unresolved_services, (void *)service, SET_TYPE_STRING);

 emutex_unlock (&ml_unresolved_mutex);

 return retval;
}

struct group_data *mod_group_get_data (char *group) {
 struct group_data *ret = NULL;

/* eputs ("mod_group_get_data", stderr);
 fflush (stderr);*/

 emutex_lock (&ml_group_data_mutex);

/* eputs ("got mutex", stderr);
 fflush (stderr);*/

 struct stree *cur = module_logics_group_data ? streefind (module_logics_group_data, group, tree_find_first) : NULL;
 if (cur) { ret = (struct group_data *)cur->value; }
 else {
  char *tnodeid = emalloc (strlen (group)+17);
  struct cfgnode *gnode = NULL;

  memcpy (tnodeid, "services-alias-", 16);
  strcat (tnodeid, group);

  ret = ecalloc (1, sizeof (struct group_data));

  if ((gnode = cfg_getnode (tnodeid, NULL)) && gnode->arbattrs) {
   ssize_t r = 0;

   for (r = 0; gnode->arbattrs[r]; r+=2) {
    if (strmatch (gnode->arbattrs[r], "group")) {
     ret->members = str2set (':', gnode->arbattrs[r+1]);
    } else if (strmatch (gnode->arbattrs[r], "seq")) {
     if (strmatch (gnode->arbattrs[r+1], "any"))
      ret->options |=  MOD_PLAN_GROUP_SEQ_ANY;
     else if (strmatch (gnode->arbattrs[r+1], "all"))
      ret->options |=  MOD_PLAN_GROUP_SEQ_ALL;
     else if (strmatch (gnode->arbattrs[r+1], "any-iop"))
      ret->options |=  MOD_PLAN_GROUP_SEQ_ANY_IOP;
     else if (strmatch (gnode->arbattrs[r+1], "most"))
      ret->options |=  MOD_PLAN_GROUP_SEQ_MOST;
    }
   }
  }
  free (tnodeid);

  if (!ret->members || !ret->options) {
   free (ret);
   ret = NULL;
  } else {
   module_logics_group_data = streeadd (module_logics_group_data, group, (void *)ret, SET_NOALLOC, (void *)ret);
  }
 }

 emutex_unlock (&ml_group_data_mutex);

 return ret;
}

void cross_taskblock (struct module_taskblock *source, struct module_taskblock *target) {
 if (source->enable) {
  char **tmp = (char **)setcombine ((const void **)target->enable, (const void **)source->enable, SET_TYPE_STRING);
  if (target->enable) free (target->enable);
  target->enable = tmp;

  tmp = (char **)setslice ((const void **)target->disable, (const void **)source->enable, SET_TYPE_STRING);
  if (target->disable) free (target->disable);
  target->disable = tmp;
 }
 if (source->critical) {
  char **tmp = (char **)setcombine ((const void **)target->critical, (const void **)source->critical, SET_TYPE_STRING);
  if (target->critical) free (target->critical);
  target->critical = tmp;
 }

 if (source->disable) {
  char **tmp = (char **)setcombine ((const void **)target->disable, (const void **)source->disable, SET_TYPE_STRING);
  if (target->disable) free (target->disable);
  target->disable = tmp;

  tmp = (char **)setslice ((const void **)target->enable, (const void **)source->disable, SET_TYPE_STRING);
  if (target->enable) free (target->enable);
  target->enable = tmp;

  tmp = (char **)setslice ((const void **)target->critical, (const void **)source->disable, SET_TYPE_STRING);
  if (target->critical) free (target->critical);
  target->critical = tmp;
 }
}

struct mloadplan *mod_plan (struct mloadplan *plan, char **atoms, unsigned int task, struct cfgnode *mode) {
 char disable_all_but_feedback = 0, disable_all = 0, auto_add_feedback = 0;

 if (!plan) {
  plan = emalloc (sizeof (struct mloadplan));
  memset (plan, 0, sizeof (struct mloadplan));
 }

 if (mode) {
  char **base = NULL;
  uint32_t xi = 0;
  char **enable   = str2set (':', cfg_getstring ("enable/services", mode));
  char **disable  = str2set (':', cfg_getstring ("disable/services", mode));
  char **critical = str2set (':', cfg_getstring ("enable/critical", mode));
  char *strng = cfg_getstring ("feedback/auto-add", mode);

  if ((strng = cfg_getstring ("feedback/auto-add", mode)))
   auto_add_feedback = parse_boolean(strng);
  if ((strng = cfg_getstring ("options/shutdown", mode)) && parse_boolean(strng)) {
   plan->options |= plan_option_shutdown;
  }

  if (!enable)
   enable  = str2set (':', cfg_getstring ("enable/mod", mode));
  if (!disable)
   disable = str2set (':', cfg_getstring ("disable/mod", mode));

  if (mode->arbattrs) for (; mode->arbattrs[xi]; xi+=2) {
   if (strmatch(mode->arbattrs[xi], "base")) {
    base = str2set (':', mode->arbattrs[xi+1]);
   }
  }

  if (enable) {
   char **tmp = (char **)setcombine ((const void **)plan->changes.enable, (const void **)enable, SET_TYPE_STRING);
   if (plan->changes.enable) free (plan->changes.enable);
   plan->changes.enable = tmp;
  }
  if (disable) {
   char **tmp = (char **)setcombine ((const void **)plan->changes.disable, (const void **)disable, SET_TYPE_STRING);
   if (plan->changes.disable) free (plan->changes.disable);
   plan->changes.disable = tmp;
  }
  if (critical) {
   char **tmp = (char **)setcombine ((const void **)plan->changes.critical, (const void **)critical, SET_TYPE_STRING);
   if (plan->changes.critical) free (plan->changes.critical);
   plan->changes.critical = tmp;
  }

  if (auto_add_feedback && (strng = cfg_getstring("feedback/default", mode))) {
   if (plan->changes.enable) {
    uint32_t y = 0;

    for (; plan->changes.enable[y]; y++) {
     struct stree *st = NULL;

     emutex_lock (&ml_service_list_mutex);

     if ((st = streefind (module_logics_service_list, plan->changes.enable[y], tree_find_first))) {
      struct lmodule **lmod = st->value;

      if (lmod[0] && lmod[0]->module && lmod[0]->module->mode & einit_module_feedback) {
       emutex_unlock (&ml_service_list_mutex);
       goto have_feedback;
      }
     }

     emutex_unlock (&ml_service_list_mutex);
    }
   }

   plan->changes.enable = (char **)setadd ((void **)plan->changes.enable, (void *)strng, SET_TYPE_STRING);
  }
  have_feedback:

  if (base) {
   int y = 0;
   struct cfgnode *cno;
   while (base[y]) {
    if (!inset ((const void **)plan->used_modes, (void *)base[y], SET_TYPE_STRING)) {
     cno = cfg_findnode (base[y], einit_node_mode, NULL);
     if (cno) {
      plan = mod_plan (plan, NULL, 0, cno);
     }
    }

    y++;
   }

   free (base);
  }

  if (mode->id) {
   plan->used_modes = (char **)setadd ((void **)plan->used_modes, mode->id, SET_TYPE_STRING);
  }

  plan->mode = mode;
 } else {
  if (task & einit_module_enable) {
   char **tmp = (char **)setcombine ((const void **)plan->changes.enable, (const void **)atoms, SET_TYPE_STRING);
   if (plan->changes.enable) free (plan->changes.enable);
   plan->changes.enable = tmp;
  }
  if (task & einit_module_disable) {
   char **tmp = (char **)setcombine ((const void **)plan->changes.disable, (const void **)atoms, SET_TYPE_STRING);
   if (plan->changes.disable) free (plan->changes.disable);
   plan->changes.disable = tmp;
  }
 }

 disable_all = inset ((const void **)plan->changes.disable, (void *)"all", SET_TYPE_STRING);
 disable_all_but_feedback = inset ((const void **)plan->changes.disable, (void *)"all-but-feedback", SET_TYPE_STRING);

 if (disable_all || disable_all_but_feedback) {
  struct stree *cur;
  ssize_t i = 0;
  char **tmpy = service_usage_query_cr (service_is_provided, NULL, NULL);

  emutex_lock (&ml_service_list_mutex);
  emutex_lock (&ml_tb_target_state_mutex);
//  char **tmpx = (char **)setcombine ((const void **)plan->changes.enable, (const void **)target_state.enable, SET_TYPE_STRING);
  char **tmpx = (char **)setcombine ((const void **)tmpy, (const void **)target_state.enable, SET_TYPE_STRING);

  emutex_unlock (&ml_tb_target_state_mutex);

  char **tmp = (char **)setcombine ((const void **)tmpx, (const void **)plan->changes.disable, SET_TYPE_STRING);

  free (tmpx);
  free (tmpy);

  if (plan->changes.disable) {
   free (plan->changes.disable);
   plan->changes.disable = NULL;
  }

  if (tmp) {
   for (; tmp[i]; i++) {
    char add = 1;

    if (inset ((const void **)plan->changes.disable, (void *)tmp[i], SET_TYPE_STRING)) {
     add = 0;
    } else if ((disable_all && strmatch(tmp[i], "all")) ||
               (disable_all_but_feedback && strmatch(tmp[i], "all-but-feedback"))) {
     add = 0;
    } else if (module_logics_service_list && (cur = streefind (module_logics_service_list, tmp[i], tree_find_first))) {
     struct lmodule **lm = (struct lmodule **)cur->value;
     if (lm) {
      ssize_t y = 0;
      for (; lm[y]; y++) {
       if (disable_all_but_feedback && (lm[y]->module->mode & einit_module_feedback)) {
        add = 0;

        break;
       }
      }
     }
    } else if (!service_usage_query (service_is_provided, NULL, tmp[i])) {
     add = 0;
    }

    if (add) {
     plan->changes.disable = (char **)setadd((void **)plan->changes.disable, (void *)tmp[i], SET_TYPE_STRING);
    }
   }

   free (tmp);
  }

  emutex_unlock (&ml_service_list_mutex);
 }

 return plan;
}

unsigned int mod_plan_commit (struct mloadplan *plan) {
 struct einit_event *fb = evinit (einit_feedback_plan_status);

 if (!plan) return 0;

 if (plan->options & plan_option_shutdown)
  shutting_down = 1;

// do some extra work if the plan was derived from a mode
 if (plan->mode) {
  char *cmdt;
  cmode = plan->mode;

  struct einit_event eex = evstaticinit (einit_core_mode_switching);
  eex.para = (void *)plan->mode;
  event_emit (&eex, einit_event_flag_broadcast);
  evstaticdestroy (eex);

  if ((cmdt = cfg_getstring ("before-switch/emit-event", cmode))) {
   struct einit_event ee = evstaticinit (event_string_to_code(cmdt));
   event_emit (&ee, einit_event_flag_broadcast);
   evstaticdestroy (ee);
  }

  if ((cmdt = cfg_getstring ("before-switch/ipc", cmode))) {
   char **cmdts = str2set (':', cmdt);
   uint32_t in = 0;

   if (cmdts) {
    for (; cmdts[in]; in++)
     ipc_process(cmdts[in], stderr);

    free (cmdts);
   }
  }
 }

 fb->task = MOD_SCHEDULER_PLAN_COMMIT_START;
 fb->para = (void *)plan;
 status_update (fb);

 emutex_lock (&ml_tb_target_state_mutex);

 cross_taskblock (&(plan->changes), &target_state);

 emutex_lock (&ml_tb_current_mutex);

 cross_taskblock (&target_state, &current);

 uint32_t i = 0;

 if (current.enable) {
  char **tmp = NULL;
  for (i = 0; current.enable[i]; i++) {
   if (!service_usage_query (service_is_provided, NULL, current.enable[i])) {
    tmp = (char **)setadd ((void **)tmp, (void *)current.enable[i], SET_TYPE_STRING);
   }
  }
  free (current.enable);
  current.enable = tmp;
 }
 if (current.disable) {
  char **tmp = NULL;
  for (i = 0; current.disable[i]; i++) {
   if (service_usage_query (service_is_provided, NULL, current.disable[i])) {
    tmp = (char **)setadd ((void **)tmp, (void *)current.disable[i], SET_TYPE_STRING);
   }
  }
  free (current.disable);
  current.disable = tmp;
 }

 emutex_unlock (&ml_tb_current_mutex);
 emutex_unlock (&ml_tb_target_state_mutex);

 mod_commit_and_wait (plan->changes.enable, plan->changes.disable);

 fb->task = MOD_SCHEDULER_PLAN_COMMIT_FINISH;
 status_update (fb);

// do some more extra work if the plan was derived from a mode
 if (plan->mode) {
  char *cmdt;
  cmode = plan->mode;
  amode = plan->mode;

  struct einit_event eex = evstaticinit (einit_core_mode_switch_done);
  eex.para = (void *)plan->mode;
  event_emit (&eex, einit_event_flag_broadcast);
  evstaticdestroy (eex);

  if (amode->id) {
   struct einit_event eema = evstaticinit (einit_core_plan_update);
   eema.string = estrdup(amode->id);
   eema.para   = (void *)amode;
   event_emit (&eema, einit_event_flag_broadcast);
   free (eema.string);
   evstaticdestroy (eema);
  }

  if ((cmdt = cfg_getstring ("after-switch/emit-event", amode))) {
   struct einit_event ee = evstaticinit (event_string_to_code(cmdt));
   event_emit (&ee, einit_event_flag_broadcast);
   evstaticdestroy (ee);
  }

  if ((cmdt = cfg_getstring ("after-switch/ipc", amode))) {
   char **cmdts = str2set (':', cmdt);
   uint32_t in = 0;

   if (cmdts) {
    for (; cmdts[in]; in++) {
     ipc_process(cmdts[in], stderr);
    }
    free (cmdts);
   }
  }
 }

 evdestroy (fb);

 module_logic_update_init_d();

 if (plan->mode) return 0; // always return "OK" if it's based on a mode

 return 0;
}

int mod_plan_free (struct mloadplan *plan) {
 if (plan) {
  if (plan->changes.enable) free (plan->changes.enable);
  if (plan->changes.disable) free (plan->changes.disable);
  if (plan->changes.critical) free (plan->changes.critical);

  if (plan->used_modes) free (plan->used_modes);

  free (plan);
 }
 return 0;
}

double mod_get_plan_progress_f (struct mloadplan *plan) {
 if (plan) {
  return 0.0;
 } else {
  double all = 0, left = 0;
  emutex_lock (&ml_tb_target_state_mutex);
  if (target_state.enable) all += setcount ((const void **)target_state.enable);
  if (target_state.disable) all += setcount ((const void **)target_state.disable);
  emutex_unlock (&ml_tb_target_state_mutex);

  emutex_lock (&ml_tb_current_mutex);
  if (current.enable) left += setcount ((const void **)current.enable);
  if (current.disable) left += setcount ((const void **)current.disable);
  emutex_unlock (&ml_tb_current_mutex);

  return 1.0 - (double)(left / all);
 }
}

void mod_sort_service_list_items_by_preference() {
 struct stree *cur;

 emutex_lock (&ml_service_list_mutex);

 cur = module_logics_service_list;

 while (cur) {
  struct lmodule **lm = (struct lmodule **)cur->value;

  if (lm) {
   /* order modules that should be enabled according to the user's preference */
   uint32_t mpx, mpy, mpz = 0;
   char *pnode = NULL, **preference = NULL;

   /* first make sure all modules marked as "deprecated" are last */
   for (mpx = 0; lm[mpx]; mpx++); mpx--;
   for (mpy = 0; mpy < mpx; mpy++) {
    if (lm[mpy]->module && (lm[mpy]->module->mode & einit_module_deprecated)) {
     struct lmodule *t = lm[mpx];
     lm[mpx] = lm[mpy];
     lm[mpy] = t;
     mpx--;
    }
   }

   /* now to the sorting bit... */
   pnode = emalloc (strlen (cur->key)+18);
   pnode[0] = 0;
   strcat (pnode, "services-prefer-");
   strcat (pnode, cur->key);

   if ((preference = str2set (':', cfg_getstring (pnode, NULL)))) {
    debugx ("applying module preferences for service %s", cur->key);

    for (mpx = 0; preference[mpx]; mpx++) {
     for (mpy = 0; lm[mpy]; mpy++) {
      if (lm[mpy]->module && lm[mpy]->module->rid && strmatch(lm[mpy]->module->rid, preference[mpx])) {
       struct lmodule *tm = lm[mpy];

       lm[mpy] = lm[mpz];
       lm[mpz] = tm;

       mpz++;
      }
     }
    }
    free (preference);
   }

   free (pnode);
  }

  cur = streenext(cur);
 }

 emutex_unlock (&ml_service_list_mutex);
}

int mod_switchmode (char *mode) {
 if (!mode) return -1;
 struct cfgnode *cur = cfg_findnode (mode, einit_node_mode, NULL);
 struct mloadplan *plan = NULL;

 if (!cur) {
  notice (1, "module-logic-v3: scheduled mode \"%s\" not defined, aborting", mode);
  return -1;
 }

 plan = mod_plan (NULL, NULL, 0, cur);
 if (!plan) {
  notice (1, "module-logic-v3: scheduled mode \"%s\" defined but nothing to be done", mode);
 } else {
  pthread_t th;
  mod_plan_commit (plan);
  /* make it so that the erase operation will not disturb the flow of the program */
  ethread_create (&th, &thread_attribute_detached, (void *(*)(void *))mod_plan_free, (void *)plan);
 }

 return 0;
}

int mod_modaction (char **argv) {
 int argc = setcount ((const void **)argv), ret = 0;
 int32_t task = 0;
 struct mloadplan *plan;
 if (!argv || (argc != 2)) return -1;

 if (strmatch (argv[1], "enable") || strmatch (argv[1], "start")) task = einit_module_enable;
 else if (strmatch (argv[1], "disable") || strmatch (argv[1], "stop")) task = einit_module_disable;
 else {
  struct lmodule **tm = NULL;
  uint32_t r = 0;

  emutex_lock (&ml_service_list_mutex);
  if (module_logics_service_list) {
   struct stree *cur = streefind (module_logics_service_list, argv[0], tree_find_first);
   if (cur) {
    tm = cur->value;
   }
  }

  emutex_unlock (&ml_service_list_mutex);

  ret = 1;
  if (tm) {
   if (strmatch (argv[1], "status")) {
    for (; tm[r]; r++) {
     if (tm[r]->status & status_working) {
      ret = 2;
      break;
     }
     if (tm[r]->status & status_enabled) {
      ret = 0;
      break;
     }
    }
   } else {
    for (; tm[r]; r++) {
     int retx = mod (einit_module_custom, tm[r], argv[1]);

     if (retx == status_ok)
      ret = 0;
    }
   }
  }

  return ret;
 }

 argv[1] = NULL;

 if ((plan = mod_plan (NULL, argv, task, NULL))) {
  pthread_t th;

  ret = mod_plan_commit (plan);

  if (task & einit_module_enable) {
   ret = !mod_isprovided (argv[0]);
  } else if (task & einit_module_disable) {
   ret = mod_isprovided (argv[0]);
  }

  ethread_create (&th, &thread_attribute_detached, (void *(*)(void *))mod_plan_free, (void *)plan);
 }

// free (argv[0]);
// free (argv);

 return ret;
}

void module_logic_einit_event_handler(struct einit_event *ev) {
 if ((ev->type == einit_core_update_configuration) && !initdone) {
  initdone = 1;

  function_register("module-logic-get-plan-progress", 1, (void (*)(void *))mod_get_plan_progress_f);
 } else if (ev->type == einit_core_module_list_update) {
  /* update list with services */
  struct stree *new_service_list = NULL;
  struct lmodule *cur = ev->para;

  emutex_lock (&ml_service_list_mutex);

  while (cur) {
   if (cur->si && cur->si->provides) {
    ssize_t i = 0;

    for (; cur->si->provides[i]; i++) {
     struct stree *slnode = new_service_list ?
       streefind (new_service_list, cur->si->provides[i], tree_find_first) :
       NULL;
     struct lnode **curval = (struct lnode **) (slnode ? slnode->value : NULL);

     curval = (struct lnode **)setadd ((void **)curval, cur, SET_NOALLOC);

     if (slnode) {
      slnode->value = curval;
      slnode->luggage = curval;
     } else {
      new_service_list = streeadd (new_service_list, cur->si->provides[i], (void *)curval, SET_NOALLOC, (void *)curval);
     }
    }
   }
   cur = cur->next;
  }

  if (module_logics_service_list) streefree (module_logics_service_list);
  module_logics_service_list = new_service_list;

  emutex_unlock (&ml_service_list_mutex);

  mod_sort_service_list_items_by_preference();

  emutex_lock (&ml_unresolved_mutex);

  if (unresolved_services) {
   free (unresolved_services);
   unresolved_services = NULL;
  }
  if (broken_services) {
   free (broken_services);
   broken_services = NULL;
  }

  emutex_unlock (&ml_unresolved_mutex);
  emutex_lock (&ml_group_data_mutex);

  if (module_logics_group_data) {
   streefree (module_logics_group_data);
   module_logics_group_data = NULL;
  }

  emutex_unlock (&ml_group_data_mutex);

  ev->chain_type = einit_core_module_list_update_complete;
 } else if ((ev->type == einit_core_service_update) && (!(ev->status & status_working))) {
/* something's done now, update our lists */
  mod_examine_module ((struct lmodule *)ev->para);
 } else switch (ev->type) {
  case einit_core_switch_mode:
   if (!ev->string) return;
   else {
    if (ev->output) {
     struct einit_event ee = evstaticinit(einit_feedback_register_fd);
     ee.output = ev->output;
     ee.ipc_options = ev->ipc_options;
     event_emit (&ee, einit_event_flag_broadcast);
     evstaticdestroy(ee);
    }

    mod_switchmode (ev->string);

    if (ev->output) {
     struct einit_event ee = evstaticinit(einit_feedback_unregister_fd);
     ee.output = ev->output;
     ee.ipc_options = ev->ipc_options;
     event_emit (&ee, einit_event_flag_broadcast);
     evstaticdestroy(ee);
    }
   }
   return;
  case einit_core_change_service_status:
   if (!ev->set) return;
   else {
    if (ev->output) {
     struct einit_event ee = evstaticinit(einit_feedback_register_fd);

     if (ev->set && ev->set[0] && ev->set[1] &&
         (strmatch (ev->set[1], "enable") || strmatch (ev->set[1], "disable") ||
          strmatch (ev->set[1], "start") || strmatch (ev->set[1], "stop"))) {
      uint32_t r = 0;
      char **senable = NULL;
      char **sdisable = NULL;

      eputs ("checking for previously requested but dropped or broken services:", ev->output);

      emutex_lock (&ml_tb_target_state_mutex);
      if (target_state.enable) {
       for (r = 0; target_state.enable[r]; r++) {
        if (!mod_isprovided (target_state.enable[r]))
         senable = (char **)setadd ((void **)senable, (void *)target_state.enable[r], SET_TYPE_STRING);
       }
      }
      if (target_state.disable) {
       for (r = 0; target_state.disable[r]; r++) {
        if (mod_isprovided (target_state.disable[r]))
         sdisable = (char **)setadd ((void **)sdisable, (void *)target_state.disable[r], SET_TYPE_STRING);
       }
      }
      emutex_unlock (&ml_tb_target_state_mutex);

      if (senable) {
       char *x = set2str (' ', (const char **)senable);

       eprintf (ev->output, "\n \e[33m** will also enable: %s\e[0m", x);
       free (senable);
       free (x);
      }
      if (sdisable) {
       char *x = set2str (' ', (const char **)sdisable);

       eprintf (ev->output, "\n \e[33m** will also disable: %s\e[0m", x);
       free (sdisable);
       free (x);
      }
      eputs ("\n \e[32m>> check complete.\e[0m\n", ev->output);
     }

     ee.output = ev->output;
     ee.ipc_options = ev->ipc_options;
     event_emit (&ee, einit_event_flag_broadcast);
     evstaticdestroy(ee);
    }

    ev->integer = mod_modaction ((char **)ev->set);
/*    if (mod_modaction ((char **)ev->set)) {
     ev->integer = 1;
    }*/

    if (ev->output) {
     struct einit_event ee = evstaticinit(einit_feedback_unregister_fd);

     ee.output = ev->output;
     ee.ipc_options = ev->ipc_options;
     event_emit (&ee, einit_event_flag_broadcast);
     evstaticdestroy(ee);

     fflush (ev->output);

     if (ev->integer) {
      eputs (" \e[31m!! request failed.\e[0m\n", ev->output);
     } else {
      eputs (" \e[32m>> changes applied.\e[0m\n", ev->output);
     }
    }
   }
   return;
  default:
   return;
 }
}

void module_logic_update_init_d () {
 struct cfgnode *einit_d = cfg_getnode ("core-module-logic-maintain-init.d", NULL);

 notice (2, "module_logic_update_init_d(): regenerating list of services in init.d.");

 if (einit_d && einit_d->flag && einit_d->svalue) {
  char *init_d_path = cfg_getstring ("core-module-logic-init.d-path", NULL);

  if (init_d_path) {
   struct stree *cur;
   emutex_lock (&ml_service_list_mutex);
//  struct stree *module_logics_service_list;
   cur = module_logics_service_list;

   while (cur) {
    char tmp[BUFFERSIZE];
    esprintf (tmp, BUFFERSIZE, "%s/%s", init_d_path, cur->key);

    symlink (einit_d->svalue, tmp);

    cur = cur->next;
   }

   emutex_unlock (&ml_service_list_mutex);
  }
 }
}

void module_logic_ipc_event_handler (struct einit_event *ev) {
 if (ev->argv && ev->argv[0] && ev->argv[1] && ev->output) {
  if (strmatch (ev->argv[0], "update") && strmatch (ev->argv[1], "init.d")) {
   module_logic_update_init_d();
  } else if (strmatch (ev->argv[0], "examine") && strmatch (ev->argv[1], "configuration")) {
   struct cfgnode *cfgn = cfg_findnode ("mode-enable", 0, NULL);
   char **modes = NULL;

   while (cfgn) {
    if (cfgn->arbattrs && cfgn->mode && cfgn->mode->id && (!modes || !inset ((const void **)modes, (const void *)cfgn->mode->id, SET_TYPE_STRING))) {
     uint32_t i = 0;
     modes = (char **)setadd ((void **)modes, (void *)cfgn->mode->id, SET_TYPE_STRING);

     for (i = 0; cfgn->arbattrs[i]; i+=2) {
      if (strmatch(cfgn->arbattrs[i], "services")) {
       char **tmps = str2set (':', cfgn->arbattrs[i+1]);

       if (tmps) {
        uint32_t i = 0;

        emutex_lock(&ml_service_list_mutex);

        for (; tmps[i]; i++) {
         if (!streefind (module_logics_service_list, tmps[i], tree_find_first) && !mod_group_get_data(tmps[i])) {
          eprintf (ev->output, " * mode \"%s\": service \"%s\" referenced but not found\n", cfgn->mode->id, tmps[i]);
          ev->ipc_return++;
         }
        }

        emutex_unlock(&ml_service_list_mutex);

        free (tmps);
       }
       break;
      }
     }
    }

    cfgn = cfg_findnode ("mode-enable", 0, cfgn);
   }

   ev->implemented = 1;
  } else if (strmatch (ev->argv[0], "list")) {
   if (strmatch (ev->argv[1], "services")) {
    struct stree *modes = NULL;
    struct stree *cur = NULL;
    struct cfgnode *cfgn = cfg_findnode ("mode-enable", 0, NULL);

    while (cfgn) {
     if (cfgn->arbattrs && cfgn->mode && cfgn->mode->id && (!modes || !streefind (modes, cfgn->mode->id, tree_find_first))) {
      uint32_t i = 0;
      for (i = 0; cfgn->arbattrs[i]; i+=2) {
       if (strmatch(cfgn->arbattrs[i], "services")) {
        char **tmps = str2set (':', cfgn->arbattrs[i+1]);

        modes = streeadd (modes, cfgn->mode->id, tmps, SET_NOALLOC, tmps);

        break;
       }
      }
     }

     cfgn = cfg_findnode ("mode-enable", 0, cfgn);
    }

    emutex_lock(&ml_service_list_mutex);

    cur = module_logics_service_list;

    while (cur) {
     char **inmodes = NULL;
     struct stree *mcur = modes;

     while (mcur) {
      if (inset ((const void **)mcur->value, (void *)cur->key, SET_TYPE_STRING)) {
       inmodes = (char **)setadd((void **)inmodes, (void *)mcur->key, SET_TYPE_STRING);
      }

      mcur = streenext(mcur);
     }

     if (inmodes) {
      char *modestr;
      if (ev->ipc_options & einit_ipc_output_xml) {
       modestr = set2str (':', (const char **)inmodes);
       eprintf (ev->output, " <service id=\"%s\" used-in=\"%s\">\n", cur->key, modestr);
      } else {
       modestr = set2str (' ', (const char **)inmodes);
       eprintf (ev->output, (ev->ipc_options & einit_ipc_output_ansi) ?
                            "\e[1mservice \"%s\" (%s)\n\e[0m" :
                            "service \"%s\" (%s)\n",
                            cur->key, modestr);
      }
      free (modestr);
      free (inmodes);
     } else if (!(ev->ipc_options & einit_ipc_only_relevant)) {
      if (ev->ipc_options & einit_ipc_output_xml) {
       eprintf (ev->output, " <service id=\"%s\">\n", cur->key);
      } else {
       eprintf (ev->output, (ev->ipc_options & einit_ipc_output_ansi) ?
                            "\e[1mservice \"%s\" (not in any mode)\e[0m\n" :
                            "service \"%s\" (not in any mode)\n",
                            cur->key);
      }
     }

     if (inmodes || (!(ev->ipc_options & einit_ipc_only_relevant))) {
      if (ev->ipc_options & einit_ipc_output_xml) {
       if (cur->value) {
        struct lmodule **xs = cur->value;
        uint32_t u = 0;
        for (u = 0; xs[u]; u++) {
         eprintf (ev->output, "  <module id=\"%s\" name=\"%s\" />\n",
                   xs[u]->module && xs[u]->module->rid ? xs[u]->module->rid : "unknown",
                   xs[u]->module && xs[u]->module->name ? xs[u]->module->name : "unknown");
        }
       }

       eputs (" </service>\n", ev->output);
      } else {
       if (cur->value) {
        struct lmodule **xs = cur->value;
        uint32_t u = 0;
        for (u = 0; xs[u]; u++) {
         eprintf (ev->output, (ev->ipc_options & einit_ipc_output_ansi) ?
           ((xs[u]->module && (xs[u]->module->mode & einit_module_deprecated)) ?
                                  " \e[31m- \e[0mcandidate \"%s\" (%s)\n" :
                                  " \e[33m* \e[0mcandidate \"%s\" (%s)\n") :
             " * candidate \"%s\" (%s)\n",
           xs[u]->module && xs[u]->module->rid ? xs[u]->module->rid : "unknown",
           xs[u]->module && xs[u]->module->name ? xs[u]->module->name : "unknown");
        }
       }
      }
     }

     cur = streenext (cur);
    }

    emutex_unlock(&ml_service_list_mutex);

    ev->implemented = 1;
   }
#ifdef DEBUG
   else if (strmatch (ev->argv[1], "control-blocks")) {
    emutex_lock (&ml_tb_target_state_mutex);

    if (target_state.enable) {
     char *r = set2str (' ', (const char **)target_state.enable);
     if (r) {
      eprintf (ev->output, "target_state.enable = { %s }\n", r);
      free (r);
     }
    }
    if (target_state.disable) {
     char *r = set2str (' ', (const char **)target_state.disable);
     if (r) {
      eprintf (ev->output, "target_state.disable = { %s }\n", r);
      free (r);
     }
    }
    if (target_state.critical) {
     char *r = set2str (' ', (const char **)target_state.critical);
     if (r) {
      eprintf (ev->output, "target_state.critical = { %s }\n", r);
      free (r);
     }
    }

    emutex_unlock (&ml_tb_target_state_mutex);
    emutex_lock (&ml_tb_current_mutex);

    if (current.enable) {
     char *r = set2str (' ', (const char **)current.enable);
     if (r) {
      eprintf (ev->output, "current.enable = { %s }\n", r);
      free (r);
     }
    }
    if (current.disable) {
     char *r = set2str (' ', (const char **)current.disable);
     if (r) {
      eprintf (ev->output, "current.disable = { %s }\n", r);
      free (r);
     }
    }
    if (current.critical) {
     char *r = set2str (' ', (const char **)current.critical);
     if (r) {
      eprintf (ev->output, "current.critical = { %s }\n", r);
      free (r);
     }
    }

    emutex_unlock (&ml_tb_current_mutex);

    ev->implemented = 1;
   }
#endif
  }
 }
}

/* end common functions */

/* start new functions */

int mod_gettask (char * service);

pthread_mutex_t
 ml_examine_mutex = PTHREAD_MUTEX_INITIALIZER,
 ml_chain_examine = PTHREAD_MUTEX_INITIALIZER,
 ml_workthreads_mutex = PTHREAD_MUTEX_INITIALIZER,
 ml_commits_mutex = PTHREAD_MUTEX_INITIALIZER,
 ml_changed_mutex = PTHREAD_MUTEX_INITIALIZER;

struct stree *module_logics_chain_examine = NULL; // value is a (char **)
struct stree *module_logics_chain_examine_reverse = NULL;
char **currently_provided = NULL;
char **changed_recently = NULL;
signed char mod_flatten_current_tb_group(char *serv, char task);
void mod_spawn_workthreads ();
char mod_haschanged(char *service);

uint32_t ml_workthreads = 0;
uint32_t ml_commits = 0;
int32_t ignorereorderfor = 0;

char **lm_workthreads_list = NULL;

char mod_workthreads_dec (char *service) {
 emutex_lock (&ml_workthreads_mutex);

 lm_workthreads_list = strsetdel (lm_workthreads_list, service);

 ml_workthreads--;

// eprintf (stderr, "%s: workthreads: %i (%s)\n", service, ml_workthreads, set2str (' ', lm_workthreads_list));
// fflush (stderr);

 if (!ml_workthreads) {
  char spawn = 0;
  uint32_t i = 0;
  emutex_unlock (&ml_workthreads_mutex);

  emutex_lock (&ml_tb_current_mutex);
  if (current.enable) {
   for (i = 0; current.enable[i]; i++) {
    if (!mod_isprovided (current.enable[i]) && !mod_isbroken(current.enable[i])) {
     spawn = 1;
     break;
    }
   }
  }
  if (!spawn && current.disable) {
   for (i = 0; current.disable[i]; i++) {
    if (mod_isprovided (current.disable[i]) && !mod_isbroken(current.disable[i])) {
     spawn = 1;
     break;
    }
   }
  }
  emutex_unlock (&ml_tb_current_mutex);

  if (spawn)
   mod_spawn_workthreads ();

 } else {
  emutex_unlock (&ml_workthreads_mutex);
 }

#if 0
#ifdef _POSIX_PRIORITY_SCHEDULING
 sched_yield();
#endif

 pthread_cond_broadcast (&ml_cond_service_update);
#endif

 return 0;
}

char mod_workthreads_inc (char *service) {
 char retval = 0;
 emutex_lock (&ml_workthreads_mutex);

 if (inset ((const void **)lm_workthreads_list, (void *)service, SET_TYPE_STRING)) {
//  eprintf (stderr, " XX someone's already working on %s...\n", service);
  fflush (stderr);
  retval = 1;
 } else {
  lm_workthreads_list = (char **)setadd((void **)lm_workthreads_list, (void *)service, SET_TYPE_STRING);

  ml_workthreads++;
 }
 emutex_unlock (&ml_workthreads_mutex);

 return retval;
}

void mod_commits_dec () {
 notice (5, "plan finished.");

 char clean_broken = 0, **unresolved = NULL, **broken = NULL;
 emutex_lock (&ml_unresolved_mutex);
 if (broken_services) {
  broken = (char **)setdup ((const void **)broken_services, SET_TYPE_STRING);
 }
 if (unresolved_services) {
  unresolved = (char **)setdup ((const void **)unresolved_services, SET_TYPE_STRING);
 }
 emutex_unlock (&ml_unresolved_mutex);

 if (broken) {
  struct einit_event ee = evstaticinit(einit_feedback_broken_services);
  ee.set = (void **)broken;

  event_emit (&ee, einit_event_flag_broadcast);
  evstaticdestroy (ee);

  free (broken);
 }
 if (unresolved) {
  struct einit_event ee = evstaticinit(einit_feedback_unresolved_services);
  ee.set = (void **)unresolved;

  event_emit (&ee, einit_event_flag_broadcast);
  evstaticdestroy (ee);

  free (unresolved);
 }

 emutex_lock (&ml_commits_mutex);
 ml_commits--;
 clean_broken = (ml_commits == 0);
 emutex_unlock (&ml_commits_mutex);

 if (clean_broken) {
  emutex_lock (&ml_unresolved_mutex);
  if (unresolved_services) {
   free (unresolved_services);
   unresolved_services = NULL;
  }
  if (broken_services) {
   free (broken_services);
   broken_services = NULL;
  }
  emutex_unlock (&ml_unresolved_mutex);

  emutex_lock (&ml_changed_mutex);
  if (changed_recently) {
   free (changed_recently);
   changed_recently = NULL;
  }
  emutex_unlock (&ml_changed_mutex);

  emutex_lock(&ml_chain_examine);
  if (module_logics_chain_examine) {
   streefree (module_logics_chain_examine);
   module_logics_chain_examine = NULL;
  }
  if (module_logics_chain_examine_reverse) {
   streefree (module_logics_chain_examine_reverse);
   module_logics_chain_examine_reverse = NULL;
  }
  emutex_unlock(&ml_chain_examine);
 }

#if 0
#ifdef _POSIX_PRIORITY_SCHEDULING
 sched_yield();
#endif

 pthread_cond_broadcast (&ml_cond_service_update);
#endif
}

void mod_commits_inc () {
// char spawn = 0;
 notice (5, "plan started.");

 emutex_lock (&ml_commits_mutex);
 ml_commits++;
 emutex_unlock (&ml_commits_mutex);

// emutex_lock (&ml_workthreads_mutex);
// spawn = (ml_workthreads == 0);
// emutex_unlock (&ml_workthreads_mutex);

// if (spawn)
 mod_spawn_workthreads ();
}

void mod_defer_notice (struct lmodule *mod, char **services) {
 char tmp[BUFFERSIZE];
 char *s = set2str (' ', (const char **)services);

 struct einit_event ee = evstaticinit (einit_feedback_module_status);
 mod->status |= status_deferred;

 ee.module = mod;
 ee.status = status_deferred;

 if (s) {
  esprintf (tmp, BUFFERSIZE, "queued after: %s", s);
  ee.string = estrdup(tmp);
 }

 event_emit (&ee, einit_event_flag_broadcast);
 evstaticdestroy (ee);

 if (s) free (s);
}

void mod_defer_until (char *service, char *after) {
 struct stree *xn = NULL;

#ifdef DEBUG
 eprintf (stderr, " ** deferring %s until after %s\n", service, after);
#endif

 emutex_lock(&ml_chain_examine);

 if ((xn = streefind (module_logics_chain_examine, after, tree_find_first))) {
  if (!inset ((const void **)xn->value, service, SET_TYPE_STRING)) {
   char **n = (char **)setadd ((void **)xn->value, service, SET_TYPE_STRING);

   xn->value = (void *)n;
   xn->luggage = (void *)n;
  }
 } else {
  char **n = (char **)setadd ((void **)NULL, service, SET_TYPE_STRING);

  module_logics_chain_examine =
   streeadd(module_logics_chain_examine, after, n, SET_NOALLOC, n);
 }

 if ((xn = streefind (module_logics_chain_examine_reverse, service, tree_find_first))) {
  if (!inset ((const void **)xn->value, after, SET_TYPE_STRING)) {
   char **n = (char **)setadd ((void **)xn->value, after, SET_TYPE_STRING);

   xn->value = (void *)n;
   xn->luggage = (void *)n;
  }
 } else {
  char **n = (char **)setadd ((void **)NULL, after, SET_TYPE_STRING);

  module_logics_chain_examine_reverse =
   streeadd(module_logics_chain_examine_reverse, service, n, SET_NOALLOC, n);
 }

 emutex_unlock(&ml_chain_examine);

#if 0
#ifdef _POSIX_PRIORITY_SCHEDULING
 sched_yield();
#endif

 pthread_cond_broadcast (&ml_cond_service_update);
#endif
}

void mod_remove_defer (char *service) {
 struct stree *xn = NULL;
 emutex_lock(&ml_chain_examine);

 if ((xn = streefind (module_logics_chain_examine_reverse, service, tree_find_first))) {
  uint32_t i = 0;

  if (xn->value) {
   for (; ((char **)xn->value)[i]; i++) {
    struct stree *yn = streefind (module_logics_chain_examine, ((char **)xn->value)[i], tree_find_first);

    if (yn) {
     yn->value = (void *)strsetdel ((char **)yn->value, ((char **)xn->value)[i]);

     if (!yn->value) {
      module_logics_chain_examine = streedel (yn);
     } else {
      yn->luggage = yn->value;
     }
    }
   }
  }

  module_logics_chain_examine_reverse = streedel (xn);
 }

 emutex_unlock(&ml_chain_examine);
}

void mod_decrease_deferred_by (char *service) {
 struct stree *xn = NULL;
 char **do_examine = NULL;

 emutex_lock(&ml_chain_examine);

 if ((xn = streefind (module_logics_chain_examine, service, tree_find_first))) {
  uint32_t i = 0;

  if (xn->value) {
   for (; ((char **)xn->value)[i]; i++) {
    struct stree *yn = streefind (module_logics_chain_examine_reverse, ((char **)xn->value)[i], tree_find_first);

    if (yn) {
     yn->value = (void *)strsetdel ((char **)yn->value, ((char **)xn->value)[i]);
     yn->luggage = yn->value;

     if (!yn->value) {
      do_examine = (char **)setadd ((void **)do_examine, yn->key, SET_TYPE_STRING);

      module_logics_chain_examine_reverse = streedel (yn);
     }
    }
   }
  }

  module_logics_chain_examine = streedel (xn);
 }

 emutex_unlock(&ml_chain_examine);

 if (do_examine) {
  uint32_t i = 0;

  for (; do_examine[i]; i++) {
   mod_examine (do_examine[i]);
  }
  free (do_examine);
 }
}

char mod_isdeferred (char *service) {
 char ret = 0;

 emutex_lock(&ml_chain_examine);

 struct stree *r =
  streefind (module_logics_chain_examine_reverse, service, tree_find_first);

#if 0
 if (r) {
  char **deferrees = r->value;
  uint32_t i = 0;

  for (; deferrees[i]; i++) {
   eprintf (stderr, " -- %s: deferred by %s\n", service, deferrees[i]);
  }
 }
#endif

 emutex_unlock(&ml_chain_examine);

 ret = r != NULL;

 return ret;
}

char mod_mark (char *service, char task) {
 char retval = 0;

 emutex_lock (&ml_unresolved_mutex);

 if ((task & MARK_BROKEN) && !inset ((const void **)broken_services, (void *)service, SET_TYPE_STRING)) {
  broken_services = (char **)setadd ((void **)broken_services, (void *)service, SET_TYPE_STRING);
 }
 if ((task & MARK_UNRESOLVED) && !inset ((const void **)unresolved_services, (void *)service, SET_TYPE_STRING)) {
  unresolved_services = (char **)setadd ((void **)unresolved_services, (void *)service, SET_TYPE_STRING);
 }

 emutex_unlock (&ml_unresolved_mutex);

 mod_remove_defer (service);

#ifdef _POSIX_PRIORITY_SCHEDULING
 sched_yield();
#endif

 pthread_cond_broadcast (&ml_cond_service_update);

 return retval;
}

char mod_isprovided(char *service) {
/* char ret = 0;

 emutex_lock (&ml_currently_provided_mutex);

 ret = inset ((const void **)currently_provided, (const void *)service, SET_TYPE_STRING);

 emutex_unlock (&ml_currently_provided_mutex);*/

 return service_usage_query (service_is_provided, NULL, service);
}

char mod_haschanged(char *service) {
 char ret = 0;

 emutex_lock (&ml_changed_mutex);

 ret = inset ((const void **)changed_recently, (const void *)service, SET_TYPE_STRING);

 emutex_unlock (&ml_changed_mutex);

 return ret;
}

void mod_queue_enable (char *service) {
 emutex_lock (&ml_tb_current_mutex);

 current.enable = (char **)setadd ((void **)current.enable, (const void *)service, SET_TYPE_STRING);
 current.disable = strsetdel ((char **)current.disable, (void *)service);

 emutex_unlock (&ml_tb_current_mutex);

 mod_examine (service);
}

void mod_queue_disable (char *service) {
 emutex_lock (&ml_tb_current_mutex);

 current.disable = (char **)setadd ((void **)current.disable, (const void *)service, SET_TYPE_STRING);
 current.enable = strsetdel ((char **)current.enable, (void *)service);

 emutex_unlock (&ml_tb_current_mutex);

 mod_examine (service);
}

signed char mod_flatten_current_tb_group(char *serv, char task) {
 struct group_data *gd = mod_group_get_data (serv);

#ifdef DEBUG
 eputs ("g", stderr);
 fflush (stderr);
#endif

 if (gd) {
  uint32_t changes = 0;
  char *service = estrdup (serv);

  if (!gd->members || !gd->members[0])
   return -1;

  if (gd->options & (MOD_PLAN_GROUP_SEQ_ANY | MOD_PLAN_GROUP_SEQ_ANY_IOP)) {
   uint32_t i = 0;

   for (; gd->members[i]; i++) {
    if (((task & einit_module_enable) && mod_isprovided (gd->members[i])) ||
          ((task & einit_module_disable) && !mod_isprovided (gd->members[i]))) {
     free (service);
     return 0;
    }

    if (mod_isbroken (gd->members[i])) {
     continue;
    }

    if (!inset ((const void **)(task & einit_module_enable ? current.enable : current.disable), gd->members[i], SET_TYPE_STRING)) {
     changes++;

     if (task & einit_module_enable) {
      current.enable = (char **)setadd ((void **)current.enable, (const void *)gd->members[i], SET_TYPE_STRING);
     } else {
      current.disable = (char **)setadd ((void **)current.disable, (const void *)gd->members[i], SET_TYPE_STRING);
     }

     mod_defer_until(service, gd->members[i]);

     free (service);
     return 1;
    }

    free (service);
    return 0;
   }

   notice (2, "marking group %s broken (...)", service);

   mod_mark (service, MARK_BROKEN);
  } else { // MOD_PLAN_GROUP_SEQ_ALL | MOD_PLAN_GROUP_SEQ_MOST
   uint32_t i = 0, bc = 0, sc = 0;

   for (; gd->members[i]; i++) {
    if (((task & einit_module_enable) && mod_isprovided (gd->members[i])) ||
        ((task & einit_module_disable) && !mod_isprovided (gd->members[i]))) {
#ifdef DEBUG
     eprintf (stderr, "%s: skipping %s (already in proper state)\n", service, gd->members[i]);
#endif

     sc++;
     continue;
    }

    if (mod_isbroken (gd->members[i])) {
#ifdef DEBUG
     eprintf (stderr, "%s: skipping %s (broken)\n", service, gd->members[i]);
#endif

     bc++;
     continue;
    }

    if (!inset ((const void **)(task & einit_module_enable ? current.enable : current.disable), gd->members[i], SET_TYPE_STRING)) {
     changes++;

#ifdef DEBUG
     eprintf (stderr, "%s: deferring after %s\n", service, gd->members[i]);
#endif

     mod_defer_until(service, gd->members[i]);

     if (task & einit_module_enable) {
      current.enable = (char **)setadd ((void **)current.enable, (const void *)gd->members[i], SET_TYPE_STRING);
     } else {
      current.disable = (char **)setadd ((void **)current.disable, (const void *)gd->members[i], SET_TYPE_STRING);
     }
    }
   }

#ifdef DEBUG
   notice (6, "group %s: i=%i; sc=%i; bc=%i\n", service, i, sc, bc);
#endif

   if (bc) {
    if (bc == i) {
     notice (5, "group %s broken!\n", service);

     mod_mark (service, MARK_BROKEN);
    } else if (gd->options & MOD_PLAN_GROUP_SEQ_ALL) {
     notice (5, "group %s broken!\n", service);

     mod_mark (service, MARK_BROKEN);
    }
   }
  }

  free (service);
  return changes != 0;
 }

 return -1;
}

signed char mod_flatten_current_tb_module(char *serv, char task) {
 emutex_lock (&ml_service_list_mutex);
 struct stree *xn = streefind (module_logics_service_list, serv, tree_find_first);

#ifdef DEBUG
 eputs ("m", stderr);
 fflush (stderr);
#endif

 if (xn && xn->value) {
  struct lmodule **lm = xn->value;
  uint32_t changes = 0;
  char *service = estrdup (serv);

  if (task & einit_module_enable) {
   struct lmodule *first = lm[0];
   char broken = 0, rotate = 0;

   do {
/*    eputs (".", stderr);
    fflush (stderr);*/
    struct lmodule *rcurrent = lm[0];

    rotate = 0;
    broken = 0;

    first = 0;
    if (rcurrent && rcurrent->si && rcurrent->si->requires) {
     uint32_t i = 0;

     for (; rcurrent->si->requires[i]; i++) {
      if (mod_isprovided (rcurrent->si->requires[i])) {
       continue;
      }

      if (mod_isbroken (rcurrent->si->requires[i])) {
       rotate = 1;
       broken = 1;

       break;
      }

      if (!inset ((const void **)current.enable, (const void *)rcurrent->si->requires[i], SET_TYPE_STRING)) {
       current.enable = (char **)setadd ((void **)current.enable, (const void *)rcurrent->si->requires[i], SET_TYPE_STRING);

       changes++;
      }

      broken = 0;
     }
    }

    if (rotate && lm[1]) {
     ssize_t rx = 1;

     for (; lm[rx]; rx++) {
      lm[rx-1] = lm[rx];
     }

     lm[rx-1] = rcurrent;
    } else {
     rotate = 0;
    }
   } while (broken && rotate && (lm[0] != first));

   if (broken) {
    notice (2, "marking module %s broken (broken != 0)", service);

    mod_mark (service, MARK_BROKEN);
   }
  } else { /* disable... */
   uint32_t z = 0;

   for (; lm[z]; z++) {
    char **t = service_usage_query_cr (service_get_services_that_use, lm[z], NULL);

    if (t) {
     uint32_t i = 0;

     for (; t[i]; i++) {
      if (!mod_isprovided (t[i]) || mod_isbroken (t[i])) {
       continue;
      }

      if (!inset ((const void **)current.disable, (const void *)t[i], SET_TYPE_STRING)) {
       current.disable = (char **)setadd ((void **)current.disable, (const void *)t[i], SET_TYPE_STRING);

       changes++;
      }
     }
    }
   }
  }

  emutex_unlock (&ml_service_list_mutex);

  free (service);

  return changes != 0;
 }

 emutex_unlock (&ml_service_list_mutex);

 return -1;
}

void mod_flatten_current_tb () {
 emutex_lock (&ml_tb_current_mutex);

 repeat_ena:
 if (current.enable) {
  uint32_t i;

#ifdef DEBUG
  eputs ("e", stderr);
  fflush (stderr);
#endif

  for (i = 0; current.enable[i]; i++) {
   signed char t = 0;
   if (mod_isprovided (current.enable[i]) || mod_isbroken(current.enable[i])) {
    current.enable = (char **)setdel ((void **)current.enable, current.enable[i]);
    goto repeat_ena;
   }

#ifdef DEBUG
   eputs ("+", stderr);
/*   eputs (current.enable[i], stderr);*/
   fflush (stderr);
#endif

   if (((t = mod_flatten_current_tb_group(current.enable[i], einit_module_enable)) == -1) &&
       ((t = mod_flatten_current_tb_module(current.enable[i], einit_module_enable)) == -1)) {
    notice (2, "can't resolve service %s\n", current.enable[i]);

    mod_mark (current.enable[i], MARK_UNRESOLVED);
   } else {
    if (t) {
     goto repeat_ena;
    }
   }

#ifdef DEBUG
   eputs ("-", stderr);
   fflush (stderr);
#endif
  }

  for (i = 0; current.enable[i]; i++) {
   struct stree *xn = streefind (module_logics_service_list, current.enable[i], tree_find_first);

   if (!mod_group_get_data (current.enable[i]) && xn && xn->value) {
    struct lmodule **lm = xn->value;
    mod_defer_notice (lm[0], NULL);
   }
  }
 }

 repeat_disa:
 if (current.disable) {
  uint32_t i;

#ifdef DEBUG
  eputs ("d", stderr);
  fflush (stderr);
#endif

  for (i = 0; current.disable[i]; i++) {
   signed char t = 0;
   if (!mod_isprovided (current.disable[i]) || mod_isbroken(current.disable[i])) {
    current.disable = (char **)setdel ((void **)current.disable, current.disable[i]);
    goto repeat_disa;
   }

#ifdef DEBUG
   eputs ("z", stderr);
   fflush (stderr);
#endif

   if (((t = mod_flatten_current_tb_group(current.disable[i], einit_module_disable)) == -1) &&
       ((t = mod_flatten_current_tb_module(current.disable[i], einit_module_disable)) == -1)) {
    notice (2, "can't resolve service %s\n", current.disable[i]);

    mod_mark (current.disable[i], MARK_UNRESOLVED);
   } else {
    if (t) {
     goto repeat_disa;
    }
   }

#ifdef DEBUG
   eputs ("!", stderr);
   fflush (stderr);
#endif
  }

  for (i = 0; current.disable[i]; i++) {
   struct stree *xn = streefind (module_logics_service_list, current.disable[i], tree_find_first);

   if (!mod_group_get_data (current.disable[i]) && xn && xn->value) {
    struct lmodule **lm = xn->value;
    mod_defer_notice (lm[0], NULL);
   }
  }
 }

#ifdef DEBUG
 eputs ("R", stderr);
 fflush (stderr);
#endif

 emutex_unlock (&ml_tb_current_mutex);
}

void mod_examine_module (struct lmodule *module) {
 if (!(module->status & status_working)) {
  if (module->si && module->si->provides) {
   uint32_t i = 0;

   if (module->status & status_enabled) {
//    eprintf (stderr, " ** service enabled, module=%s...\n", module->module->rid);

    emutex_lock (&ml_tb_current_mutex);
    current.enable = (char **)setslice_nc ((void **)current.enable, (const void **)module->si->provides, SET_TYPE_STRING);
    emutex_unlock (&ml_tb_current_mutex);

    emutex_lock (&ml_currently_provided_mutex);
    currently_provided = (char **)setcombine_nc ((void **)currently_provided, (const void **)module->si->provides, SET_TYPE_STRING);
    emutex_unlock (&ml_currently_provided_mutex);

    emutex_lock (&ml_changed_mutex);
    changed_recently = (char **)setcombine_nc ((void **)changed_recently, (const void **)module->si->provides, SET_TYPE_STRING);
    emutex_unlock (&ml_changed_mutex);

#ifdef _POSIX_PRIORITY_SCHEDULING
    sched_yield();
#endif

    pthread_cond_broadcast (&ml_cond_service_update);
   } else if ((module->status & status_disabled) || (module->status == status_idle)) {
//    eputs ("service disabled...\n", stderr);

    emutex_lock (&ml_tb_current_mutex);
    current.disable = (char **)setslice_nc ((void **)current.disable, (const void **)module->si->provides, SET_TYPE_STRING);
    emutex_unlock (&ml_tb_current_mutex);

    emutex_lock (&ml_currently_provided_mutex);
    currently_provided = (char **)setslice_nc ((void **)currently_provided, (const void **)module->si->provides, SET_TYPE_STRING);
    emutex_unlock (&ml_currently_provided_mutex);

    emutex_lock (&ml_changed_mutex);
    changed_recently = (char **)setcombine_nc ((void **)changed_recently, (const void **)module->si->provides, SET_TYPE_STRING);
    emutex_unlock (&ml_changed_mutex);

#ifdef _POSIX_PRIORITY_SCHEDULING
    sched_yield();
#endif

    pthread_cond_broadcast (&ml_cond_service_update);
   }

   for (; module->si->provides[i]; i++) {
    mod_examine (module->si->provides[i]);
   }
  }
 }

#if 0
#ifdef _POSIX_PRIORITY_SCHEDULING
 sched_yield();
#endif

 pthread_cond_broadcast (&ml_cond_service_update);
#endif
}

void mod_post_examine (char *service) {
 char **pex = NULL;
 struct stree *post_examine;

 emutex_lock (&ml_chain_examine);

 if ((post_examine = streefind (module_logics_chain_examine, service, tree_find_first))) {
  pex = (char **)setdup ((const void **)post_examine->value, SET_TYPE_STRING);
 }

 emutex_unlock (&ml_chain_examine);

 mod_decrease_deferred_by (service);

 if (pex) {
  uint32_t j = 0;

  for (; pex[j]; j++) {
   mod_examine (pex[j]);
  }

  free (pex);
 }
}

void mod_pre_examine (char *service) {
 char **pex = NULL;
 struct stree *post_examine;

 emutex_lock (&ml_chain_examine);

 if ((post_examine = streefind (module_logics_chain_examine_reverse, service, tree_find_first))) {
  pex = (char **)setdup ((const void **)post_examine->value, SET_TYPE_STRING);
 }

 emutex_unlock (&ml_chain_examine);

 if (pex) {
  uint32_t j = 0, broken = 0, done = 0;

  for (; pex[j]; j++) {
   if (mod_isbroken (pex[j])) {
    broken++;
   } else {
    int task = mod_gettask (service);

    if (!task ||
        ((task & einit_module_enable) && mod_isprovided (pex[j])) ||
        ((task & einit_module_disable) && !mod_isprovided (pex[j]))) {
     done++;

     mod_post_examine (pex[j]);
    }

    mod_examine (pex[j]);
   }
  }
  free (pex);

  if ((broken + done) == j) {
   mod_remove_defer (service);
   mod_examine (service);
  }
 }
}

char mod_disable_users (struct lmodule *module) {
 if (!service_usage_query(service_not_in_use, module, NULL)) {
  ssize_t i = 0;
  char **need = NULL;
  char **t = service_usage_query_cr (service_get_services_that_use, module, NULL);
  char retval = 1;

  if (t) {
   for (; t[i]; i++) {
    if (mod_isbroken (t[i])) {
     if (need) free (need);
     return 0;
    } else {
     emutex_lock (&ml_tb_current_mutex);

     if (!inset ((const void **)current.disable, (void *)t[i], SET_TYPE_STRING)) {
      retval = 2;
      need = (char **)setadd ((void **)need, t[i], SET_TYPE_STRING);

      if (module->si && module->si->provides) {
       uint32_t y = 0;
       for (; module->si->provides[y]; y++) {
        mod_defer_until (module->si->provides[y], t[i]);
       }
      }
     }

     emutex_unlock (&ml_tb_current_mutex);
    }
   }

   if (retval == 2) {
    mod_defer_notice (module, need);

    emutex_lock (&ml_tb_current_mutex);

    char **tmp = (char **)setcombine ((const void **)current.disable, (const void **)need, SET_TYPE_STRING);
    if (current.disable) free (current.disable);
    current.disable = tmp;

    emutex_unlock (&ml_tb_current_mutex);

    for (i = 0; need[i]; i++) {
     mod_examine (need[i]);
    }
   }
  }

  return retval;
 }

 return 0;
}

char mod_enable_requirements (struct lmodule *module) {
 if (!service_usage_query(service_requirements_met, module, NULL)) {
  char retval = 1;
  if (module->si && module->si->requires) {
   ssize_t i = 0;
   char **need = NULL;

   for (; module->si->requires[i]; i++) {
    if (mod_isbroken (module->si->requires[i])) {
     if (need) free (need);
     return 0;
    } else if (!service_usage_query (service_is_provided, NULL, module->si->requires[i])) {
     emutex_lock (&ml_tb_current_mutex);

#ifdef DEBUG
     notice (4, "(%s) still need %s:", set2str(' ', module->si->provides), module->si->requires[i]);
#endif

     if (!inset ((const void **)current.enable, (void *)module->si->requires[i], SET_TYPE_STRING)) {
      retval = 2;
      need = (char **)setadd ((void **)need, module->si->requires[i], SET_TYPE_STRING);

      if (module->si && module->si->provides) {
       uint32_t y = 0;
       for (; module->si->provides[y]; y++) {
        mod_defer_until (module->si->provides[y], module->si->requires[i]);
       }
      }
     }

     emutex_unlock (&ml_tb_current_mutex);
    }
   }

   if (retval == 2) {
    mod_defer_notice (module, need);

    emutex_lock (&ml_tb_current_mutex);

    char **tmp = (char **)setcombine ((const void **)current.enable, (const void **)need, SET_TYPE_STRING);
    if (current.enable) free (current.enable);
    current.enable = tmp;

    emutex_unlock (&ml_tb_current_mutex);

    for (i = 0; need[i]; i++) {
     mod_examine (need[i]);
    }
   }
  }

  return retval;
 }

 return 0;
}

void mod_apply_enable (struct stree *des) {
 if (!des) return;
  struct lmodule **lm = (struct lmodule **)des->value;

  if (lm && lm[0]) {
   struct lmodule *first = lm[0];

   do {
    struct lmodule *current = lm[0];

    if ((current->status & status_enabled) || mod_enable_requirements (current)) {
#ifdef DEBUG
     notice (4, "not spawning thread thread for %s; exiting (not quite there yet)", des->key);
#endif

     mod_workthreads_dec(des->key);
     return;
    }

    mod (einit_module_enable, current, NULL);

/* check module status or return value to find out if it's appropriate for the task */
    if (current->status & status_enabled) {
#ifdef DEBUG
     notice (4, "not spawning thread thread for %s; exiting (already up)", des->key);
#endif

     mod_workthreads_dec(des->key);
     return;
    }

/* next module */
    emutex_lock (&ml_service_list_mutex);

/* make sure there's not been a different thread that did what we want to do */
    if ((lm[0] == current) && lm[1]) {
     ssize_t rx = 1;

     notice (10, "service %s: done with module %s, rotating the list", des->key, (current->module && current->module->rid ? current->module->rid : "unknown"));

     for (; lm[rx]; rx++) {
      lm[rx-1] = lm[rx];
     }

     lm[rx-1] = current;
    }

    emutex_unlock (&ml_service_list_mutex);
   } while (lm[0] != first);
/* if we tried to enable something and end up here, it means we did a complete
   round-trip and nothing worked */

   emutex_lock (&ml_tb_current_mutex);
   current.enable = strsetdel(current.enable, des->key);
   emutex_unlock (&ml_tb_current_mutex);

/* mark service broken if stuff went completely wrong */
   notice (2, "ran out of options for service %s (enable), marking as broken", des->key);

   mod_mark (des->key, MARK_BROKEN);
  }

#ifdef DEBUG
  notice (4, "not spawning thread thread for %s; exiting (end of function)", des->key);
#endif

  mod_workthreads_dec(des->key);
  return;
}

void mod_apply_disable (struct stree *des) {
 if (!des) return;

  struct lmodule **lm = (struct lmodule **)des->value;

  if (lm && lm[0]) {
   struct lmodule *first = lm[0];
   char any_ok = 0, failures = 0;

   do {
    struct lmodule *current = lm[0];

    if ((current->status & status_disabled) || (current->status == status_idle)) {
//     eprintf (stderr, "%s (%s) disabled...", des->key, current->module->rid);
     any_ok = 1;
     goto skip_module;
    }

    if (mod_disable_users (current)) {
//     eprintf (stderr, "cannot disable %s yet...", des->key);
     mod_workthreads_dec(des->key);
     return;
    }

    mod (einit_module_disable, current, NULL);

    /* check module status or return value to find out if it's appropriate for the task */
    if ((current->status & status_disabled) || (current->status == status_idle)) {
     any_ok = 1;
    } else {
//     eputs ("gonna ZAPP! something later...\n", stderr);
     failures = 1;
    }

    skip_module:
/* next module */
    emutex_lock (&ml_service_list_mutex);

/* make sure there's not been a different thread that did what we want to do */
    if ((lm[0] == current) && lm[1]) {
     ssize_t rx = 1;

     notice (10, "service %s: done with module %s, rotating the list", des->key, (current->module && current->module->rid ? current->module->rid : "unknown"));

     for (; lm[rx]; rx++) {
      lm[rx-1] = lm[rx];
     }

     lm[rx-1] = current;
    }

    emutex_unlock (&ml_service_list_mutex);
   } while (lm[0] != first);
/* if we tried to enable something and end up here, it means we did a complete
   round-trip and nothing worked */

/* zap stuff that's broken */
/*   if (failures) {
    eputs ("ZAPP...?\n", stderr);
   }*/
   if (shutting_down && failures) {
    struct lmodule *first = lm[0];

    notice (1, "was forced to ZAPP! %s", des->key);

    do {
     struct lmodule *current = lm[0];

     if (current->status & status_enabled)
      mod (einit_module_custom, current, "zap");

     emutex_lock (&ml_service_list_mutex);
     if ((lm[0] == current) && lm[1]) {
      ssize_t rx = 1;

      notice (10, "service %s: done with module %s, rotating the list", des->key, (current->module && current->module->rid ? current->module->rid : "unknown"));

      for (; lm[rx]; rx++) {
       lm[rx-1] = lm[rx];
      }

      lm[rx-1] = current;
     }
     emutex_unlock (&ml_service_list_mutex);
    } while (lm[0] != first);
   }

   emutex_lock (&ml_tb_current_mutex);
   current.disable = strsetdel(current.disable, des->key);
   emutex_unlock (&ml_tb_current_mutex);

   if (any_ok) {
    mod_workthreads_dec(des->key);
    return;
   }

/* mark service broken if stuff went completely wrong */
   notice (2, "ran out of options for service %s (disable), marking as broken", des->key);

   mod_mark (des->key, MARK_BROKEN);
  }

  mod_workthreads_dec(des->key);
  return;
}

int mod_gettask (char * service) {
 int task = 0;

 emutex_lock (&ml_tb_current_mutex);
 if (inset ((const void **)current.disable, service, SET_TYPE_STRING))
  task = einit_module_disable;
 else if (inset ((const void **)current.enable, service, SET_TYPE_STRING))
  task = einit_module_enable;
 emutex_unlock (&ml_tb_current_mutex);

 return task;
}

char mod_examine_group (char *groupname) {
 struct group_data *gd = mod_group_get_data (groupname);
 if (!gd) return 0;
 char post_examine = 0;
 char **members = NULL;
 uint32_t options = 0;

 emutex_lock (&ml_group_data_mutex);
 if (gd->members) {
  members = (char **)setdup ((const void **)gd->members, SET_TYPE_STRING);
 }
 options = gd->options;
 emutex_unlock (&ml_group_data_mutex);

 if (members) {
  int task = mod_gettask (groupname);

//  notice (2, "group %s: examining members", groupname);

  ssize_t x = 0, mem = setcount ((const void **)members), failed = 0, on = 0, off = 0, groupc = 0;
  struct lmodule **providers = NULL;
  char group_failed = 0, group_ok = 0;

  for (; members[x]; x++) {
   if (mod_isbroken (members[x])) {
    failed++;
   } else {
    struct stree *serv = NULL;

    emutex_lock (&ml_service_list_mutex);

    if (mod_isprovided(members[x])) {
     on++;

     if (module_logics_service_list && (serv = streefind(module_logics_service_list, members[x], tree_find_first))) {
      struct lmodule **lm = (struct lmodule **)serv->value;

      if (lm) {
       ssize_t y = 0;

       for (; lm[y]; y++) {
        if ((lm[y]->status & status_enabled) && (!providers || !inset ((const void **)providers, (const void *)lm[y], SET_NOALLOC))) {
         providers = (struct lmodule **)setadd ((void **)providers, (void *)lm[y], SET_NOALLOC);

         break;
        }
       }
      }
     } else {
      struct lmodule **lm = (struct lmodule **)service_usage_query_cr (service_get_providers, NULL, members[x]);

      groupc++; /* must be a group... */

      if (lm) {
       ssize_t y = 0;

       for (; lm[y]; y++) {
        if (!providers || !inset ((const void **)providers, (const void *)lm[y], SET_NOALLOC)) {
         providers = (struct lmodule **)setadd ((void **)providers, (void *)lm[y], SET_NOALLOC);

#ifdef DEBUG
         eprintf (stderr, " ** group %s provided by %s (groupc=%i)", groupname, lm[y]->module->name, groupc);
#endif
        }
       }
      }
     }
    }
   }

   emutex_unlock (&ml_service_list_mutex);
  }

  if (!on || ((task & einit_module_disable) && (on == groupc))) {
   if (mod_isprovided (groupname)) {
    emutex_lock (&ml_currently_provided_mutex);
    currently_provided = (char **)strsetdel ((char **)currently_provided, (char *)groupname);
    emutex_unlock (&ml_currently_provided_mutex);

    emutex_lock (&ml_tb_current_mutex);
    current.enable = strsetdel (current.enable, groupname);
    current.disable = strsetdel (current.disable, groupname);
    emutex_unlock (&ml_tb_current_mutex);

    emutex_lock (&ml_changed_mutex);
    if (!inset ((const void **)changed_recently, (const void *)groupname, SET_TYPE_STRING))
     changed_recently = (char **)setadd ((void **)changed_recently, (const void *)groupname, SET_TYPE_STRING);
    emutex_unlock (&ml_changed_mutex);

    notice (2, "marking group %s off", groupname);

    post_examine = 1;

#ifdef _POSIX_PRIORITY_SCHEDULING
    sched_yield();
#endif

    pthread_cond_broadcast (&ml_cond_service_update);
   }

   if (task & einit_module_disable) {
    post_examine = 1;
   }
  }

  if (task & einit_module_enable) {
   if (on) {
    if (options & (MOD_PLAN_GROUP_SEQ_ANY | MOD_PLAN_GROUP_SEQ_ANY_IOP)) {
     if (on > 0) {
      group_ok = 1;
     }
    } else if (options & MOD_PLAN_GROUP_SEQ_MOST) {
     if (on && ((on + off + failed) >= mem)) {
      group_ok = 1;
     }
    } else if (options & MOD_PLAN_GROUP_SEQ_ALL) {
     if (on >= mem) {
      group_ok = 1;
     } else if (failed) {
      group_failed = 1;
     }
    } else {
     notice (2, "marking group %s broken (bad group type)", groupname);

     mod_mark (groupname, MARK_BROKEN);
    }
   } else {
    if (failed >= mem) {
     group_failed = 1;
    }
   }

   if (group_ok) {
    notice (2, "marking group %s up", groupname);

    emutex_lock (&ml_tb_current_mutex);
    current.enable = strsetdel (current.enable, groupname);
    current.disable = strsetdel (current.disable, groupname);
    emutex_unlock (&ml_tb_current_mutex);

    emutex_lock (&ml_changed_mutex);
    if (!inset ((const void **)changed_recently, (const void *)groupname, SET_TYPE_STRING))
     changed_recently = (char **)setadd ((void **)changed_recently, (const void *)groupname, SET_TYPE_STRING);
    emutex_unlock (&ml_changed_mutex);

    service_usage_query_group (service_set_group_providers, (struct lmodule *)providers, groupname);

    if (!mod_isprovided (groupname)) {
     emutex_lock (&ml_currently_provided_mutex);
     currently_provided = (char **)setadd ((void **)currently_provided, (void *)groupname, SET_TYPE_STRING);
     emutex_unlock (&ml_currently_provided_mutex);
    }

    post_examine = 1;

#ifdef _POSIX_PRIORITY_SCHEDULING
    sched_yield();
#endif

    pthread_cond_broadcast (&ml_cond_service_update);
   } else if (group_failed) {
    notice (2, "marking group %s broken (group requirements failed)", groupname);

    mod_mark (groupname, MARK_BROKEN);
   } else {
/* just to make sure everything will actually be enabled/disabled */
    emutex_lock (&ml_tb_current_mutex);
    mod_flatten_current_tb_group(groupname, task);
    emutex_unlock (&ml_tb_current_mutex);
   }
  } else { /* mod_disable */
/* just to make sure everything will actually be enabled/disabled */
   emutex_lock (&ml_tb_current_mutex);
   mod_flatten_current_tb_group(groupname, task);
   emutex_unlock (&ml_tb_current_mutex);
  }

  if (providers) free (providers);

  free (members);
 }

 if (post_examine) {
  mod_post_examine (groupname);
 }

#if 0
#ifdef _POSIX_PRIORITY_SCHEDULING
 sched_yield();
#endif

 pthread_cond_broadcast (&ml_cond_service_update);
#endif
 return 1;
}

char mod_reorder (struct lmodule *lm, int task, char *service, char dolock) {
 char **before = NULL, **after = NULL, **xbefore = NULL, hd = 0;

 if (!lm) {
  fflush (stderr);
  emutex_lock (&ml_service_list_mutex);
  struct stree *v = streefind (module_logics_service_list, service, tree_find_first);
  struct lmodule **lmx = v ? v->value : NULL;
  emutex_unlock (&ml_service_list_mutex);
  fflush (stderr);

  if (lmx && lmx[0]) lm = lmx[0];
  else return 0;
 }

 if (lm->si && (lm->si->before || lm->si->after)) {
  if (task & einit_module_enable) {
   before = lm->si->before;
   after = lm->si->after;
  } else if (task & einit_module_disable) {
   if (shutting_down && lm->si->shutdown_before)
    before = lm->si->shutdown_before;
   else
    before = lm->si->after;

   if (shutting_down && lm->si->shutdown_after)
    after = lm->si->shutdown_after;
   else
    after = lm->si->before;
  }
 }

 /* "loose" service ordering */
 if (before) {
  uint32_t i = 0;

  for (; before[i]; i++) {
   char **d;
   if (dolock) emutex_lock (&ml_tb_current_mutex);
   if ((d = inset_pattern ((const void **)(task & einit_module_enable ? current.enable : current.disable), before[i], SET_TYPE_STRING)) && (d = strsetdel (d, service))) {
    uint32_t y = 0;
    if (dolock) emutex_unlock (&ml_tb_current_mutex);

    for (; d[y]; y++) {
     struct group_data *gd = mod_group_get_data(d[y]);

     if (!gd || !gd->members || !inset ((const void **)gd->members, (void *)service, SET_TYPE_STRING)) {
      mod_defer_until (d[y], service);

      xbefore = (char **)setadd ((void **)xbefore, (void *)d[y], SET_TYPE_STRING);
     }
    }

    free (d);
   } else {
    if (dolock) emutex_unlock (&ml_tb_current_mutex);
   }
  }
 }
 if (after) {
  uint32_t i = 0;

  for (; after[i]; i++) {
   char **d;
   if (dolock) emutex_lock (&ml_tb_current_mutex);
   if ((d = inset_pattern ((const void **)(task & einit_module_enable ? current.enable : current.disable), after[i], SET_TYPE_STRING)) && (d = strsetdel (d, service))) {
    uint32_t y = 0;
    if (dolock) emutex_unlock (&ml_tb_current_mutex);

    for (; d[y]; y++) {
     struct group_data *gd = mod_group_get_data(d[y]);

     if ((!xbefore || !inset ((const void **)xbefore, (void *)d[y], SET_TYPE_STRING)) &&
           (!gd || !gd->members || !inset ((const void **)gd->members, (void *)service, SET_TYPE_STRING))) {
      mod_defer_until (service, d[y]);

      hd = 1;
     }
    }

    mod_defer_notice (lm, d);

    free (d);
   } else {
    if (dolock) emutex_unlock (&ml_tb_current_mutex);
   }
  }
 }

 return hd;
}

void mod_examine (char *service) {
 if (mod_workthreads_inc(service)) return;

 if (mod_isbroken (service)) {
  notice (2, "service %s marked as being broken", service);

  mod_workthreads_dec(service);

/* make sure this is not still queued for something */
  int task = mod_gettask (service);

  if (task) {
   emutex_lock (&ml_tb_current_mutex);
   if (task & einit_module_enable) {
    current.enable = strsetdel (current.enable, service);
   } else if (task & einit_module_disable) {
    current.disable = strsetdel (current.disable, service);
   }
   emutex_unlock (&ml_tb_current_mutex);
  }

  mod_post_examine(service);

  return;
 } else if (mod_isdeferred (service)) {
  mod_pre_examine(service);
#ifdef DEBUG
  notice (2, "service %s still marked as deferred", service);
#endif

  if (mod_workthreads_dec(service)) return;

  return;
 } else if (mod_examine_group (service)) {
#ifdef DEBUG
  notice (2, "service %s: group examination complete", service);
#endif

  if (mod_workthreads_dec(service)) return;

  return;
 } else {
  int task = mod_gettask (service);

  if (!task ||
      ((task & einit_module_enable) && mod_isprovided (service)) ||
      ((task & einit_module_disable) && !mod_isprovided (service))) {
   mod_post_examine (service);

   if (mod_workthreads_dec(service)) return;

   return;
  }

#ifdef DEBUG
  eprintf (stderr, " ** examining service %s (%s).\n", service,
                   task & einit_module_enable ? "enable" : "disable");
#endif

  emutex_lock (&ml_service_list_mutex);
  struct stree *v = streefind (module_logics_service_list, service, tree_find_first);
  struct lmodule **lm = v ? v->value : NULL;
  emutex_unlock (&ml_service_list_mutex);

  if (lm && lm[0]) {
   pthread_t th;
   char hd;
   hd = mod_reorder (lm[0], task, service, 1);

   if (hd) {
    if (mod_workthreads_dec(service)) return;

    return;
    }
   if (task & einit_module_enable) {
    if (ethread_create (&th, &thread_attribute_detached, (void *(*)(void *))mod_apply_enable, v)) {
     mod_apply_enable(v);
    }
   } else {
    if (ethread_create (&th, &thread_attribute_detached, (void *(*)(void *))mod_apply_disable, v)) {
     mod_apply_disable(v);
    }
   }
  } else {
   notice (2, "cannot resolve service %s.", service);

   mod_mark (service, MARK_UNRESOLVED);

   mod_workthreads_dec(service);
  }

  return;
 }
 mod_workthreads_dec(service);
}

void workthread_examine (char *service) {
// if (mod_workthreads_inc(service)) return;

 mod_examine (service);
 free (service);

// mod_workthreads_dec(service);
}

void mod_spawn_batch(char **batch, int task) {
 char **dospawn = NULL;
 uint32_t i, deferred, broken, groupc;

 retry:
 
 deferred = 0; broken = 0; groupc = 0;

 if (dospawn) {
  free(dospawn);
  dospawn = NULL;
 }

 if (!batch) return;

 for (i = 0; batch[i]; i++) {
  if (mod_isbroken(batch[i])) {
   broken++;
//   eprintf (stderr, " !! %s\n", batch[i]);
  } else if (mod_isdeferred(batch[i]) || mod_reorder(NULL, task, batch[i], 0)) {
   deferred++;
//   eprintf (stderr, " !! %s\n", batch[i]);

   groupc += mod_group_get_data (batch[i]) ? 1 : 0;
  } else if ((task == einit_module_enable) && mod_isprovided (batch[i])) {
   current.enable = strsetdel (current.enable, batch[i]);
   batch = current.enable;
   goto retry;
  } else if ((task == einit_module_disable) && !mod_isprovided (batch[i])) {
   current.disable = strsetdel (current.disable, batch[i]);
   batch = current.disable;
   goto retry;
  } else {
   dospawn = (char **)setadd ((void **)dospawn, batch[i], SET_TYPE_STRING);

   groupc += mod_group_get_data (batch[i]) ? 1 : 0;
  }
 }

#ifdef DEBUG
 char *alist = set2str (' ', batch);

 eprintf (stderr, "i=%i (%s), broken=%i, deferred=%i, groups=%i\n", i, alist ? alist : "none", broken, deferred, groupc);

 if (alist) free (alist);
#endif

 if (i == (broken + deferred + groupc)) {
/* foo: circular dependencies? kill the whole chain and hope for something good... */
  emutex_lock(&ml_chain_examine);
  if (module_logics_chain_examine) {
   streefree (module_logics_chain_examine);
   module_logics_chain_examine = NULL;
  }
  if (module_logics_chain_examine_reverse) {
   streefree (module_logics_chain_examine_reverse);
   module_logics_chain_examine_reverse = NULL;
  }
  emutex_unlock(&ml_chain_examine);
 }

 if (dospawn) {
  for (i = 0; dospawn[i]; i++) {
   char *sc = estrdup (dospawn[i]);
   pthread_t th;

#ifdef DEBUG
   eprintf (stderr, " XX spawning thread for %s\n", dospawn[i]);
#endif

   if (ethread_create (&th, &thread_attribute_detached, (void *(*)(void *))workthread_examine, sc)) {
    emutex_unlock (&ml_tb_current_mutex);
    workthread_examine (sc);
    emutex_lock (&ml_tb_current_mutex);
//    notice (1, "couldn't create thread!!");
   }
  }

  free (dospawn);
  dospawn = NULL;
 }

// ignorereorderfor =
}

void mod_spawn_workthreads () {
 emutex_lock(&ml_chain_examine);
 if (module_logics_chain_examine) {
  streefree (module_logics_chain_examine);
  module_logics_chain_examine = NULL;
 }
 if (module_logics_chain_examine_reverse) {
  streefree (module_logics_chain_examine_reverse);
  module_logics_chain_examine_reverse = NULL;
 }
 emutex_unlock(&ml_chain_examine);

 emutex_lock (&ml_tb_current_mutex);
 if (current.enable) {
  mod_spawn_batch(current.enable, einit_module_enable);
 }

 if (current.disable) {
  mod_spawn_batch(current.disable, einit_module_disable);
 }
 emutex_unlock (&ml_tb_current_mutex);

#if 0
#ifdef _POSIX_PRIORITY_SCHEDULING
 sched_yield();
#endif

 pthread_cond_broadcast (&ml_cond_service_update);
#endif
}

void mod_commit_and_wait (char **en, char **dis) {
 int remainder;
 uint32_t iterations = 0;

#if 0
#ifdef _POSIX_PRIORITY_SCHEDULING
 sched_yield();
#endif

 pthread_cond_broadcast (&ml_cond_service_update);
#endif

#ifdef DEBUG
 eputs ("flattening...\n", stderr);
 fflush(stderr);
#endif
 mod_flatten_current_tb();

#ifdef DEBUG
 eputs ("flat as a pancake now...\n", stderr);
 fflush(stderr);
#endif

 mod_commits_inc();

 while (1) {
  remainder = 0;
  iterations++;

#ifdef DEBUG
  char **stillneed = NULL;
#endif

  if (en) {
   uint32_t i = 0;

   for (; en[i]; i++) {
    if (!mod_isbroken (en[i]) && !mod_haschanged(en[i]) && !mod_isprovided(en[i])) {
#ifdef DEBUG
     eprintf (stderr, "not yet provided: %s\n", en[i]);
     stillneed = setadd (stillneed, en[i], SET_TYPE_STRING);
#endif

     remainder++;

     emutex_lock (&ml_tb_current_mutex);
     if (!inset ((const void **)current.enable, en[i], SET_TYPE_STRING)) {
      emutex_unlock (&ml_tb_current_mutex);

#ifdef DEBUG
      notice (2, "something must've gone wrong with service %s...", en[i]);
#endif

      remainder--;
     } else
      emutex_unlock (&ml_tb_current_mutex);
    }
   }
  }

  if (dis) {
   uint32_t i = 0;

   for (; dis[i]; i++) {
    if (!mod_isbroken (dis[i]) && !mod_haschanged(dis[i]) && mod_isprovided(dis[i])) {
#ifdef DEBUG
     eprintf (stderr, "still provided: %s\n", dis[i]);
     stillneed = setadd (stillneed, dis[i], SET_TYPE_STRING);
#endif

     remainder++;

     emutex_lock (&ml_tb_current_mutex);
     if (!inset ((const void **)current.disable, dis[i], SET_TYPE_STRING)) {
      current.disable = (char **)setadd ((void **)current.disable, dis[i], SET_TYPE_STRING);
      emutex_unlock (&ml_tb_current_mutex);

#ifdef DEBUG
      notice (2, "something must've gone wrong with service %s...", dis[i]);
#endif

      remainder--;
     } else
      emutex_unlock (&ml_tb_current_mutex);
    }
   }
  }

  if (remainder <= 0) {
   mod_commits_dec();

#ifdef DEBUG
   notice (4, "finished: enable=%s; disable=%s\n", en ? set2str (' ', en) : "none", dis ? set2str (' ', dis) : "none");
   fflush (stderr);
#endif

//   pthread_mutex_destroy (&ml_service_update_mutex);
   return;
  }

#ifdef DEBUG
  if (!stillneed) {
   notice (4, "still need %i services\n", remainder);
  } else {
   notice (4, "still need %i services (%s)\n", remainder, set2str (' ', stillneed));
  }
  fflush (stderr);

  emutex_lock (&ml_workthreads_mutex);

  notice (4, "workthreads: %i (%s)\n", ml_workthreads, set2str (' ', lm_workthreads_list));
  emutex_unlock (&ml_workthreads_mutex);
#endif

  if (iterations >= MAX_ITERATIONS) {
   notice (1, "plan aborted (too many iterations: %i).\n", iterations);

#ifdef DEBUG
   notice (4, "aborted: enable=%s; disable=%s\n", en ? set2str (' ', en) : "none", dis ? set2str (' ', dis) : "none");
   fflush (stderr);
#endif

   mod_commits_dec();
//   pthread_mutex_destroy (&ml_service_update_mutex);
   return;
  }

  emutex_lock (&ml_service_update_mutex);
  int e;
#if defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0)
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts))
   bitch (bitch_stdio, errno, "gettime failed!");

  ts.tv_sec += 1; /* max wait before re-evaluate */

  e = pthread_cond_timedwait (&ml_cond_service_update, &ml_service_update_mutex, &ts);
#elif defined(DARWIN)
  struct timespec ts;
  struct timeval tv;

  gettimeofday (&tv, NULL);

  ts.tv_sec = tv.tv_sec + 1; /* max wait before re-evaluate */

  e = pthread_cond_timedwait (&ml_cond_service_update, &ml_service_update_mutex, &ts);
#else
  notice (2, "warning: un-timed lock.");
  e = pthread_cond_wait (&ml_cond_service_update, &ml_service_update_mutex);
#endif
  emutex_unlock (&ml_service_update_mutex);

  if (e
#ifdef ETIMEDOUT
      && (e != ETIMEDOUT)
#endif
     ) {
   bitch (bitch_epthreads, e, "waiting on conditional variable for plan");
  }/* else {
   notice (1, "woke up, checking plan.\n");
  }*/
 };

/* never reached */
 mod_commits_dec();
}

/* end new functions */
