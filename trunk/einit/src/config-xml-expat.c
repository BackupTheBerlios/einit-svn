/*
 *  config-xml-expat.c
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

#define PATH_MODULES 1
#define FEEDBACK_MODULE 1

struct uhash *hconfiguration = NULL;
struct cfgnode *curmode = NULL;

void cfg_xml_handler_tag_start (void *userData, const XML_Char *name, const XML_Char **atts) {
 int i = 0;
 if (!strcmp (name, "mode")) {
  struct cfgnode *newnode = ecalloc (1, sizeof (struct cfgnode));
  newnode->nodetype = EI_NODETYPE_MODE;
  newnode->arbattrs = (char **)setdup ((void **)atts, SET_TYPE_STRING);
  for (; newnode->arbattrs[i] != NULL; i+=2) {
   if (!strcmp (newnode->arbattrs[i], "id")) {
    newnode->id = estrdup((char *)newnode->arbattrs[i+1]);
   } else if (!strcmp (newnode->arbattrs[i], "base")) {
    newnode->base = str2set (':', (char *)newnode->arbattrs[i+1]);
   }
  }
  if (newnode->id) {
   char *id = newnode->id;
   cfg_addnode (newnode);
   free (newnode);
/* this is admittedly a tad more complicated than necessary, however its the only way to find the last addition to the hash
   with this id */
   curmode = NULL;
   while (curmode = cfg_findnode (id, EI_NODETYPE_MODE, curmode)) {
    newnode = curmode;
   }
   curmode = newnode;
  }
 } else {
  struct cfgnode *newnode = ecalloc (1, sizeof (struct cfgnode));
  newnode->id = estrdup ((char *)name);
  newnode->nodetype = EI_NODETYPE_CONFIG;
  newnode->mode = curmode;
  newnode->arbattrs = (char **)setdup ((void **)atts, SET_TYPE_STRING);
  if (newnode->arbattrs)
   for (; newnode->arbattrs[i] != NULL; i+=2) {
    if (!strcmp (newnode->arbattrs[i], "s"))
     newnode->svalue = (char *)newnode->arbattrs[i+1];
    else if (!strcmp (newnode->arbattrs[i], "i"))
     newnode->value = atoi (newnode->arbattrs[i+1]);
    else if (!strcmp (newnode->arbattrs[i], "bi"))
     newnode->value = strtol (newnode->arbattrs[i+1], (char **)NULL, 2);
    else if (!strcmp (newnode->arbattrs[i], "oi"))
     newnode->value = strtol (newnode->arbattrs[i+1], (char **)NULL, 8);
    else if (!strcmp (newnode->arbattrs[i], "b")) {
     int j = i+1;
     newnode->flag = (!strcmp (newnode->arbattrs[j], "true") ||
                      !strcmp (newnode->arbattrs[j], "enabled") ||
                      !strcmp (newnode->arbattrs[j], "yes"));
    }
   }
  cfg_addnode (newnode);
  free (newnode);
 }
}

void cfg_xml_handler_tag_end (void *userData, const XML_Char *name) {
 if (!strcmp (name, "mode"))
  curmode = NULL;
}

int cfg_load (char *configfile) {
 static char recursion = 0;
 int cfgfd, e, blen, cfgplen;
 char * buf, * data;
 struct uhash *hnode;
 ssize_t rn;
 struct cfgnode *node = NULL, *last = NULL;
 char *confpath = NULL;
 XML_Parser par;
 if (!configfile) return 0;
 cfgfd = open (configfile, O_RDONLY);
 if (cfgfd != -1) {
  buf = emalloc (BUFFERSIZE*sizeof(char));
  blen = 0;
  do {
   buf = erealloc (buf, blen + BUFFERSIZE);
   if (buf == NULL) return bitch(BTCH_ERRNO);
   rn = read (cfgfd, (char *)(buf + blen), BUFFERSIZE);
   blen = blen + rn;
  } while (rn > 0);
  close (cfgfd);
  data = erealloc (buf, blen);
  par = XML_ParserCreate (NULL);
  if (par != NULL) {
   XML_SetElementHandler (par, cfg_xml_handler_tag_start, cfg_xml_handler_tag_end);
   if (XML_Parse (par, data, blen, 1) == XML_STATUS_ERROR) {
    puts ("cfg_load(): XML_Parse() failed:");
    puts (XML_ErrorString (XML_GetErrorCode (par)));
   }
   XML_ParserFree (par);
  }
  free (data);

  if (!recursion) {
   confpath = cfg_getpath ("configuration-path");
   if (!confpath) confpath = "/etc/einit/";
   cfgplen = strlen(confpath) +1;
   rescan_node:
   hnode = hconfiguration;
   while (hnode = hashfind (hnode, "include")) {
    node = (struct cfgnode *)hnode->value;
    if (node->svalue) {
     char *includefile = ecalloc (1, sizeof(char)*(cfgplen+strlen(node->svalue)));
     includefile = strcat (includefile, confpath);
     includefile = strcat (includefile, node->svalue);
     recursion++;
     cfg_load (includefile);
     recursion--;
     free (includefile);
     if (node->id) free (node->id);
     hashdel (hconfiguration, hnode);
     goto rescan_node;
    }
   }
  }
  return hconfiguration != NULL;
 } else {
  return bitch(BTCH_ERRNO);
 }
}

int cfg_free () {
 struct uhash *cur = hconfiguration;
 struct cfgnode *node = NULL;
 while (cur) {
  if (node = (struct cfgnode *)cur->value) {
   if (node->base)
    free (node->base);

   if (node->custom)
    free (node->custom);
   if (node->id)
    free (node->id);
   if (node->path)
    free (node->path);
  }
  cur = hashnext (cur);
 }
 hashfree (hconfiguration);
 hconfiguration = NULL;
 return 1;
}

int cfg_addnode (struct cfgnode *node) {
 if (!node) return;
 hconfiguration = hashadd (hconfiguration, node->id, node, sizeof(struct cfgnode), node->arbattrs);
// hconfiguration = hashadd (hconfiguration, node->id, node, -1);
}

struct cfgnode *cfg_findnode (char *id, unsigned int type, struct cfgnode *base) {
 struct uhash *cur = hconfiguration;
 if (base) {
  while (cur) {
   if (cur->value == base) {
    cur = hashnext (cur);
    break;
   }
   cur = hashnext (cur);
  }
 }
 if (!cur || !id) return NULL;
 while (cur = hashfind (cur, id)) {
  if (cur->value && (!type || !(((struct cfgnode *)cur->value)->nodetype ^ type)))
   return cur->value;
  cur = hashnext (cur);
 }
 return NULL;
}

// get string (by id)
char *cfg_getstring (char *id, struct cfgnode *mode) {
 struct cfgnode *node = NULL;
 char *ret = NULL, **sub;
 uint32_t i;

 if (!id) return NULL;
 mode = mode ? mode : cmode;

 if (strchr (id, '/')) {
  sub = str2set ('/', id);
  while (node = cfg_findnode (sub[0], 0, node)) {
   if (node->arbattrs) {
    if (node->mode == mode) {
     char f = 0;
     for (i = 0; node->arbattrs[i]; i+=2) {
      if (f = (!strcmp(node->arbattrs[i], sub[1]))) {
       ret = node->arbattrs[i+1];
       break;
      }
     }
     if (f) break;
    } else if (!ret && !node->mode) {
     for (i = 0; node->arbattrs[i]; i+=2) {
      if (!strcmp(node->arbattrs[i], sub[1])) {
       ret = node->arbattrs[i+1];
       break;
      }
     }
    }
   }
  }
  free (sub);
 } else
  while (node = cfg_findnode (id, 0, node)) {
   if (node->svalue) {
    if (node->mode == mode) {
     ret = node->svalue;
     break;
    } else if (!ret && !node->mode)
     ret = node->svalue;
   }
  }

 return ret;
}

// get node (by id)
struct cfgnode *cfg_getnode (char *id, struct cfgnode *mode) {
 struct cfgnode *node = NULL;
 struct cfgnode *ret = NULL;

 if (!id) return NULL;
 mode = mode ? mode : cmode;

 while (node = cfg_findnode (id, 0, node)) {
  if (node->mode == mode) {
   ret = node;
   break;
  } else if (!ret && !node->mode)
   ret = node;
 }

 return ret;
}

/* those i-could've-sworn-there-were-library-functions-for-that functions */
char *cfg_getpath (char *id) {
 int mplen;
 struct cfgnode *svpath = cfg_findnode (id, 0, NULL);
 if (!svpath || !svpath->svalue) return NULL;
 mplen = strlen (svpath->svalue) +1;
 if (svpath->svalue[mplen-2] != '/') {
  if (svpath->path) return svpath->path;
  char *tmpsvpath = (char *)emalloc (mplen+1);
  tmpsvpath[0] = 0;

  strcat (tmpsvpath, svpath->svalue);
  tmpsvpath[mplen-1] = '/';
  tmpsvpath[mplen] = 0;
//  svpath->svalue = tmpsvpath;
  svpath->path = tmpsvpath;
  return tmpsvpath;
 }
 return svpath->svalue;
}
