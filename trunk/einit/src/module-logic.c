/*
 *  module-logic.c
 *  einit
 *
 *  Created by Magnus Deininger on 05/09/2006.
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
#include <pthread.h>
#include <einit/module-logic.h>

#ifdef STABLE
struct lmodule *mlist;
int mcount;

char **provided;
char **required;
pthread_mutex_t mlist_mutex;

/* helper functions for mod_plan should go right here */

int mod_plan_sort_by_preference (struct lmodule **cand, char *atom) {
 char *pstring = emalloc ((8 + strlen (atom)) * sizeof (char));
 char **preftab;
 struct cfgnode *node;
 unsigned int tn = 0, cn = 0, ci;
 pstring[0] = 0;
 strcat (pstring, "prefer-");
 strcat (pstring, atom);
 node = cfg_findnode (pstring, 0, NULL);
 if (!node || !node->svalue) return 0;
 free (pstring);
 preftab = str2set (':', node->svalue);

 for (; preftab[tn]; tn++) {
  for (ci = 0; cand[ci]; ci++) {
   if (cand[ci]->module && cand[ci]->module->rid &&
       !strcmp (cand[ci]->module->rid, preftab[tn])) {
    struct lmodule *tmp = cand[cn];
    cand[cn] = cand[ci];
    cand[ci] = tmp;
    cn++;
    break;
   }
  }
 }

 free (preftab);
 return 0;
}

struct uhash *mod_plan2hash (struct mloadplan *plan, struct uhash *hash, int flag) {
 struct uhash *ihash = hash;
 int i;

 if (!plan) return NULL;
 if ((flag != MOD_P2H_PROVIDES_NOBACKUP) && plan->left && plan->left[0]) {
  for (i = 0; plan->left[i]; i++)
   ihash = mod_plan2hash(plan->left[i], ihash, flag);
 }
 if (plan->right && plan->right[0]) {
  for (i = 0; plan->right[i]; i++)
   ihash = mod_plan2hash(plan->right[i], ihash, flag);
 }
 if (plan->orphaned && plan->orphaned[0]) {
  for (i = 0; plan->orphaned[i]; i++)
   ihash = mod_plan2hash(plan->orphaned[i], ihash, flag);
 }
 if (flag == MOD_P2H_LIST) {
//  ihash = hashadd (ihash, "", plan, sizeof (struct mloadplan));
  ihash = hashadd (ihash, "", plan, -1, NULL);
 } else {
   switch (flag) {
    case MOD_P2H_PROVIDES:
    case MOD_P2H_PROVIDES_NOBACKUP:
     if (plan->mod && plan->mod->module && plan->mod->module->rid)
//      ihash = hashadd (ihash, plan->mod->module->rid, plan, sizeof (struct mloadplan));
      ihash = hashadd (ihash, plan->mod->module->rid, plan, -1, NULL);
     if (plan->provides && plan->provides[0]) {
      for (i = 0; plan->provides[i]; i++) {
//       ihash = hashadd (ihash, plan->provides[i], (void *)plan, sizeof (struct mloadplan));
       ihash = hashadd (ihash, plan->provides[i], (void *)plan, -1, NULL);
      }
     }
     break;
    case MOD_P2H_REQUIRES:
     if (plan->requires && plan->requires[0]) {
      for (i = 0; plan->requires[i]; i++) {
//       ihash = hashadd (ihash, plan->requires[i], (void *)plan, sizeof (struct mloadplan));
       ihash = hashadd (ihash, plan->requires[i], (void *)plan, -1, NULL);
      }
     }
     break;
   }
 }

 return ihash;
}

struct mloadplan *mod_plan_restructure (struct mloadplan *plan) {
 struct uhash *hash_prov, *hash_req, *hash_prov_nb, *c, *d;
 struct mloadplan **orphans = NULL;
 struct mloadplan **curpl = NULL;
 unsigned int i, j;
 unsigned char pass = 0, adds;
 if (!plan) return NULL;
 else if (!plan->mod && !plan->right && !plan->left && !plan->orphaned) {
  free (plan);
  return NULL;
 }

 hash_req = mod_plan2hash (plan, NULL, MOD_P2H_REQUIRES);
 hash_prov = mod_plan2hash (plan, NULL, MOD_P2H_PROVIDES);
 hash_prov_nb = mod_plan2hash (plan, NULL, MOD_P2H_PROVIDES_NOBACKUP);

 d = hash_prov;

 while (d) {
  if (d->value) {
   struct mloadplan *v = (struct mloadplan *)d->value;
   if (v->right) free (v->right);
   v->right = NULL;
//   if (v->left) free (v->left);
//   v->left = NULL;
  }
  d = hashnext (d);
 }

 d = hash_prov_nb;
 while (d) {
  if (d->value) {
   struct mloadplan *v = (struct mloadplan *)d->value;

//   printf ("%s: %i\n", v->mod->module->rid, v->task);

//   if (v->mod && v->mod->module) {
    if (((v->task & MOD_ENABLE) && (v->requires)) ||
        ((v->task & MOD_DISABLE) && ((v->provides)))) {
     char **req = NULL;
     if (v->task & MOD_ENABLE) req = v->requires;
     if (v->task & MOD_DISABLE) req = v->provides;

     for (j = 0; req[j]; j++) {
      adds = 0;
      if ((v->task & MOD_ENABLE) && inset ((void **)provided, (void *)req[j], SET_TYPE_STRING)) {
       plan->right = (struct mloadplan **)setadd ((void **)plan->right, (void *)v, -1);
       adds++;
      } else if ((v->task & MOD_DISABLE) && inset ((void **)required, (void *)req[j], SET_TYPE_STRING)) {
       plan->right = (struct mloadplan **)setadd ((void **)plan->right, (void *)v, -1);
       adds++;
      } else {
       if (v->task & MOD_ENABLE) c = hash_prov;
       if (v->task & MOD_DISABLE) c = hash_req;
       while (c && (c = hashfind (c, req[j]))) {
        struct mloadplan *e = c->value;
        adds++;
        e->right = (struct mloadplan **)setadd ((void **)e->right, (void *)v, -1);
        c = c->next;
       }
      }
      if (adds) {
       plan->orphaned = (struct mloadplan **)setdel ((void **)plan->orphaned, (void*)v);
      } else if (v->task & MOD_ENABLE) {
       if (!inset ((void **)plan->unavailable, (void *)req[j], SET_TYPE_STRING) && !inset ((void **)plan->unsatisfied, (void *)req[j], SET_TYPE_STRING))
        plan->unsatisfied = (char **)setadd ((void **)plan->unsatisfied, (void *)req[j], -1);
      } else if (v->task & MOD_DISABLE) {
       plan->right = (struct mloadplan **)setadd ((void **)plan->right, (void *)v, -1);
       plan->orphaned = (struct mloadplan **)setdel ((void **)plan->orphaned, (void*)v);
      }
     }
    } else {
     plan->right = (struct mloadplan **)setadd ((void **)plan->right, (void *)v, -1);
     plan->orphaned = (struct mloadplan **)setdel ((void **)plan->orphaned, (void*)v);
    }
//   }
  }
  d = hashnext (d);
 }

 hashfree (hash_prov_nb);
 hashfree (hash_prov);
 hashfree (hash_req);

 return plan;
}

/* end helper functions */

struct mloadplan *mod_plan (struct mloadplan *plan, char **atoms, unsigned int task, struct cfgnode *mode_notused) {
 struct lmodule *curmod;
 struct mloadplan **nplancand = NULL;
 int si = 0;
 if (!atoms && !(task & MOD_DISABLE_UNSPEC)) return NULL;

 if (!plan) {
  plan = (struct mloadplan *)ecalloc (1, sizeof (struct mloadplan));
  plan->task = task;
 }

 if (task & MOD_DISABLE_UNSPEC) {
  curmod = mlist;
  while (curmod) {
   struct smodule *tmp = curmod->module;
   if (curmod->status & STATUS_ENABLED) {
    if (tmp) {
     if ((tmp->mode & EINIT_MOD_FEEDBACK) && !(task & MOD_DISABLE_UNSPEC_FEEDBACK) ||
         (tmp->rid && inset ((void **)atoms, (void *)tmp->rid, SET_TYPE_STRING)))
      goto skipcurmod;
     if (tmp->provides && atoms) {
      for (si = 0; atoms[si]; si++)
       if (inset ((void **)tmp->provides, (void *)atoms[si], SET_TYPE_STRING))
        goto skipcurmod;
     }
    }
    struct mloadplan *cplan = (struct mloadplan *)ecalloc (1, sizeof (struct mloadplan));
    cplan->task = MOD_DISABLE;
    cplan->mod = curmod;
    cplan->options = MOD_PLAN_IDLE;
    if (curmod->module) {
     if (curmod->module->requires) cplan->requires = (char **)setdup ((void **)curmod->module->requires, -1);
     if (curmod->module->provides) cplan->provides = (char **)setdup ((void **)curmod->module->provides, -1);
    }
    pthread_mutex_init (&cplan->mutex, NULL);
    plan->orphaned = (struct mloadplan **)setadd ((void **)plan->orphaned, (void *)cplan, -1);
   }
   skipcurmod:
   curmod = curmod->next;
  }
 }

 if (atoms) {
  atoms = (char **)setdup ((void **)atoms, -1);
  for (si = 0; atoms[si]; si++) {
   struct lmodule **cand = (struct lmodule **)ecalloc (mcount+1, sizeof (struct lmodule *));
   struct mloadplan *cplan = NULL;
   struct mloadplan *tcplan = NULL;
   struct mloadplan **planl = NULL;
   unsigned int cc = 0, npcc;
   curmod = mlist;
   char **groupatoms = NULL;
   uint32_t groupoptions = 0;
   struct cfgnode *gnode = NULL;

   while (gnode = cfg_findnode (atoms[si], 0, gnode)) {
    uint32_t gni = 0;
    if (gnode->arbattrs)
     for (; gnode->arbattrs[gni]; gni+=2) {
      if (!strcmp (gnode->arbattrs[gni], "group")) {
       char **gatoms = str2set (':', gnode->arbattrs[gni+1]);
       int32_t gatomi = 0;
       groupoptions |= MOD_PLAN_GROUP;
       if (gatoms) {
        for (; gatoms[gatomi]; gatomi++)
         groupatoms = (char **)setadd ((void **)groupatoms, (void *) gatoms[gatomi], -1);
//        free (gatoms);
       }
      } else if (!strcmp (gnode->arbattrs[gni], "seq")) {
       if (!strcmp (gnode->arbattrs[gni+1], "most")) {
        groupoptions |= MOD_PLAN_GROUP_SEQ_MOST;
       }
       else if (!strcmp (gnode->arbattrs[gni+1], "any"))
        groupoptions |= MOD_PLAN_GROUP_SEQ_ANY;
       else if (!strcmp (gnode->arbattrs[gni+1], "any-iop"))
        groupoptions |= MOD_PLAN_GROUP_SEQ_ANY_IOP;
       else if (!strcmp (gnode->arbattrs[gni+1], "all"))
        groupoptions |= MOD_PLAN_GROUP_SEQ_ALL;
      }
     }
   }

   while (curmod) {
    struct smodule *tmp = curmod->module;
    if (tmp) {
     uint32_t gai;
     if ((tmp->rid && !strcmp (tmp->rid, atoms[si])) ||
        (tmp->provides && inset ((void **)tmp->provides, (void *)atoms[si], SET_TYPE_STRING))) {
      addmodtocandidates:
      cand[cc] = curmod;
      cc++;
      curmod = curmod->next;
      continue;
     }
     if (groupatoms) {
      for (gai = 0; groupatoms[gai]; gai++) {
       if (tmp->provides && inset ((void **)tmp->provides, (void *)groupatoms[gai], SET_TYPE_STRING)) {
        goto addmodtocandidates;
       }
      }
     }
    }
    curmod = curmod->next;
   }
   free (groupatoms);
//   printf ("looking for \"%s\": %i candidate(s)\n", atoms[si], cc);

   if (cc) {
    if (mod_plan_sort_by_preference (cand, atoms[si])) {
     return NULL;
    }
    cplan = (struct mloadplan *)ecalloc (1, sizeof (struct mloadplan));
    cplan->task = task;
    pthread_mutex_init (&cplan->mutex, NULL);
    if (cc == 1) {
     cplan->mod = cand[0];
     cplan->options = MOD_PLAN_IDLE;
     if (groupatoms) {
      cplan->provides = (char **)setadd ((void **)cplan->provides, (void **)atoms[si], -1);
      if (cand[0]->module) {
       if (cand[0]->module->requires) {
        int ir = 0;
        for (; cand[0]->module->requires[ir]; ir++) {
         cplan->requires = (char **)setadd ((void **)cplan->requires, (void *)cand[0]->module->requires[ir], -1);
        }
       }
       if (cand[0]->module->provides) {
        int ir = 0;
        for (; cand[0]->module->provides[ir]; ir++) {
         cplan->provides = (char **)setadd ((void **)cplan->provides, (void *)cand[0]->module->provides[ir], -1);
        }
       }
      }
     } else {
      if (cand[0]->module) {
       if (cand[0]->module->requires) cplan->requires = (char **)setdup ((void **)cand[0]->module->requires, -1);
       if (cand[0]->module->provides) cplan->provides = (char **)setdup ((void **)cand[0]->module->provides, -1);
      }
     }
    } else if (cc > 1) {
     unsigned int icc = 0;
     planl = (struct mloadplan **)ecalloc (cc+1, sizeof (struct mloadplan *));
     cplan->group = planl;
     if (groupoptions)
      cplan->options |= groupoptions;
     else
      cplan->options = MOD_PLAN_IDLE + MOD_PLAN_GROUP + MOD_PLAN_GROUP_SEQ_ANY_IOP;

     cplan->provides = (char **)setadd ((void **)cplan->provides, (void **)atoms[si], -1);
     for (; icc < cc; icc++) {
      tcplan = (struct mloadplan *)ecalloc (1, sizeof (struct mloadplan));
      tcplan->task = task;
      tcplan->mod = cand[icc];
      tcplan->options = MOD_PLAN_IDLE;
      pthread_mutex_init (&tcplan->mutex, NULL);
      if (cand[icc]->module) {
       if (cand[icc]->module->requires) {
        int ir = 0;
        for (; cand[icc]->module->requires[ir]; ir++) {
         cplan->requires = (char **)setadd ((void **)cplan->requires, (void *)cand[icc]->module->requires[ir], -1);
        }
       }
       if (cand[icc]->module->provides) {
        int ir = 0;
        for (; cand[icc]->module->provides[ir]; ir++) {
         cplan->provides = (char **)setadd ((void **)cplan->provides, (void *)cand[icc]->module->provides[ir], -1);
        }
       }
       cplan->provides = (char **)setadd ((void **)cplan->provides, (void **)cand[icc]->module->rid, -1);
      }
      cplan->group[icc] = tcplan;
     }
    }
    nplancand = (struct mloadplan **)setadd ((void **)nplancand, (void *)cplan, -1);

    if (plan && plan->unsatisfied) {
     plan->unsatisfied = strsetdel (plan->unsatisfied, atoms[si]);
    }
   } else {
    if (plan && plan->unsatisfied && inset ((void **)plan->unsatisfied, (void *)atoms [si], SET_TYPE_STRING)) {
     char *tmpa = estrdup (atoms[si]);
     printf ("can't satisfy atom: %s\n", atoms[si]);
     plan->unsatisfied = strsetdel (plan->unsatisfied, atoms[si]);
     plan->unavailable = (char **)setadd ((void **)plan->unavailable, (void *)tmpa, -1);
     return plan;
    }
   }

   free (cand);
  }
  free (atoms);
 }

 plan->orphaned = (struct mloadplan **)setcombine ((void **)plan->orphaned, (void **)nplancand, -1);

 plan = mod_plan_restructure(plan);
 if (plan && plan->unsatisfied && plan->unsatisfied[0])
  mod_plan (plan, plan->unsatisfied, task, NULL);

 return plan;
}

unsigned int mod_plan_commit (struct mloadplan *plan) {
 int32_t i, status;
 pthread_t **childthreads = NULL;
 if (!plan) return STATUS_FAIL;
 pthread_mutex_lock(&plan->mutex);
 if (plan->options & MOD_PLAN_OK) return STATUS_OK;
 if (plan->options & MOD_PLAN_FAIL) return STATUS_FAIL;
 if (!plan->mod) {
//  puts ("starting group");
  if  (!(plan->options & MOD_PLAN_GROUP)) status = STATUS_OK;
  else if (!plan->group) status = STATUS_FAIL;
  else {
   if (plan->options & MOD_PLAN_GROUP_SEQ_MOST) {
    plan->position = 0;
//    puts ("assuming OK unless something happens");
    status = STATUS_OK;
   } else
    status = STATUS_FAIL;
   for (; plan->group[plan->position]; plan->position++) {
    uint32_t retval;
    retval = mod_plan_commit (plan->group[plan->position]);
    if (retval == STATUS_ENABLED) retval = STATUS_OK;
    if (retval == STATUS_DISABLED) retval = STATUS_OK;
    if (plan->options & MOD_PLAN_GROUP_SEQ_ALL) {
     switch (retval) {
      case STATUS_FAIL_REQ:
       status = STATUS_FAIL_REQ;
       goto endofmodaction;
       break;
      case STATUS_FAIL:
       status = STATUS_FAIL;
       goto endofmodaction;
       break;
      case STATUS_IDLE:
      case STATUS_OK:
       status = STATUS_OK;
       break;
     }
    } else if (plan->options & MOD_PLAN_GROUP_SEQ_ANY_IOP) {
     switch (retval) {
      case STATUS_FAIL_REQ:
       status = STATUS_FAIL_REQ;
       goto endofmodaction;
       break;
      case STATUS_FAIL:
       break;
      case STATUS_IDLE:
      case STATUS_OK:
       status = STATUS_OK;
       goto endofmodaction;
       break;
     }
    } else if (plan->options & MOD_PLAN_GROUP_SEQ_ANY) {
     switch (retval) {
      case STATUS_FAIL_REQ:
      case STATUS_FAIL:
       break;
      case STATUS_IDLE:
      case STATUS_OK:
       status = STATUS_OK;
       goto endofmodaction;
       break;
     }
    } else if (plan->options & MOD_PLAN_GROUP_SEQ_MOST) {
     switch (retval) {
      case STATUS_FAIL_REQ:
//       puts ("status set to FAIL_REQ");
       status = STATUS_FAIL_REQ;
       break;
      case STATUS_FAIL:
      case STATUS_OK:
      case STATUS_IDLE:
//       puts ("status unchanged");
       break;
     }
    }
   }
  }
 } else {
  status = mod(plan->task, plan->mod);
  if ((status == STATUS_ENABLED) && (plan->task & MOD_ENABLE)) status = STATUS_OK;
  else if ((status == STATUS_DISABLED) && (plan->task & MOD_DISABLE)) status = STATUS_OK;
 }
 endofmodaction:

 if (status & STATUS_OK) {
//  if (plan->group && (plan->options & MOD_PLAN_GROUP_SEQ_MOST)) {
//   puts ("group/most OK");
/* this will need to be reworked a little... no way to figure out when the group is not being provided anymore */
  if (plan->provides && plan->provides [0] && (plan->task & MOD_ENABLE)) {
   provided = (char **)setadd ((void **)provided, (void *)plan->provides[0], -1);
   service_usage_query (SERVICE_INJECT_PROVIDER, plan->mod, plan->provides[0]);
  }
//  }
  if (plan->right)
   for (i = 0; plan->right[i]; i++) {
    pthread_t *th = ecalloc (1, sizeof (pthread_t));
    if (!pthread_create (th, NULL, (void * (*)(void *))mod_plan_commit, (void*)plan->right[i])) {
     childthreads = (pthread_t **)setadd ((void **)childthreads, (void *)th, -1);
    }
//    mod_plan_commit (plan->right[i]);
   }
 } else if (plan->left) {
  for (status = 0; plan->left[i]; i++) {
   pthread_t *th = ecalloc (1, sizeof (pthread_t));
   if (!pthread_create (th, NULL, (void * (*)(void *))mod_plan_commit, (void*)plan->left[i])) {
    childthreads = (pthread_t **)setadd ((void **)childthreads, (void *)th, -1);
   }
  }
 }

 if (childthreads) {
  for (i = 0; childthreads[i]; i++) {
   int32_t returnvalue;
   pthread_join (*(childthreads[i]), (void**) &returnvalue);
/*   switch (returnvalue) {
    case STATUS_OK:
     puts ("success."); break;
    case STATUS_FAIL_REQ:
     puts ("can't load yet."); break;
   }*/
   free (childthreads[i]);
  }
  free (childthreads);
 }

 switch (status) {
  case STATUS_OK:
   plan->options &= MOD_PLAN_OK;
   plan->options |= MOD_PLAN_IDLE;
   plan->options ^= MOD_PLAN_IDLE;
   break;
  case STATUS_FAIL:
   plan->options &= MOD_PLAN_FAIL;
   plan->options |= MOD_PLAN_IDLE;
   plan->options ^= MOD_PLAN_IDLE;
   break;
 }

 pthread_mutex_unlock(&plan->mutex);
 return status;
// return STATUS_OK;
 panic:
  pthread_mutex_unlock(&plan->mutex);
  bitch (BTCH_ERRNO);
  return STATUS_FAIL;
}

/* free all elements in the plan */
int mod_plan_free (struct mloadplan *plan) {
 struct uhash *hash_list;
 struct uhash *d;

 hash_list = mod_plan2hash (plan, NULL, MOD_P2H_LIST);

 d = hash_list;
 while (d) {
  if (d->value) {
   struct mloadplan *v = (struct mloadplan *)d->value;
//   if (v->right) free (v->right);
//   if (v->left) free (v->left);
//   if (v->orphaned) free (v->orphaned);
//   free (d->value);
//   d->value = NULL;
  }
  d = hashnext (d);
 }

 hashfree (hash_list);
}

#ifdef DEBUG
/* debugging functions: only available if DEBUG is set (obviously...) */
void mod_plan_ls (struct mloadplan *plan) {
 char *rid = "n/a", *name = "unknown", *action;
 static int recursion;
 unsigned char pass = 0;
 struct mloadplan **cur;
 int i;
 if (!recursion) puts ("committing this plan will...");
 if (!plan) return;
 if (plan->mod) {
  if (plan->mod->module) {
   if (plan->mod->module->rid)
    rid = plan->mod->module->rid;
   if (plan->mod->module->name)
    name = plan->mod->module->name;
  }
 } else if (plan->options & MOD_PLAN_GROUP) {
  if (plan->provides && plan->provides[0])
   rid = plan->provides[0];
  else
   rid = "group";
  if (plan->options & MOD_PLAN_GROUP_SEQ_ANY)
   name = "any element";
  else if (plan->options & MOD_PLAN_GROUP_SEQ_ANY_IOP)
   name = "any element, in order of preference";
  else if (plan->options & MOD_PLAN_GROUP_SEQ_MOST)
   name = "most elements";
  else if (plan->options & MOD_PLAN_GROUP_SEQ_ALL)
   name = "all elements";
 }
 for (i = 0; i < recursion; i++)
  fputs (" ", stdout);
 switch (plan->task) {
  case MOD_ENABLE:
   action = "enable"; break;
  case MOD_DISABLE:
   action = "disable"; break;
  default:
   action = "do something with..."; break;
 }
 printf ("%s %s (%s)\n", action, rid, name);
 while (pass < 4) {
  recursion++;
  switch (pass) {
   case 0:
    if (plan->left && plan->left[0]) {
     for (i = 0; i < recursion; i++)
      fputs (" ", stdout);
     cur = plan->left;
     puts ("on failure {");
     break;
    }
    pass++;
   case 1:
    if (plan->right && plan->right[0]) {
     for (i = 0; i < recursion; i++)
      fputs (" ", stdout);
     cur = plan->right;
     puts ("on success {");
     break;
    }
    pass++;
   case 2:
    if (plan->orphaned && plan->orphaned[0]) {
     for (i = 0; i < recursion; i++)
      fputs (" ", stdout);
	 cur = plan->orphaned;
     puts ("orphans {");
	 break;
    }
	pass++;
   case 3:
    if (plan->group && plan->group[0]) {
     for (i = 0; i < recursion; i++)
      fputs (" ", stdout);
	 cur = plan->group;
     puts ("GROUP {");
	 break;
    }
	pass++;
   default:
    recursion--;
    goto unsat;
  }

  recursion++;
  for (i = 0; cur[i]; i++) {
   if (cur[i] != plan)
    mod_plan_ls (cur[i]);
   else {
    puts ("Circular dependency detected, aborting.");
	recursion-=2;
    return;
   }
  }
  recursion--;
  for (i = 0; i < recursion; i++)
   fputs (" ", stdout);
  puts ("}");

  pass++;
  recursion--;
 }

 unsat:

/* if (plan->provides && plan->provides[0]) {
  for (i = 0; plan->provides[i]; i++) {
   int ic = -1;
   for (; ic < recursion; ic++)
    fputs (" ", stdout);
   printf ("provides: %s\n", plan->provides[i]);
  }
 }


 if (plan->requires && plan->requires[0]) {
  for (i = 0; plan->requires[i]; i++) {
   int ic = -1;
   for (; ic < recursion; ic++)
    fputs (" ", stdout);
   printf ("requires: %s\n", plan->requires[i]);
  }
 }

 if (plan->unsatisfied && plan->unsatisfied[0]) {
  for (i = 0; plan->unsatisfied[i]; i++) {
   int ic = -1;
   for (; ic < recursion; ic++)
    fputs (" ", stdout);
   printf ("unsatisfied dependency: %s\n", plan->unsatisfied[i]);
  }
 }*/

 if (plan->unavailable && plan->unavailable[0]) {
  for (i = 0; plan->unavailable[i]; i++) {
   int ic = -1;
   for (; ic < recursion; ic++)
    fputs (" ", stdout);
   printf ("unavailable dependency: %s\n", plan->unavailable[i]);
  }
 }
}
#endif

/*
   new, experimental, completely instable and not working version of the previous code.
*/

#else

struct lmodule *mlist;

// create a plan for loading a set of atoms
struct mloadplan *mod_plan (struct mloadplan *plan, char **atoms, unsigned int task, struct cfgnode *mode) {
 uint32_t a = 0, b = 0;
 char
  **enable = NULL, **aenable = NULL,
  **disable = NULL, **adisable = NULL,
  **reset = NULL, **areset = NULL;
 struct cfgnode *rmode = mode;
 struct mloadplan_node nnode;

 if (!plan) {
  plan = ecalloc (1, sizeof (struct mloadplan));
  pthread_mutex_init (&plan->mutex, NULL);
 }
 pthread_mutex_lock (&plan->mutex);

 if (mode) {
  enable  = str2set (':', cfg_getstring ("enable/mod", mode)),
  disable = str2set (':', cfg_getstring ("disable/mod", mode)),
  reset   = str2set (':', cfg_getstring ("reset/mod", mode));

  if (mode->base) {
   int y = 0;
   struct cfgnode *cno;
   while (mode->base[y]) {
    cno = cfg_findnode (mode->base[y], EI_NODETYPE_MODE, NULL);
    if (cno) {
     char
      **denable  = str2set (':', cfg_getstring ("enable/mod", cno)),
      **ddisable = str2set (':', cfg_getstring ("disable/mod", cno)),
      **dreset   = str2set (':', cfg_getstring ("reset/mod", cno));

     if (denable) {
      char **t = (char **)setcombine ((void **)denable, (void **)enable, SET_TYPE_STRING);
      free (denable);
      free (enable);
      enable = t;
     }
     if (ddisable) {
      char **t = (char **)setcombine ((void **)ddisable, (void **)disable, SET_TYPE_STRING);
      free (ddisable);
      free (disable);
      enable = t;
     }
     if (dreset) {
      char **t = (char **)setcombine ((void **)dreset, (void **)reset, SET_TYPE_STRING);
      free (dreset);
      free (reset);
      enable = t;
     }
    }
    y++;
   }
  }
 }

 if (disable) {
  char **current = (char **)setdup ((void **)disable, SET_TYPE_STRING);
  puts ("disable:");
  for (a = 0; current[a]; a++) {
   puts (current[a]);
  }
  if (current) free (current);
 }
 if (enable) {
  char **current = (char **)setdup ((void **)enable, SET_TYPE_STRING);
  char **recurse = NULL;
//  puts ("enable:");
  while (current) {
   for (a = 0; current[a]; a++) {
    struct lmodule *cur = mlist;
    struct uhash *ha;
    memset (&nnode, 0, sizeof (struct mloadplan_node));
    pthread_mutex_init (&nnode.mutex, NULL);
    nnode.plan = plan;

    if (ha = hashfind (plan->services, current[a]))
     continue;

    while (cur) {
     struct smodule *mo = cur->module;
     if (mo && inset ((void **)mo->provides, (void *)current[a], SET_TYPE_STRING)) {
      nnode.mod = (struct lmodule **)setadd ((void **)nnode.mod, (void *)cur, SET_NOALLOC);
      recurse = (char **)setcombine ((void **)recurse, (void **)mo->requires, SET_NOALLOC);
     }

     cur = cur->next;
    }

    if (!nnode.mod) {
     char tmp[2048]; tmp[0] = 0;
     strcat (tmp, current[a]);
     strcat (tmp, "/group");

     if (nnode.group = str2set (':', cfg_getstring (tmp, mode)))
      recurse = (char **)setcombine ((void **)recurse, (void **)nnode.group, SET_NOALLOC);
    }

    if (nnode.mod || nnode.group) {
     plan->services = hashadd (plan->services, current[a], (void *)&nnode, sizeof(struct mloadplan_node), nnode.group);
     aenable = (char **)setadd ((void **)aenable, (void *)current[a], SET_TYPE_STRING);
//     puts (current[a]);
    } else {
     plan->unavailable = (char **)setadd ((void **)plan->unavailable, (void *)current[a], SET_TYPE_STRING);
     pthread_mutex_destroy (&nnode.mutex);
    }
   }

   free (current); current = recurse; recurse = NULL;
  }
 }
 if (reset) {
  char **current = (char **)setdup ((void **)reset, SET_TYPE_STRING);
  puts ("reset:");
  for (a = 0; current[a]; a++) {
   puts (current[a]);
  }
  if (current) free (current);
 }

 if (plan->services) {
  struct uhash *ha = plan->services;

  while (ha) {
   ((struct mloadplan_node *)(ha->value))->service = ha->key;

   ha = hashnext (ha);
  }
 }

 plan->enable = enable;
 plan->disable = disable;
 plan->reset = reset;

 pthread_mutex_unlock (&plan->mutex);
 return plan;
}

// the actual loader function
void *mod_plan_commit_recurse_enable (struct mloadplan_node *node) {
 pthread_mutex_lock (&node->mutex);
 pthread_t **subthreads = NULL;
 struct uhash *ha;
 uint32_t i = 0, u = 0;

 if (node->mod) {
  fprintf (stderr, "enabling node 0x%zx\n", node);

  for (i = 0; node->mod[i]; i++) {
   char **services = (node->mod[i]->module) ? node->mod[i]->module->requires : NULL;

   if (services) {
    uint32_t j = 0;
    for (; services[j]; j++)
     if (!service_usage_query(SERVICE_IS_PROVIDED, NULL, services[j]) && (ha = hashfind (node->plan->services, services[j]))) {
      pthread_t th;
      puts (services[j]);
      if (!pthread_create (&th, NULL, (void *(*)(void *))mod_plan_commit_recurse_enable, (void *)ha->value))
       subthreads = (pthread_t **)setadd ((void **)subthreads, (void *)&th, sizeof(pthread_t));
      else {
       notice (2, "warning: subthread creation failed!");
       mod_plan_commit_recurse_enable (ha->value);
      }
     }
   }

   if (subthreads) {
    for (u = 0; subthreads[u]; u++)
     pthread_join (*(subthreads[u]), NULL);

    free (subthreads); subthreads = NULL;
   }

   if ((node->status = mod (MOD_ENABLE, node->mod[i])) & STATUS_ENABLED) break;
  }
 } else if (node->group) {
  for (u = 0; node->group[u]; u++) {
   if (!service_usage_query(SERVICE_IS_PROVIDED, NULL, node->group[u]) && (ha = hashfind (node->plan->services, node->group[u]))) {
    struct mloadplan_node  *cnode = (struct mloadplan_node  *)ha->value;

    mod_plan_commit_recurse_enable (cnode);

    if (cnode->status & STATUS_ENABLED) {
     service_usage_query (SERVICE_INJECT_PROVIDER, cnode->mod[i], node->service);
     node->status |= STATUS_ENABLED;
    }
   }
  }
 }

 pthread_mutex_unlock (&node->mutex);
// pthread_exit (NULL);
 return NULL;
}

// actually do what the plan says
unsigned int mod_plan_commit (struct mloadplan *plan) {
 if (!plan) return;
 pthread_mutex_lock (&plan->mutex);

 pthread_t **subthreads = NULL;
 struct uhash *ha;
 uint32_t u = 0;

 if (plan->enable) {
  for (u = 0; plan->enable[u]; u++) {
   if (!service_usage_query(SERVICE_IS_PROVIDED, NULL, plan->enable[u]) && (ha = hashfind (plan->services, plan->enable[u]))) {
    pthread_t th;
    puts (plan->enable[u]);
    if (!pthread_create (&th, NULL, (void *(*)(void *))mod_plan_commit_recurse_enable, (void *)ha->value))
     subthreads = (pthread_t **)setadd ((void **)subthreads, (void *)&th, sizeof (pthread_t));
    else {
     notice (2, "warning: subthread creation failed!");
     mod_plan_commit_recurse_enable (ha->value);
    }
   }
  }
 }

 if (subthreads) {
  for (u = 0; subthreads[u]; u++)
   pthread_join (*(subthreads[u]), NULL);

  free (subthreads); subthreads = NULL;
 }

 if (plan->unavailable) {
  char tmp[2048] = "WARNING: unavailable services:", tmp2[2048];

  for (; plan->unavailable[u]; u++) {
   strcpy (tmp2, tmp);
   snprintf (tmp, 2048, "%s %s", tmp2, plan->unavailable[u]);
  }

  puts (tmp);
//  notice (2, tmp);
 }

 pthread_mutex_unlock (&plan->mutex);
 return 0;
}

// free all of the resources of the plan
int mod_plan_free (struct mloadplan *plan) {
 pthread_mutex_lock (&plan->mutex);
 if (plan->enable) free (plan->enable);
 if (plan->disable) free (plan->disable);
 if (plan->reset) free (plan->reset);
 if (plan->unavailable) free (plan->unavailable);

 if (plan->services) {
  struct uhash *ha = plan->services;
  struct mloadplan_node *no = NULL;

  while (ha) {
   if (no = (struct mloadplan_node *)ha->value)
    pthread_mutex_destroy (&no->mutex);

   ha = hashnext (ha);
  }
  hashfree (plan->services);
 }

 pthread_mutex_unlock (&plan->mutex);
 pthread_mutex_destroy (&plan->mutex);

 free (plan);
 return 0;
}

#endif
