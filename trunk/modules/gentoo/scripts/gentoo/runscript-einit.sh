#!/bin/sh
#-----------------------------------------------------------------------------
# script to translate regular init.d script executions into einit-control rc calls
#-----------------------------------------------------------------------------
# Copyright (c) 2007, Magnus Deininger
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
#
#     * Redistributions of source code must retain the above copyright notice,
#       this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright notice,
#       this list of conditions and the following disclaimer in the documentation
#       and/or other materials provided with the distribution.
#     * Neither the name of the project nor the names of its contributors may be
#       used to endorse or promote products derived from this software without
#       specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
# ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
# ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

if ! test -n "$1"; then
 echo " * do not call this script directly, this is supposed to run other scripts!";
 exit 1;
fi

script=$1
action=$2

if ! test -n "${action}"; then
 if ! test "$0" != "/sbin/runscript-einit.sh"; then
  action=$1;
  script=$0;
 else
  echo " * you did not specify an action";
  exit 1;
 fi
fi

if test "${action}" = "start"; then
 action="enable";
elif test "${action}" = "stop"; then
 action="disable"
fi

servicename="$(basename ${script})"
servicename="$(echo ${servicename}|sed -e y/./-/)"

/sbin/einit-control rc ${servicename} ${action}
