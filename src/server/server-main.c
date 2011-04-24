/*
 *  Copyright(C) 2005 Neuros Technology International LLC. 
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
 * Neuros-Cooler platform nms server module.
 *
 * REVISION:
 * 
 * 5) Added in background preference support, start of
 *    service does not stop other by default. ------------- 2007-08-07 MG
 * 4) Remove table for dbg cmd names. Handle PLAY retval -- 2007-05-24 nerochiaro
 * 3) Get rid of extra ACK if data return is expected. ---- 2006-04-14 MG
 * 2) Added encoder interface. ---------------------------- 2006-01-06 MG
 * 1) Initial creation. ----------------------------------- 2005-09-19 MG 
 *
 */

#include <pthread.h>
#include <unistd.h>
//#define OSD_DBG_MSG
#include "nc-err.h"

#include "cmd-nms.h"
#include "com-nms.h"
#include "server-nms.h"
#include "nmsplugin.h"
#include "plugin-internals.h"
#include "video-control.h"
#include "server-monitor-internal.h"

const char version[] = "1.0.2";
static int just_recorded = FALSE;
static char record_file[PATH_MAX];

static int StartPlay ( void * data, int dataLen )
{
	int type;
	char * p;
	int ret  = 0;

	DBGLOG("start to playback.");

	p = (char*)data;
	memcpy(&type, p, sizeof(int));

	switch (type)
	  {
	  case NPT_FILE:     /* single file. */
		  //DBGLOG("Start to play file %s", p+sizeof(int));
		  DBGLOG("Start to play file");
		  ret = SrvPlayFile(p+sizeof(int));
		  just_recorded = FALSE;
		  break;

	  case NPT_DIR:      /* directory.   */
		  //DBGLOG("Start to play directory %s", p+sizeof(int));
		  DBGLOG("Start to play directory");
		  ret = SrvPlayDir(p+sizeof(int));
		  just_recorded = FALSE;
		  break;
		  
	  case NPT_HISTORY:  /* directory.   */
		  DBGLOG("Start to play history");
		  if (just_recorded)
		  {
			ret = SrvPlayFile(record_file);
			just_recorded = FALSE;
		  }
		  else
			ret = SrvPlayHistory();
		  break;

	  case NPT_PLAYLIST: /* playlist.    */
	  case NPT_DB:       /* Neuros DB.   */
	  case NPT_OTHER:     /* other.       */
	  default: break;
	  }
	return ret;
}

static NMS_SRV_RECORD_ERROR StartRecord ( void * data, int dataLen, NMS_SRV_ERROR_DETAIL *detail )
{
	rec_ctrl_t *params = (rec_ctrl_t*) data;

	memset(record_file, 0, sizeof(record_file));
	strncpy(record_file, data + sizeof(rec_ctrl_t), dataLen);

	just_recorded = TRUE;
	return SrvRecord(params, data + sizeof(rec_ctrl_t), detail);
}

/**
 * Server command receiver routine.
 *
 * @param pkt
 *        packet node.
 * @return
 *        15 to exit command thread, all other value
 *        are considered normal.
 */
int
SrvRxCmd( void * pkt )
{
	int ret = 0;
	int acked = 0;

	pkt_node_t * p = (pkt_node_t *)pkt;

	DBGLOG("command: %d", p->hdr.cmd);
	switch (p->hdr.cmd) 
	{
	case CMD_GET_VERSION:
		DBGLOG("CMD_GET_VERSION.");
		CoolCmdSendPacket(p->fd, CMD_GET_VERSION|NMS_CMD_ACK, 
					  (void*)version, strlen(version)+1);
		acked = 1;
		break;

		// careful: this will be the last command server responds to.
	case CMD_STOP_SERVER: 
		DBGLOG("CMD_STOP_SERVER.");
		ret = 15;
		break;
		
	case CMD_SET_INPUT_MODE:
		DBGLOG("CMD_SET_INPUT_MODE.");
		EncInputSetMode(*(int*)p->data);
		break;
		
	case CMD_SET_OUTPUT_MODE:
		DBGLOG("CMD_SET_OUTPUT_MODE.");
	  	{
			OutputSetMode(*(int*)p->data);
			if (SrvIsMonitorActive())
			{
				SrvStartMonitorInternal();
			}
			else if (SrvIsRecording())
			{
			}
			else if (SrvIsPlaying())
			{
				OutputActivateMode(0);
			}
	  	}
	  	break;

	case CMD_SET_OUTPUT_PROPORTIONS:
		DBGLOG("CMD_SET_OUTPUT_PROPORTIONS.");
		SrvSetProportions((*(int*)p->data));
		break;

	case CMD_GET_OUTPUT_PROPORTIONS:
		DBGLOG("CMD_GET_OUTPUT_PROPORTIONS.");
		int proportions = SrvGetProportions();
		CoolCmdSendPacket(p->fd, CMD_GET_OUTPUT_PROPORTIONS|NMS_CMD_ACK, 
		                  (void*)&proportions, sizeof(int));
		acked = 1;
		break;

	case CMD_START_SLIDE_SHOW:
		DBGLOG("CMD_START_SLIDE_SHOW.");
		{
			int startslideshow;
			startslideshow = SrvStartSlideShow();
			CoolCmdSendPacket(p->fd, CMD_START_SLIDE_SHOW|NMS_CMD_ACK, 
						  (void*)&startslideshow, sizeof(int));
			acked = 1;
			DBGLOG("CMD_START_SLIDE_SHOW completed.");
		}
		break;
		
	case CMD_SET_SLIDE_SHOW_IMAGE:
		DBGLOG("CMD_SLIDE_SHOW_SET_IMAGE.");
		{
			SrvSlideShowSetImage((char *)p->data);
		}
		break;
		
	case CMD_STOP_SLIDE_SHOW:
		DBGLOG("CMD_SLIDE_SHOW_STOP.");
		SrvStopSlideShow();
		break;
		
	case CMD_PLAY:
		DBGLOG("CMD_PLAY.");
		int ret = StartPlay(p->data, p->hdr.dataLen);
		CoolCmdSendPacket(p->fd, CMD_PLAY|NMS_CMD_ACK, 
					  (void*)&ret, sizeof(int));
		acked = 1;
		break;
		
	case CMD_PAUSE_UNPAUSE:
		DBGLOG("CMD_PAUSE_UNPAUSE.");
		SrvPauseUnpause();
		break;
		
	case CMD_STOP_PLAY:
		DBGLOG("CMD_STOP_PLAY.");
		SrvStop();
		break;
		
	case CMD_GET_SRV_STATUS:
		DBGLOG("CMD_GET_SRV_STATUS.");
	  	{
			int status;
			status = SrvGetSrvStatus();
			CoolCmdSendPacket(p->fd, CMD_GET_SRV_STATUS|NMS_CMD_ACK,                                                 
						  (void*)&status, sizeof(int));
			acked = 1;
	  	}
	  	break;
		
	case CMD_GET_VOLUME:
		DBGLOG("CMD_GET_VOLUME.");
		{
			int vol[2];
			SrvGetVolume(&vol[0], &vol[1]);
			CoolCmdSendPacket(p->fd, CMD_GET_VOLUME|NMS_CMD_ACK, 
						  (void*)vol, sizeof(vol));
			acked = 1;
		}
		break;
		
	case CMD_SET_VOLUME:
		DBGLOG("CMD_SET_VOLUME.");
		{
			int vol[2];
			int * pv;
			pv = (int*)p->data;
			vol[0] = *pv++;
			vol[1] = *pv;
			SrvSetVolume(vol[0], vol[1]);
		}
		break;
		
	case CMD_GET_PLAY_TIME:
		DBGLOG("CMD_GET_PLAYTIME.");
		{
			int t;
			t = SrvGetPlaytime();
			CoolCmdSendPacket(p->fd, CMD_GET_PLAY_TIME|NMS_CMD_ACK,                                                 
						  (void*)&t, sizeof(int));
			acked = 1;
		}
		break;
		
	case CMD_SEEK:
		DBGLOG("CMD_SEEK.");
		{
			int t;
			int *pv;
			pv = (int*)p->data;
			t = *pv;
			t = SrvSeek(t);
			CoolCmdSendPacket(p->fd, CMD_SEEK|NMS_CMD_ACK, 
						  (void*)&t, sizeof(int));
			acked = 1;
		}
		break;
		
	case CMD_TRACK_CHANGE:
		DBGLOG("CMD_TRACK_CHANGE.");
		{
			int track;
			int *pv;
			int retv;
			pv = (int*)p->data;
			track = *pv;
			retv = SrvTrackChange(track);
			CoolCmdSendPacket(p->fd, CMD_TRACK_CHANGE|NMS_CMD_ACK, 
						  (void*)&retv, sizeof(int));
			acked = 1;
		}
		break;
		
	case CMD_FF_RW:
		DBGLOG("CMD_FF_RW.");
		SrvFfRw(*(int*)p->data);
		break;
		
	case CMD_GET_FFRW_LEVEL:
		DBGLOG("CMD_GET_FFRW_LEVEL.");
	 	{
			int level = SrvGetFFRWLevel();
			CoolCmdSendPacket(p->fd, CMD_GET_FFRW_LEVEL|NMS_CMD_ACK, 
						  (void*)&level, sizeof(int));
			acked = 1;
	 	}
		break;

	case CMD_SF_RW:
		DBGLOG("CMD_SF_RW.");
		SrvSfRw(*(int*)p->data);
		break;
		
	case CMD_GET_SFRW_LEVEL:
		DBGLOG("CMD_GET_SFRW_LEVEL.");
	 	{
			int level = SrvGetSFRWLevel();
			CoolCmdSendPacket(p->fd, CMD_GET_SFRW_LEVEL|NMS_CMD_ACK, 
						  (void*)&level, sizeof(int));
			acked = 1;
	 	}
		break;
		
	case CMD_FRAME_BY_FRAME:	
		DBGLOG("CMD_FRAME_BY_FRAME.");	
		SrvFrameByFrame(*(int*)p->data);	
		break;

	case CMD_REPEAT_A_B:
		DBGLOG("CMD_REPEAT_A_B.");	
		SrvRepeatAB(*(int*)p->data);	
		break;
		
	case CMD_GET_REPEAT_AB_STATUS:
		DBGLOG("CMD_GET_REPEAT_AB_STATUS.");
		{
			int status = SrvGetRepeatABStatus();
			CoolCmdSendPacket(p->fd, CMD_GET_REPEAT_AB_STATUS|NMS_CMD_ACK, 
						  (void*)&status, sizeof(int));
			acked = 1;
		}
		break;
		
	case CMD_IS_PLAYING:
		DBGLOG("CMD_IS_PLAYING.");
		{
			int playing = SrvIsPlaying();
			CoolCmdSendPacket(p->fd, CMD_IS_PLAYING|NMS_CMD_ACK, 
						  (void*)&playing, sizeof(int));
			acked = 1;
		}
		break;
		
	case CMD_GET_PLAYMODE:
		DBGLOG("CMD_GET_PLAYMODE.");
		{
			int mode = SrvGetPlaymode();
			CoolCmdSendPacket(p->fd, CMD_GET_PLAYMODE|NMS_CMD_ACK, 
						  (void*)&mode, sizeof(int));
			acked = 1;
		}
		break;
		
	case CMD_SET_PLAYMODE:
		DBGLOG("CMD_SET_PLAYMODE.");
		{
			SrvSetPlaymode(*((int*)p->data));
		}
		break;
		
	case CMD_SET_EDITMODE:
		DBGLOG("CMD_SET_EDITMODE.");
		{
			SrvSetEditmode(*((int*)p->data));
		}
		break;	
		
	case CMD_GET_REPEATMODE:
		DBGLOG("CMD_GET_REPEATMODE.");
		{
			int mode = SrvGetRepeatmode();
			CoolCmdSendPacket(p->fd, CMD_GET_REPEATMODE|NMS_CMD_ACK, 
						  (void*)&mode, sizeof(int));
			acked = 1;
		}
		break;
		
	case CMD_SET_REPEATMODE:
		DBGLOG("CMD_SET_REPEATMODE.");
		{
			SrvSetRepeatmode(*((int*)p->data));
		}
		break;
		
	case CMD_GET_TOTAL_FILES:
		DBGLOG("CMD_GET_TOTAL_FILES.");
		{
			int num = SrvGetTotalFiles();
			CoolCmdSendPacket(p->fd, CMD_GET_TOTAL_FILES|NMS_CMD_ACK, 
						  (void*)&num, sizeof(int));
			acked = 1;
		}
		break;
		
	case CMD_GET_FILE_INDEX:
		DBGLOG("CMD_GET_FILE_INDEX.");
		{
			int idx = SrvGetFileIndex();
			CoolCmdSendPacket(p->fd, CMD_GET_FILE_INDEX|NMS_CMD_ACK, 
						  (void*)&idx, sizeof(int));
			acked = 1;
		}
		break;
		
	case CMD_GET_FILE_PATH:
		DBGLOG("CMD_GET_FILE_PATH.");
		{
			char path[PATH_MAX];
			int idx;
			int *pv;
			
			pv = (int*)p->data;
			idx = *pv;
			SrvGetFilePath(idx, path,PATH_MAX);
			
			CoolCmdSendPacket(p->fd, CMD_GET_FILE_PATH|NMS_CMD_ACK, 
						  (void*)path, strlen(path)+1);
			acked = 1;
		}
		break;
		
	case CMD_MEDIA_INFO:
		DBGLOG("CMD_MEDIA_INFO.");
		{
			media_info_t media_info;
			
			memset(&media_info, 0 , sizeof(media_info_t));
			SrvGetMediaInfo((char *)p->data, &media_info);
			
			CoolCmdSendPacket(p->fd,CMD_MEDIA_INFO|NMS_CMD_ACK,
						  (void *)(&media_info),sizeof(media_info_t));
			acked = 1;
		}
		break;		

		/* start of encoder interface. */
	case CMD_RECORD:
		DBGLOG("CMD_RECORD.");

		NMS_SRV_ERROR_DETAIL result;
		StartRecord(p->data, p->hdr.dataLen, &result);

		CoolCmdSendPacket(p->fd, CMD_RECORD | NMS_CMD_ACK,
		                  (void *)(&result), sizeof(NMS_SRV_ERROR_DETAIL));
		acked = 1;

		break;

	case CMD_PAUSE_UNPAUSE_RECORD:
		DBGLOG("CMD_PAUSE_UNPAUSE_RECORD.");
		SrvPauseRecord(*(int*)p->data);
		break;
		
	case CMD_STOP_RECORD:
		DBGLOG("CMD_STOP_RECORD.");
		SrvStopRecord();
		break;
		
	case CMD_GET_GAIN:
		DBGLOG("CMD_GET_GAIN.");
		{
			int gain[2];
			SrvGetGain(&gain[0], &gain[1]);
			CoolCmdSendPacket(p->fd, CMD_GET_GAIN|NMS_CMD_ACK, 
						  (void*)gain, sizeof(gain));
			acked = 1;
		}
		break;
		
	case CMD_SET_GAIN:
		DBGLOG("CMD_SET_GAIN.");
		{
			int gain[2];
			int * pg;
			pg = (int*)p->data;
			gain[0] = *pg++;
			gain[1] = *pg;
			SrvSetGain(gain[0], gain[1]);
		}
		break;
		
	case CMD_GET_RECORD_TIME:
		DBGLOG("CMD_GET_RECORD_TIME.");
		{
			int t;
			t = SrvGetRecordtime();
			CoolCmdSendPacket(p->fd, CMD_GET_RECORD_TIME|NMS_CMD_ACK, 
						  (void*)&t, sizeof(int));
			acked = 1;
		}
		break;

	case CMD_GET_RECORD_SIZE:
		DBGLOG("CMD_GET_RECORD_SIZE.");
		{
			unsigned int t;
			t = SrvGetRecordsize();
			CoolCmdSendPacket(p->fd, CMD_GET_RECORD_SIZE|NMS_CMD_ACK, 
						  (void*)&t, sizeof(unsigned int));
			acked = 1;
		}
		break;

	case CMD_GET_RECORD_ERROR:
		DBGLOG("CMD_GET_RECORD_ERROR.");
		{
			NMS_SRV_ERROR_DETAIL det;
			SrvGetRecordError(&det);
			
			CoolCmdSendPacket(p->fd, CMD_GET_RECORD_ERROR|NMS_CMD_ACK,
			                  (void*)&det, sizeof(NMS_SRV_ERROR_DETAIL));
			acked = 1;
		}
		break;
		
	case CMD_IS_RECORDING:
		DBGLOG("CMD_IS_RECORDING.");
		{
			int recording = SrvIsRecording();
			CoolCmdSendPacket(p->fd, CMD_IS_RECORDING|NMS_CMD_ACK, 
						  (void*)&recording, sizeof(int));
			acked = 1;
		}
		break;
		
	case CMD_START_MONITOR:
		DBGLOG("CMD_START_MONITOR.");
		{
			int startmonitor;
			int pid = (*(int*)p->data);
			startmonitor = SrvStartMonitor(pid);
			CoolCmdSendPacket(p->fd, CMD_START_MONITOR|NMS_CMD_ACK, 
						  (void*)&startmonitor, sizeof(int));
			acked = 1;
			DBGLOG("CMD_START_MONITOR completed.");
		}
		break;
		
	case CMD_STOP_MONITOR:
		DBGLOG("CMD_STOP_MONITOR.");

		int pid = (*(int*)p->data);
		SrvStopMonitor(pid);
		break;

	case CMD_IS_MONITOR_ENABLED:
		DBGLOG("CMD_IS_MONITOR_ENABLED.");
		{
			int monitoractive = SrvIsMonitorActive();
			CoolCmdSendPacket(p->fd, CMD_IS_MONITOR_ENABLED|NMS_CMD_ACK, 
						  (void*)&monitoractive, sizeof(int));
			acked = 1;
		}
		break;
		
	case CMD_CAP_INIT:
		DBGLOG("CMD_CAP_INIT.");
		{
			capture_desc_t desc;
			capture_ret_t capret;
			desc.capture_type = (*(int*)p->data);
			int suc = CaptureInit(&desc);
			if(suc)
			{
				capret.width = -1;
				capret.height = -1;
			}
			else
			{
				capret.width = desc.width;
				capret.height = desc.height;
			}
			capret.ret = suc;
			CoolCmdSendPacket(p->fd,CMD_CAP_INIT|NMS_CMD_ACK,
						  (void *)(&capret),sizeof(capture_ret_t));
			acked = 1;
		}
		break;	

	case CMD_CAP_GET_FRAME:
		DBGLOG("CMD_CAP_GET_FRAME.");
		{
			frame_desc_t desc;
			int suc = CaptureGetFrame(&desc);
			if(suc)
			{
				CoolCmdSendPacket(p->fd, CMD_CAP_GET_FRAME|NMS_CMD_ACK, 
						  (void *)(&suc), sizeof(int));
			}
			else
			{
				CoolCmdSendPacket(p->fd, CMD_CAP_GET_FRAME|NMS_CMD_ACK, 
						  (void *)(desc.data), desc.size);
				CaptureReleaseFrame();
			}
			acked = 1;
		}
		break;
			
	case CMD_CAP_FINISH:
		DBGLOG("CMD_CAP_FINISH.");
		{
			int suc = CaptureFinish();
			CoolCmdSendPacket(p->fd, CMD_CAP_FINISH|NMS_CMD_ACK, 
						  (void*)&suc, sizeof(int));
			acked = 1;
		}
		break;

	case CMD_PING:
		DBGLOG("CMD_PING.");
		break;

	default:
		/* retain the data for now. */
		WPRINT("unknown command, data retained.");
	    ret = 1; 
		break;
	}
	
	/* send back original command as ack. */
	if (!acked)	
	{
		CoolCmdSendPacket(p->fd, p->hdr.cmd|NMS_CMD_ACK, NULL, 0);
		DBGLOG("server acked.");
	}
	close(p->fd);
	
	/* release data. */
	if ( 0 == ret ) 
	{
		if (p->hdr.dataLen)  free(p->data);
	} 
	
	return ret;
}
