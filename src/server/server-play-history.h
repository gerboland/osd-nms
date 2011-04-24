#ifndef NMS_SERVER_PLAY_HISTORY__H
#define NMS_SERVER_PLAY_HISTORY__H
/*
 *  Copyright(C) 2006 Neuros Technology International LLC. 
 *               <www.neurostechnology.com>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2 of the License.
 *  
 *
 *  This program is distributed in the hope that, in addition to its 
 *  original purpose to support Neuros hardware, it will be useful 
 *  otherwise, but WITHOUT ANY WARRANTY; without even the implied 
 *  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  
 *  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 ****************************************************************************
 *
 * Neuros-Cooler platform nms playback history header.
 *
 * REVISION:
 * 
 * 
 * 1) Initial creation. ----------------------------------- 2006-09-04 MG 
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "nc-type.h"

/// player history management
typedef struct
{
  int  yes;       ///0: history not available 1: available
  int  type;     ///playback type NPT_xxx, see com-nms.h
  int  fileIdx;  ///file index
  int  mark;     ///bookmark of current file
  char path[260];///full path to playable contents
} play_history_t;


void SetPlayHistory(int, int, int, const char*);
int  GetPlayHistory(play_history_t *);

#endif /* NMS_SERVER_PLAY_HISTORY__H */
