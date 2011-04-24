#ifndef NMS_SERVER__H
#define NMS_SERVER__H
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
 * Neuros-Cooler platform nms server module header.
 *
 * REVISION:
 * 
 * 
 * 1) Initial creation. ----------------------------------- 2005-09-19 MG 
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
#include "cmd-nms.h"
#include "com-nms.h"

// These SRC_* defines are used internally to the server only, to create the debugging aid information
#define SRC_SERVER_RECSTART        1
#define SRC_SERVER_RECSTOP         2
#define SRC_SERVER_RECRUNNING      3
#define SRC_SERVER_RECPRECOMMIT    4

#define SRC_PLUG_INP_INIT         22
#define SRC_PLUG_INP_START        23

#define SRC_PLUG_OUT_INIT         42
#define SRC_PLUG_OUT_START        43
#define SRC_PLUG_OUT_COMMIT       44

/* server side APIs. */
int      SrvPlayFile(const char*);
int      SrvPlayDir(const char*);
int      SrvPlayHistory(void);
int      SrvRxCmd(void *);
void	 SrvPauseUnpause(void);
void     SrvStop(void);
int      SrvSeek(int);
void     SrvFfRw(int);
void     SrvSfRw(int);
int	     SrvFrameByFrame(int);
int 	 SrvGetRepeatABStatus(void);
int	     SrvRepeatAB(int);
int      SrvTrackChange(int);
void     SrvGetVolume(int *, int *);
void     SrvSetVolume(int, int);
int      SrvGetPlaytime(void);
int	     SrvGetSrvStatus(void);
int      SrvIsPlaying(void);
int      SrvGetPlaymode(void);
void     SrvSetPlaymode(int);
void     SrvSetEditmode( int mode );
int      SrvGetRepeatmode(void);
void     SrvSetRepeatmode(int);
int      SrvGetMediaInfo(const char *, void *);
int      SrvGetTotalFiles(void);
int      SrvGetFileIndex(void);
int      SrvGetFilePath(int idx, void * pathbuf,const int bufsize);

int	SrvStartSlideShow(void);
void SrvSlideShowSetImage(const char *fname);
void SrvStopSlideShow(void);
int  SrvIsSlideShowActive(void);

NMS_SRV_RECORD_ERROR SrvRecord( rec_ctrl_t * ctrl, char * fname, NMS_SRV_ERROR_DETAIL * detail);
void     SrvPauseRecord(int);
void     SrvStopRecord(void);
void     SrvGetGain(int *, int *);
void     SrvSetGain(int, int);
int      SrvGetRecordtime(void);
void     SrvGetRecordError(NMS_SRV_ERROR_DETAIL * detail);
unsigned int SrvGetRecordsize(void);
int      SrvIsRecording(void);
int		 SrvGetFFRWLevel(void);
int		 SrvGetSFRWLevel(void);
int      SrvStartMonitor(int pid);
void     SrvStopMonitor(int pid);
int	   SrvIsMonitorActive(void);
void	   SrvSetProportions(int);
int	   SrvGetProportions(void);
int      NmsSrvInit(int argc, char** argv);
void     NmsSrvStart( void );
int      NmsSrvGetSid( void );

#endif /* NMS_SERVER__H */
