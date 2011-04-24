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
 * Neuros-Cooler platform nms playback history server module.
 *
 * REVISION:
 * 
 * 1) Initial creation. ----------------------------------- 2006-09-04 MG 
 *
 */

#include <pthread.h>
#include "cmd-nms.h"
#include "com-nms.h"
#include "server-nms.h"
#include "nmsplugin.h"
#include "plugin-internals.h"
#include "dirtree.h"
#include "file-helper.h"
#include "server-play-history.h"

static play_history_t history =
  {
	.yes = 0, // history not available.
  };

/** set play history. 
 *
 * @param type
 *        playback type, see com-nms.h
 * @param fileIdx
 *        file index.
 * @param mark
 *        current file bookmark
 * @param path
 *        full path to playable contents
 *
 */
void 
SetPlayHistory(int type, int fileIdx, int mark, const char * path)
{
	history.type = type;
	history.fileIdx = fileIdx;
	history.mark = mark;
	//FIXME: buffer overflow check
	strcpy(history.path, path);

    history.yes = 1;
}

/** get play history.
 *
 * @param his
 *        current history if available.
 * @return
 *        0 if history successfully fetched, nonzero otherwise.
 *
 */
int
GetPlayHistory(play_history_t * his)
{
    if (history.yes)
	  {
		memcpy(his, &history, sizeof(history));
		return 0;
	  }
	return -1;
}
