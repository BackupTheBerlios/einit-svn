/*
 *  event.h
 *  eINIT
 *
 *  Created by Magnus Deininger on 25/06/2006.
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

#ifndef _EINIT_EVENT_H
#define _EINIT_EVENT_H

#include <inttypes.h>

#define EINIT_EVENT_FLAG_BROADCAST	0x0001
#define EINIT_EVENT_TYPE_IPC		0x0001
#define EINIT_EVENT_TYPE_NEED_MODULE	0x0002
#define EINIT_EVENT_TYPE_MOUNT_UPDATE	0x0004
#define EINIT_EVENT_TYPE_CUSTOM		0xFFFF

struct einit_event {
 uint16_t type;
 char *type_custom;
 void **set;
 char *string;
 int32_t integer;
 unsigned char flag;
 void *para;
};

struct event_function {
 uint16_t type;
 void (*handler)(struct einit_event *);
 struct event_function *next;
};

struct function_list {
 char *name;
 uint32_t version;
 void *function;
 struct function_list *next;
};

struct event_function *event_functions;

void *event_emit (struct einit_event *, uint16_t);
void event_listen (uint16_t, void (*)(struct einit_event *));

void function_register (char *, uint32_t, void *);
void function_unregister (char *, uint32_t, void *);
void **function_find (char *, uint32_t, char **);

#define event_emit_flag(a, b) {\
	struct einit_event *c = ecalloc (1, sizeof(struct einit_event));\
	c->type = a;\
	c->flag = b;\
	event_emit (c, EINIT_EVENT_FLAG_BROADCAST);\
	//free (c);\
	}

#endif
