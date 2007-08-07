/*
 *  event.h
 *  eINIT
 *
 *  Created by Magnus Deininger on 25/06/2006.
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

#ifdef __cplusplus
extern "C" {
#endif

#ifndef EINIT_EVENT_H
#define EINIT_EVENT_H

#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#include <einit/tree.h>

#define EVENT_SUBSYSTEM_MASK           0xfffff000
#define EVENT_CODE_MASK                0x00000fff

enum einit_event_emit_flags {
 einit_event_flag_broadcast    = 0x1,
/*!< this should always be specified, although just now it's being ignored */
 einit_event_flag_spawn_thread = 0x2,
/*!< use this to tell einit that you don't wish/need to wait for this to return */
 einit_event_flag_duplicate    = 0x4
/*!< duplicate event data block. important with *spawn_thread */
};

enum einit_event_subsystems {
 einit_event_subsystem_core     = 0x00001000,
 einit_event_subsystem_ipc      = 0x00002000,
/*!< incoming IPC request */
 einit_event_subsystem_mount    = 0x00003000,
/*!< update mount status */
 einit_event_subsystem_feedback = 0x00004000,
 einit_event_subsystem_power    = 0x00005000,
/*!< notify others that the power is failing, has been restored or similar */
 einit_event_subsystem_timer    = 0x00006000,
/*!< set/receive timer ticks */
 einit_event_subsystem_network  = 0x00007000,
 einit_event_subsystem_process  = 0x00008000,

 einit_event_subsystem_any      = 0xffffe000,
/*!< match any subsystem... mostly intended to be used for rebroadcasting, e.g. via D-Bus */
 einit_event_subsystem_custom   = 0xfffff000
/*!< custom events; not yet implemented */
};

enum einit_event_code {
/* einit_event_subsystem_core: */
 einit_core_panic                   = einit_event_subsystem_core     | 0x001,
/*!< put everyone in the cast range into a state of panic/calm everyone down; status contains a reason */
 einit_core_module_update           = einit_event_subsystem_core     | 0x002,
/*!< Module status changing; use the task and status fields to find out what happened */
 einit_core_service_update          = einit_event_subsystem_core     | 0x003,
/*!< Service availability changing; use the task and status fields to find out what happened */
 einit_core_configuration_update    = einit_event_subsystem_core     | 0x004,
/*!< notification of configuration update */
 einit_core_plan_update             = einit_event_subsystem_core     | 0x005,
/*!< Plan status update */
 einit_core_module_list_update      = einit_event_subsystem_core     | 0x006,
/*!< notification of module-list updates */
 einit_core_module_list_update_complete
                                    = einit_event_subsystem_core     | 0x007,

 einit_core_update_configuration    = einit_event_subsystem_core     | 0x101,
/*!< update the configuration */
 einit_core_change_service_status   = einit_event_subsystem_core     | 0x102,
/*!< change status of a service */
 einit_core_switch_mode             = einit_event_subsystem_core     | 0x103,
/*!< switch to a different mode */
 einit_core_update_modules          = einit_event_subsystem_core     | 0x104,
/*!< update the modules */
 einit_core_update_module           = einit_event_subsystem_core     | 0x105,
/*!< update this module (in ->para) */

 einit_core_mode_switching          = einit_event_subsystem_core     | 0x201,
 einit_core_mode_switch_done        = einit_event_subsystem_core     | 0x202,

 einit_core_recover                 = einit_event_subsystem_core     | 0xffe,
 einit_core_main_loop_reached       = einit_event_subsystem_core     | 0xfff,

/* einit_event_subsystem_mount: */
 einit_mount_do_update              = einit_event_subsystem_mount    | 0x001,
 einit_mount_node_mounted           = einit_event_subsystem_mount    | 0x011,
 einit_mount_node_unmounted         = einit_event_subsystem_mount    | 0x012,
 einit_mount_new_mount_level        = einit_event_subsystem_mount    | 0x021,

/* einit_event_subsystem_feedback: */
 einit_feedback_module_status       = einit_event_subsystem_feedback | 0x001,
/*!< the para field specifies a module that caused the feedback */
 einit_feedback_plan_status         = einit_event_subsystem_feedback | 0x002,
 einit_feedback_notice              = einit_event_subsystem_feedback | 0x003,
 einit_feedback_register_fd         = einit_event_subsystem_feedback | 0x011,
 einit_feedback_unregister_fd       = einit_event_subsystem_feedback | 0x012,

 einit_feedback_broken_services     = einit_event_subsystem_feedback | 0x021,
 einit_feedback_unresolved_services = einit_event_subsystem_feedback | 0x022,

/* einit_event_subsystem_power: */
 einit_power_down_scheduled         = einit_event_subsystem_power    | 0x001,
/*!< shutdown scheduled */
 einit_power_down_imminent          = einit_event_subsystem_power    | 0x002,
/*!< shutdown going to happen after this event */
 einit_power_reset_scheduled        = einit_event_subsystem_power    | 0x011,
/*!< reboot scheduled */
 einit_power_reset_imminent         = einit_event_subsystem_power    | 0x012,
/*!< reboot going to happen after this event */

 einit_power_failing                = einit_event_subsystem_power    | 0x021,
/*!< power is failing */
 einit_power_failure_imminent       = einit_event_subsystem_power    | 0x022,
/*!< power is failing NOW */
 einit_power_restored               = einit_event_subsystem_power    | 0x023,
/*!< power was restored */

 einit_timer_tick                   = einit_event_subsystem_timer    | 0x001,
/*!< tick.tick.tick. */
 einit_timer_set                    = einit_event_subsystem_timer    | 0x002,
 einit_timer_cancel                 = einit_event_subsystem_timer    | 0x003,

/* einit_event_subsystem_network: */
 einit_network_do_update            = einit_event_subsystem_network  | 0x001,

/* einit_event_subsystem_process: */
 einit_process_died                 = einit_event_subsystem_process  | 0x001
};

enum einit_ipc_options {
 einit_ipc_output_xml    = 0x0001,
 einit_ipc_output_ansi   = 0x0004,
 einit_ipc_only_relevant = 0x0100,
 einit_ipc_help          = 0x0002,
 einit_ipc_detach        = 0x0010,
 einit_ipc_implemented   = 0x1000
};

#define evstaticinit(ttype) { ttype, 0, { { NULL, NULL, 0, 0, 0, 0, NULL } }, 0, 0, { NULL }, NULL, PTHREAD_MUTEX_INITIALIZER }
#define evstaticdestroy(ev) { pthread_mutex_destroy (&(ev.mutex)); }

struct einit_event {
 enum einit_event_code type;       /*!< the event or subsystem to watch */
 enum einit_event_code chain_type; /*!< the event to be called right after this one */

 union {
/*! these struct elements are for use with non-IPC events */
  struct {
   void **set;                   /*!< a set that should make sense in combination with the event type */
   char *string;                 /*!< a string */
   int32_t integer,              /*!< generic integer */
           status,               /*!< generic integer */
           task;                 /*!< generic integer */
   unsigned char flag;           /*!< flags */

   char **stringset;             /*!< a (string-)set that should make sense in combination with the event type */
  };

/*! these struct elements are for use with IPC events */
  struct {
   char **argv;
   char *command;
   enum einit_ipc_options ipc_options;
   int argc,
       ipc_return;
   char implemented;
  };
 };

 uint32_t seqid;
 time_t timestamp;

/*! additional parameters */
 union {
  struct cfgnode *node;
  struct lmodule *module;
  void *para;
  void *file;
 };

 FILE *output;

 pthread_mutex_t mutex;          /*!< mutex for this event to be used by handlers */
};

struct event_function {
 uint32_t type;                          /*!< type of function */
 void (*handler)(struct einit_event *);  /*!< handler function */
 struct event_function *next;            /*!< next function */
};

struct exported_function {
 uint32_t version;                       /*!< API version (for internal use) */
 void const *function;                   /*!< pointer to the function */
};

enum einit_timer_options {
 einit_timer_once = 0x0001,
 einit_timer_until_cancelled = 0x0002
};

void *event_emit (struct einit_event *, enum einit_event_emit_flags);
void event_listen (enum einit_event_subsystems, void (*)(struct einit_event *));
void event_ignore (enum einit_event_subsystems, void (*)(struct einit_event *));

void function_register (const char *, uint32_t, void const *);
void function_unregister (const char *, uint32_t, void const *);
void **function_find (const char *, const uint32_t, const char **);
void *function_find_one (const char *, const uint32_t, const char **);

struct event_function *event_functions;
struct stree *exported_functions;

char *event_code_to_string (const uint32_t);
uint32_t event_string_to_code (const char *);

time_t event_timer_register (struct tm *);
time_t event_timer_register_timeout (time_t);
void event_timer_cancel (time_t);

#endif

#ifdef __cplusplus
}
#endif
