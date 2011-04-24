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
 * Neuros-Cooler platform nms playback server module.
 *
 * REVISION:
 *
 * 5) Server mutex and state machine cleanup, only one mutex is needed
 *    to protect various server state and control flags, also to protect
 *    simultaneous access to non-reentrant APIs. ---------- 2007-12-14 MG
 * 4) Added in background preference support. ------------- 2007-08-07 MG 
 * 3) Handle output locked status.------------------------- 2007-05-24 nerochiaro
 * 2) Directory playback support. ------------------------- 2006-09-02 MG
 * 1) Initial creation. ----------------------------------- 2005-09-19 MG 
 *
 */

#include <pthread.h>
#include <unistd.h>
#include <sched.h>

//#define LOG_TIME_STAMP__
//#define OSD_DBG_MSG
#include "nc-err.h"

#include "cmd-nms.h"
#include "com-nms.h"
#include "server-nms.h"
#include "nmsplugin.h"
#include "plugin-internals.h"
#include "dirtree.h"
#include "file-helper.h"
#include "server-play-history.h"

// define this to playback video only
#define PLAY_VIDEO_FILE_ONLY

#define F_INDEX(index)    	((index < node.fdnum)?				\
							 node.fdindex[index]:				\
							 node.ffindex[index-node.fdnum]) 
#define D_NAME(index)		(node.namelist[index]->d_name)
#define D_PATH				(node.path)


#define DRAIN_POLL_TICK 200000  // unit: micro-second

//FIXME: Should be a method to calculate the scan step dynamicly.
#define SCAN_STEP1_MS   50      // scan step in mili-seconds, for ffrw level <= 2.
#define SCAN_STEP2_MS   130     // scan step in mili-seconds, for ffrw level > 2.
#define PRELOAD_FRAMES  60

typedef enum
	{
		PM_SINGLE,
		PM_FOLDER
	} PLAY_MODE;

typedef enum
	{
		RM_NORMAL,
		RM_REPEAT,
		RM_SHUFFLE
	} REPEAT_MODE;

typedef enum
	{
		TC_DISABLE,
		TC_NORMAL,
		TC_NEXT,
		TC_PREVIOUS
	} TRACK_CHANGE;


// thread variables (thread, mutex, and conditionals) themselves 
// are thread-safe for sure.
static pthread_t          avThread = (pthread_t)NULL; 
static pthread_t          dirInitThread = (pthread_t)NULL;
static pthread_t          nextFileThread = (pthread_t)NULL;

// mutex fast forward, server main thread and playback thread
static pthread_mutex_t    playMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t     playdirCond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t     nextFileCond = PTHREAD_COND_INITIALIZER;

#define LOCK_PLAYMUTEX()  do {					\
		/*DBGMSG("playmutex locking.");*/		\
		pthread_mutex_lock(&playMutex);			\
		/*DBGMSG("playmutex locked.");*/		\
	}while(0)
#define UNLOCK_PLAYMUTEX()  do{					\
		/*DBGMSG("playmutex unlocking.");*/		\
		pthread_mutex_unlock(&playMutex);		\
		/*DBGMSG("playmutex unlocked.");*/		\
	}while(0)


// player state controls: default to be mutex protected.
static int                requestToStopPlaying = 0;
static int 				  muted;
static int                preloaded; // player preload completed.
static int                going;     // is playback server running?
static int                playing;   // is any file playing?
static int                ffrwLevel; // fast forward/rewind level control
static int                sfrwLevel; // slow forward/rewind level control
static int                fileIdx;   // active playing file index, starts from 0
static int                totalFiles;// total number of playable contents
static int                fileCnt;   // played file counter
static char               trackName[PATH_MAX];
static int                errorStatus;// play status
static int                everPlayed;// have we ever played successfully.
static int                playtype;  // playback type


// player mode controls
static int                editmode = 0; // 0: play 1: edit
static REPEAT_MODE        repeat = RM_REPEAT;// 0: Normal 1: Repeat 2: Shuffle
static PLAY_MODE          playmode;  // 0: single 1: Folder
static TRACK_CHANGE       trackChange;  
static int                firstplay; //the index of the file that we first play


// directory playback support
static int                dirInited = 0;
static char               dirName[PATH_MAX];
#define DPAV_NOT_DETERMINED_ 0
#define DPAV_AUDIO_          1
#define DPAV_VIDEO_          2
static int                dirPlayAV; 
static dir_node_t         node;

static NMS_SRV_STATUS_t   playState;
static NMS_SRV_PLAYBACK_REPEAT_STATUS_t rptState = NMS_PLAYBACK_REPEAT_OFF;
static int                rptA;           // if disable repeat A-B, set rptA =  rptB = 0;
static int                rptB;
static int                frameByFrame;   // 0 : disable frame-by-frame, 1 : enable frame-by-frame
static NMS_SRV_STATUS_t   playedOrPaused; // 0: play status, 1 : pause status.
static int                sfrwLevelFinal; // if sfrw exit, record the sfrwLevel to restore right play time
static int                playtime;       // FIXME, control slow forward display play time
static int                iBookMark;
static int                iSeekFlag;
static media_info_t       info;

static int                curProportions = 0; // current output proportions. effective only on next playback.

static int newThread(pthread_t * thread,
					 const pthread_attr_t * attr,
					 void *(*start_routine)(void*), void * arg)
{
	int status;
	
	status = pthread_create(thread, attr, start_routine, arg);
	if (status)
	{
		WPRINT("Thread was not created!");
		switch (status)
		{
		case EAGAIN:
			EPRINT("The system lacked the necessary resources to create "
				   "another thread, or the system-imposed limit on the total "
				   "number of threads in a process {PTHREAD_THREADS_MAX} " 
				   "would be exceeded.");
			break;
		case EINVAL:
			EPRINT("The value specified by attr is invalid.");
			break;
		case EPERM:
			EPRINT("The caller does not have appropriate permission to "
				   "set  the  required "
				   "scheduling  parameters  or scheduling policy.");
			break;
		default:
			EPRINT("Unknown error!");
			break;
		}
	}
	return status;
}

static void * avLoop( void * arg )
{
	int loc_frames = 0;
	int loc_bytes;
	int loc_quit = 1;
	
	NMS_SRV_STATUS_t loc_preState = NMS_STATUS_PLAYER_STOPPED;
	int loc_cur_t = 0;
	int loc_next_t = 0;
	int loc_prev_t = 0;
	int loc_first_time = 0;
	int loc_preload;
	int loc_ffrw_scan_step = 0;
	media_buf_t loc_buf;
	long loc_info_duration;

	LOCK_PLAYMUTEX();
	if(editmode)
	{
		playState = NMS_STATUS_PLAYER_VF;
		loc_preload = 1;
		frameByFrame = 1;
	}
	else
	{
		playState =NMS_STATUS_PLAYER_PLAY; 
		loc_preload = PRELOAD_FRAMES;
		frameByFrame = 0;
	}
	UNLOCK_PLAYMUTEX();
	
	do
	{
		DBGMSG("In avloop now.");	
		/* preload */
		while (loc_frames < loc_preload)
		{
			if (1 == OutputGetBuffer(&loc_buf, 0, 1))
			{
				WPRINT("GetBuffer failed.");
				break;
			}
			
			loc_bytes = InputGetData(&loc_buf);
			if (loc_bytes == 0) continue;
			if (loc_bytes <0 )
			{
				// hit EOF, set flag to flush.
				loc_quit = 0;
				
				LOCK_PLAYMUTEX();
				playing = 0;
				UNLOCK_PLAYMUTEX();
				break; 
			}
			//DBGMSG("input data returned. ");
			LOCK_PLAYMUTEX();
			playtime = OutputGetPlaytime();
			UNLOCK_PLAYMUTEX();

#ifdef LOG_TIME_STAMP__
			if (loc_buf.curbuf == &loc_buf.abuf)
				DBGLOG("   aT = %d\n", loc_buf.curbuf->tsms);
			else
				DBGLOG("---vT = %d\n", loc_buf.curbuf->tsms);
#endif
			
			OutputWrite(&loc_buf);
			if (loc_buf.curbuf == &loc_buf.abuf) loc_frames++;
		}
		DBGMSG("preload finished.");
		OutputStart();
		/* start output */
		DBGMSG("output started.");
	} while(0);
	
	LOCK_PLAYMUTEX();
	preloaded = 1;
	everPlayed = 1;
	UNLOCK_PLAYMUTEX();
	
 main_play_loop:
	// preloaded...
	while (1)
	{	
		int loc_playState;
		int loc_iSeekFlag;
		int loc_iBookMark;
		int loc_sfrwLevelFinal;

		LOCK_PLAYMUTEX();
		if (!playing)
		{
			UNLOCK_PLAYMUTEX();
			break;
		}
		loc_playState = playState;
		UNLOCK_PLAYMUTEX();
		
		switch (loc_playState)
		{
		case NMS_STATUS_PLAYER_PAUSE:
			if (loc_preState != NMS_STATUS_PLAYER_PAUSE)
			{
				OutputPause(1);
				LOCK_PLAYMUTEX();
				playedOrPaused = NMS_STATUS_PLAYER_PAUSE;
				UNLOCK_PLAYMUTEX();
				loc_preState = NMS_STATUS_PLAYER_PAUSE;
			}
			continue;

		case NMS_STATUS_PLAYER_PLAY:
			{
				LOCK_PLAYMUTEX();
				loc_iSeekFlag = iSeekFlag;
				loc_iBookMark = iBookMark;
				UNLOCK_PLAYMUTEX();

				if (loc_preState != NMS_STATUS_PLAYER_PLAY || loc_iSeekFlag)
				{
					LOCK_PLAYMUTEX();
					playedOrPaused = NMS_STATUS_PLAYER_PLAY;		
					loc_info_duration = info.duration;			
					UNLOCK_PLAYMUTEX();

					if (loc_preState == NMS_STATUS_PLAYER_PAUSE)
					{
						OutputPause(0);	

						LOCK_PLAYMUTEX();
						muted = 0;
						UNLOCK_PLAYMUTEX();
					}
					else if (loc_iSeekFlag)
					{
						if (loc_info_duration - loc_iBookMark > 100)
						{
							loc_next_t = InputSeek(loc_iBookMark);						
							OutputFlush(loc_next_t);
						}
						LOCK_PLAYMUTEX();
						iSeekFlag = 0;
						UNLOCK_PLAYMUTEX();
					}
					else
					{
						LOCK_PLAYMUTEX();
						if (muted)
						{
							muted = 0;
							OutputMute(muted);
						}
						loc_sfrwLevelFinal = sfrwLevelFinal;
						UNLOCK_PLAYMUTEX();

						if (loc_preState == NMS_STATUS_PLAYER_SF)
						{
							int loc_cur_tmp;

							LOCK_PLAYMUTEX();
							loc_cur_tmp = playtime;
							UNLOCK_PLAYMUTEX();

							loc_next_t = loc_first_time + (loc_cur_tmp - loc_first_time) / loc_sfrwLevelFinal;
							loc_next_t = InputSeek(loc_next_t);
							OutputFlush(loc_next_t);	
						}
					}
					loc_preState = NMS_STATUS_PLAYER_PLAY;					
					loc_prev_t = 0;
				}				
			}
			break;
		case NMS_STATUS_PLAYER_FF:
		case NMS_STATUS_PLAYER_RW:
			if (loc_preState != loc_playState)
			{
				LOCK_PLAYMUTEX();
				loc_cur_t = playtime;
				UNLOCK_PLAYMUTEX();

				OutputFlush(loc_cur_t);
				if (loc_preState == NMS_STATUS_PLAYER_PAUSE)
					OutputPause(0);
				
				LOCK_PLAYMUTEX();
				if (!muted)
				{
					muted = 1;
					OutputMute(muted);
				}
				loc_cur_t = playtime;
				loc_first_time = loc_cur_t;
				loc_next_t = loc_cur_t;
				loc_prev_t = loc_cur_t;
				loc_preState = playState;
				UNLOCK_PLAYMUTEX();
			}

			LOCK_PLAYMUTEX();
			if (ffrwLevel == 0)
			{
				UNLOCK_PLAYMUTEX();
				continue;
			}
			else if (ffrwLevel > 2)
				loc_ffrw_scan_step = SCAN_STEP2_MS;
			else 
				loc_ffrw_scan_step = SCAN_STEP1_MS;

 
		get_new_timestamp:
			if (loc_preState == NMS_STATUS_PLAYER_FF)
			{
				loc_info_duration = info.duration;
				if (loc_info_duration != 0 && (playtime + ffrwLevel * loc_ffrw_scan_step) >= loc_info_duration)
				{
					loc_next_t = loc_info_duration;
					loc_next_t = InputSeek(loc_next_t);
					OutputFlush(loc_next_t);
					
					if(editmode)
					{
						playState = NMS_STATUS_PLAYER_PLAY;
						UNLOCK_PLAYMUTEX();
						break;	
					}
					else
					{
						loc_quit = 0;
						UNLOCK_PLAYMUTEX();
						goto bail;
					}					
				}
				else
				{
					playtime += ffrwLevel * loc_ffrw_scan_step;
					loc_next_t = playtime;
					if (rptState == NMS_PLAYBACK_REPEAT_ON)
					{
						if (loc_next_t >= rptB)
						{
							loc_next_t = rptB;
							loc_next_t = InputSeek(loc_next_t);
							OutputFlush(loc_next_t);
							UNLOCK_PLAYMUTEX();
							continue;
						}
					}
				}
			}
			else
			{
				if (playtime +ffrwLevel * loc_ffrw_scan_step <= 0)
				{
					loc_next_t = 0;
					loc_next_t = InputSeek(loc_next_t);
					OutputFlush(loc_next_t);
					if(editmode)
					{
						playState = NMS_STATUS_PLAYER_VF;
						frameByFrame = 1;
						loc_preState = NMS_STATUS_PLAYER_PLAY;
						UNLOCK_PLAYMUTEX();
						break;
					}
					else
						playState = NMS_STATUS_PLAYER_PLAY;

					ffrwLevel = 0;
					UNLOCK_PLAYMUTEX();
					continue;
				}
				else
				{
					playtime += ffrwLevel * loc_ffrw_scan_step;
					loc_next_t = playtime;
					if (rptState == NMS_PLAYBACK_REPEAT_ON)
					{
						if (loc_next_t <= rptA)
						{
							loc_next_t = rptA;
							loc_next_t = InputSeek(loc_next_t);
							OutputFlush(loc_next_t);
							UNLOCK_PLAYMUTEX();
							continue;
						}
					}
				}
			}

			loc_next_t = InputSeek(loc_next_t);
			OutputFlush(loc_next_t);
			if ((loc_preState == NMS_STATUS_PLAYER_FF && loc_prev_t >= loc_next_t) ||
				(loc_preState == NMS_STATUS_PLAYER_RW && loc_prev_t <= loc_next_t))
			{
				goto get_new_timestamp;
			}
			loc_prev_t = loc_cur_t;
			loc_cur_t = loc_next_t;			
			UNLOCK_PLAYMUTEX();
			break;

		case NMS_STATUS_PLAYER_VF:
			LOCK_PLAYMUTEX();
			DBGLOG("!!!!!!!!!!!!!!!play VF,mute=%d\n",muted);
			if (frameByFrame == 1)
			{
				OutputPause(0);
				loc_cur_t = playtime;
				OutputFlush(loc_cur_t);
				if (!muted)
				{
					muted = 1;
		            OutputMute(muted);
				}
			}
			playState = NMS_STATUS_PLAYER_PAUSE;
			loc_cur_t = playtime;
			UNLOCK_PLAYMUTEX();

			loc_next_t = loc_cur_t;
			loc_prev_t = loc_cur_t;
			loc_first_time = loc_cur_t;
			
			break;

		case NMS_STATUS_PLAYER_SF:
			if (loc_preState != NMS_STATUS_PLAYER_SF)
			{
				LOCK_PLAYMUTEX();
				loc_cur_t = playtime;
				OutputFlush(loc_cur_t);
				if (loc_preState == NMS_STATUS_PLAYER_PAUSE)
					OutputPause(0);
				if (!muted)
				{
					muted = 1;
					OutputMute(muted);
				}
				loc_cur_t = playtime;
				UNLOCK_PLAYMUTEX();

				loc_next_t = loc_cur_t;
				loc_prev_t = loc_cur_t;
				loc_preState = NMS_STATUS_PLAYER_SF;				
			}
			break;

		case NMS_STATUS_PLAYER_STOPPED:
			if (loc_preState == NMS_STATUS_PLAYER_PAUSE)
				OutputPause(0);
			loc_preState = NMS_STATUS_PLAYER_STOPPED;
			loc_quit = 1;
			goto bail;

		default:
			LOCK_PLAYMUTEX();
			playState = loc_preState;
			UNLOCK_PLAYMUTEX();
			break;
		}

		LOCK_PLAYMUTEX();
		if (rptState == NMS_PLAYBACK_REPEAT_ON)
		{
			loc_cur_t = playtime;
			if (loc_cur_t > rptB)
			{				
				loc_cur_t = InputSeek(rptA);
				OutputFlush(loc_cur_t);
			}
		}
		UNLOCK_PLAYMUTEX();
		
		if ( 1 == OutputGetBuffer(&loc_buf, 1000, 0))
		{
			//DBGMSG("buffer full or playback paused!");
			continue;
		}

		loc_bytes = InputGetData(&loc_buf);
		if (loc_bytes == 0)
		{
			WPRINT("zero bytes returned!");
			continue;
		}

		LOCK_PLAYMUTEX();
		if (loc_bytes < 0) 
		{
			if(editmode)
			{
				UNLOCK_PLAYMUTEX();
				continue;	
			}
			else
			{
				WPRINT("EOF reached!");
				// hit EOF, set flag to flush.
				loc_quit = 0;
				UNLOCK_PLAYMUTEX();
				break;
			}
		}
		UNLOCK_PLAYMUTEX();

#ifdef LOG_TIME_STAMP__
		if (loc_buf.curbuf == &loc_buf.abuf)
			DBGLOG("   aT = %d\n", loc_buf.curbuf->tsms);
		else
			DBGLOG("---vT = %d\n", loc_buf.curbuf->tsms);
#endif

		LOCK_PLAYMUTEX();
		playtime = OutputGetPlaytime();
		if (loc_buf.curbuf == &loc_buf.abuf)
		{
			if (loc_preState != NMS_STATUS_PLAYER_PLAY && rptState != NMS_PLAYBACK_REPEAT_ON)
			{
				UNLOCK_PLAYMUTEX();
				continue;
			}
		}
		UNLOCK_PLAYMUTEX();

		if (loc_preState != NMS_STATUS_PLAYER_PLAY && loc_preState != NMS_STATUS_PLAYER_PAUSE)
		{
			if (loc_first_time == 0)
			{
				loc_first_time = loc_cur_t;
			}
			if (loc_preState == NMS_STATUS_PLAYER_SF)
			{
				loc_buf.vbuf.tsms = loc_buf.vbuf.tsms - loc_first_time;
				LOCK_PLAYMUTEX();
				loc_buf.vbuf.tsms *= sfrwLevel;
				UNLOCK_PLAYMUTEX();
				loc_buf.vbuf.tsms += loc_first_time;
			}		
			loc_prev_t = loc_cur_t;
		}
		
		OutputWrite(&loc_buf);
	}
	
	DBGMSG("Exit main playback loop.\n");
	// if quit, re-fetch the lock here
	
 bail:
	LOCK_PLAYMUTEX();
	ffrwLevel = 0;
	sfrwLevel = 0;
	UNLOCK_PLAYMUTEX();
	
	if (loc_preState == NMS_STATUS_PLAYER_PAUSE)
	{
		OutputPause(0);
	}

	LOCK_PLAYMUTEX();
	if (muted)
	{
		muted = 0;
		OutputMute(muted);
	}
	UNLOCK_PLAYMUTEX();
	
	// We used to call InputFinish here, but let's wait a while, because we may be asked to seek again later,
	// while draining the output buffer. And we'll need input to be active if we want to have something to seek into.
	
	if (loc_quit)
	{
		LOCK_PLAYMUTEX();
		InputFinish();
		// If we're quitting the server, we really don't want to wait that video output is drained.
		// We flush it down the toilet without sending it to the TV.
		OutputFinish(0);
		UNLOCK_PLAYMUTEX();
	}
	else
	{
		unsigned long remain;
		
		// We're at the EOF of a playback. We may have some data left in the dm320
		// gargantuan output buffers, and we need to properly make sure it goes all to TV before we're done.
		//
		// We can't really call a blocking flush here (like OutputFinish(1)).
		// This is because the server will freeze otherwise and be unable to process 
		// commands (like STOP or PAUSE) that come in during the final draining of the output buffer.
		//
		// This is an improvement over the old way of just waiting some time before calling the 
		// blocking flush: it instead keeps checking the amount of data in the dm320 video output buffer 
		// and doesn't call the blocking flush until it's all drained. It sleeps a small amount of time 
		// between checks, then before the next check it processes commands.
		//
		// HACK:
		// It works better than before, however it's still a bit of an hack, since this
		// polling is really not elegant at all. It is much better to setup a callback and 
		// have imedia call us when there's no more data in the output buffer. 
		// Or something like that. --nero

		LOCK_PLAYMUTEX();
		playing = 1; //Restore the playback flag, since we're still actually playing.
		UNLOCK_PLAYMUTEX();

		remain = OutputGetBufferedSize();
		WPRINT("Entering buffer drain loop (remain %lu bytes).\n", remain);
		
		while (remain > 0)
		{
			
			LOCK_PLAYMUTEX();
			if (!playing) 
			{
				UNLOCK_PLAYMUTEX();
				break;
			}
			
			DBGMSG("Draining output buffer. Bytes remaining: %lu\n", remain);
			if (playState == NMS_STATUS_PLAYER_PAUSE) OutputPause(1);
			else 
			{
				OutputPause(0);
				
				// If we're asked to seek (ffw, rew or point-seek), let's just return in the main loop.
				// The logic in the loop will take care of everything else, we don't need to do anything more here.
				if (playState == NMS_STATUS_PLAYER_FF || playState == NMS_STATUS_PLAYER_RW ||
					(playState == NMS_STATUS_PLAYER_PLAY && iSeekFlag != 0)) 
				{
					DBGMSG("Going back to main playback loop.\n");
					UNLOCK_PLAYMUTEX();
					goto main_play_loop;
				}
			}
			UNLOCK_PLAYMUTEX();

			usleep(DRAIN_POLL_TICK);
			remain = OutputGetBufferedSize();
		}
		DBGMSG("Exited drain loop. Playing: %d - Remain: %lu\n", playing, remain);
		
		LOCK_PLAYMUTEX();
		InputFinish();
		OutputPause(0); //before finish output must make sure pause flag is cleared
		
		//Let's always call a final non-blocking flush. Thus if there are bugs in the above logic, 
		//we notice a clip at end of video (due to flushing to kingdom come the last part of data), 
		//and hopefully we come here to look ;) --nero
		OutputFinish(0);
		UNLOCK_PLAYMUTEX();
	}

	// clear playing and ffrw flag, and release lock before
	// joining ffrw thread.
	LOCK_PLAYMUTEX();
	playing = 0;
	
	rptState = NMS_PLAYBACK_REPEAT_OFF;	
	if (playtype == NPT_FILE)
		playState = NMS_STATUS_PLAYER_STOPPED;
	else
		playState = NMS_STATUS_PLAYER_NEXT;	
	
	DBGMSG("input output finished.");
	
	// check to play next file.
	if ((0 == loc_quit) && (playtype != NPT_FILE))
	{
		DBGMSG("searching for next file.");
		//if repeat is single and the file is not played, stop nextfile loop
		if (!((repeat == RM_REPEAT) && (errorStatus == NMS_STATUS_NOT_PLAYABLE)))
			trackChange = TC_NORMAL;
		
		UNLOCK_PLAYMUTEX();
		pthread_cond_broadcast(&nextFileCond);
	}
	else UNLOCK_PLAYMUTEX();

	pthread_exit(NULL);
}

// make sure playmutex is released before calling this
static void stopPlaying( void )
{
	LOCK_PLAYMUTEX();
	if (!requestToStopPlaying)
	{
		requestToStopPlaying = 1;
		playtime = 0;
		playing = 0;
		preloaded = 0;
		rptA = 0;
		rptB = 0;
	}
	else 
	{
		// someone else has already requested to stopPlaying and not done yet,
		// we bail to let that one finish first.
		UNLOCK_PLAYMUTEX();
		return;
	}
	UNLOCK_PLAYMUTEX();
	
	if (avThread)
	{
		DBGMSG("Removing av thread!");
		pthread_join(avThread, NULL);
		avThread = (pthread_t)NULL;
		//DBGMSG("Thread removed!");
	}

	LOCK_PLAYMUTEX();
	requestToStopPlaying = 0;
	UNLOCK_PLAYMUTEX();

	DBGMSG("playing is completely stopped.");
}

static void
stopServer( void )
{
	DBGMSG("stopping playback server");
	
	LOCK_PLAYMUTEX();
	going = 0;		
	preloaded = 1;
	// quit the nextfile thread first.
	trackChange = TC_NORMAL;
	UNLOCK_PLAYMUTEX();
	
	pthread_cond_broadcast(&nextFileCond);
	
	if (nextFileThread)
	{
		DBGMSG("Removing Next file thread!");
		pthread_join(nextFileThread, NULL);
		nextFileThread = (pthread_t)NULL;
		DBGMSG("Next file Thread removed!");
	}

	if (dirInitThread)
	{
		DBGMSG("Removing Init thread!");
		pthread_join(dirInitThread, NULL);
		dirInitThread = (pthread_t)NULL;
		DBGMSG("Init Thread removed!");
	}

	LOCK_PLAYMUTEX();
	dirInited = 0;
	UNLOCK_PLAYMUTEX();
	
	stopPlaying();
	
	LOCK_PLAYMUTEX();
	trackChange = TC_DISABLE;
	UNLOCK_PLAYMUTEX();
}

static int playFile(const char * file)
{
	int status = -1;
	media_desc_t mdesc;
	
	
	//DBGLOG("playing file: [%s]", file);
	memset(&mdesc, 0, sizeof(media_desc_t));
	mdesc.ftype = NMS_WP_INVALID;
	
	if (!InputIsOurFile(file)) return -1;
	
	status = InputInit(file, &mdesc); 
	
	switch (status) 
	{
	case 0: //everything is fine
		break; 
	case 1:
		WPRINT("InputInit: DM320 is locked by something else. Please try again later.");
		LOCK_PLAYMUTEX();
		errorStatus = NMS_STATUS_OUTPUT_LOCKED;
		UNLOCK_PLAYMUTEX();
		goto bail_clean_input;
	default:
		WPRINT("InputInit: Unable to init device!");
		LOCK_PLAYMUTEX();
		errorStatus = NMS_STATUS_NOT_PLAYABLE;
		UNLOCK_PLAYMUTEX();
		goto bail_clean_input;
	}

	LOCK_PLAYMUTEX();
	InputGetInfo(file, &info);
	UNLOCK_PLAYMUTEX();


	if (InputStart(file)) 
	{
		status = -1;
		goto bail_clean_input;
	}

	status = OutputInit(&mdesc,OutputGetMode(),curProportions); 
	switch (status)
	{
	case 0: //everything is fine
		break;
	case 1:
		WPRINT("OutputInit: DM320 is locked by something else. Please try again later.");
		LOCK_PLAYMUTEX();
		errorStatus = NMS_STATUS_OUTPUT_LOCKED;
		UNLOCK_PLAYMUTEX();
		goto bail_clean_input;
	default:
		WPRINT("OutputInit: Unable to init output !");
		LOCK_PLAYMUTEX();
		if ((playtype == NPT_FILE) || (totalFiles <= 1) || (repeat == 1))
			errorStatus = NMS_STATUS_NOT_PLAYABLE;
		UNLOCK_PLAYMUTEX();
		goto bail_clean_input;
	} 

	OutputActivateMode(0);

	LOCK_PLAYMUTEX();
	playtime = 0;
	ffrwLevel = 0;
	sfrwLevel = 0;
	sfrwLevelFinal = 0;
	muted = 0;
	playing  = 1;
	trackChange = TC_DISABLE;
	UNLOCK_PLAYMUTEX();
	
	if (newThread(&avThread, NULL, avLoop, NULL)) 
	{
		status = -1;
		goto bail_clean_input;
	}
	
	return 0;
	
bail_clean_input:
	LOCK_PLAYMUTEX();
	InputFinish();
	UNLOCK_PLAYMUTEX();
	return status;
}

// initialize directory.
// arg: original directory/file name
// function shall set up the following,
// 1. filter the directory properly
// 2. total number of playable contents:  totalFiles
// 3. current file index:                 fileIdx
static void * dirInit( void * arg )
{
	LOCK_PLAYMUTEX();	
	if (CoolBlockOpenDir(dirName, &node) < 0)
	{
		WPRINT("unable to open directory.");
	}
	else
	{
		//DBGMSG("dirPlayAV = [%d]", dirPlayAV);
		if (dirPlayAV == DPAV_AUDIO_)
		{
			CoolFilterDirectory(&node, CoolIsAudioFile, DF_ALL);
		}
		else if (dirPlayAV == DPAV_VIDEO_)
		{
			CoolFilterDirectory(&node, CoolIsVideoFile, DF_ALL);
		}
		else if (dirPlayAV == DPAV_NOT_DETERMINED_)
		{
#ifdef PLAY_VIDEO_FILE_ONLY
			CoolFilterDirectory(&node, CoolIsVideoFile, DF_ALL);
			dirPlayAV = DPAV_VIDEO_;
#else
			if (0 == CoolFilterDirectory(&node, CoolIsVideoFile, DF_ALL))
			{
				CoolFilterDirectory(&node, CoolIsAudioFile, DF_ALL);
				dirPlayAV = DPAV_AUDIO_;
			}
			else dirPlayAV = DPAV_VIDEO_;
#endif
		}
	}

	// if is playing, get index of currently playback file
	totalFiles = node.fdnum+node.ffnum;

	if (playing)
	{
		int fi;
		char path[PATH_MAX];
		
		fileIdx = 0;
		fi = totalFiles;

		while(fi--)
		{
			if (!strcmp(trackName,CoolCatFilename2Path(path, PATH_MAX, D_PATH, D_NAME(F_INDEX(fi)))))
			{
				//DBGMSG("fi = %d", fi);
				fileIdx = fi;
				break;
			}
			//DBGMSG("fname = [%s]", trackName);
			//DBGMSG("path = [%s]", path);
		}
	}
	
	// done with directory init, broadcast.
	DBGMSG("done with directory initialization.");
	dirInited = 1;
	UNLOCK_PLAYMUTEX();

	pthread_cond_broadcast(&playdirCond);
	pthread_exit(NULL);
}

static char * nextFileFromDir(int idx,char * pathbuf,const int bufsize)
{
	char * fname = NULL;

	LOCK_PLAYMUTEX();
	if ((idx < totalFiles) && (idx >= 0))
	{
		//fetch filename based on index.
		fname = CoolCatFilename2Path(pathbuf, bufsize, D_PATH, D_NAME(F_INDEX(idx)));
		fname = pathbuf;
		//DBGMSG("file name = [%s]", fname);
	}
	UNLOCK_PLAYMUTEX();
	return fname;
}

static int getNewIndex()
{
	int newindex;

	LOCK_PLAYMUTEX();
	newindex = fileIdx;

	if (repeat == RM_REPEAT) goto bail;
	
	if (repeat == RM_SHUFFLE) // repeat once
	{
		newindex = rand()%totalFiles;
		goto bail;
	}
	
	if (repeat == RM_NORMAL)
	{
		if(playmode == PM_SINGLE)
		{
			newindex = -1;
			goto bail;
		}

		if(trackChange == TC_NORMAL) 
			newindex += 1;
		else if(trackChange == TC_NEXT) 
			newindex += 1;
		else if(trackChange == TC_PREVIOUS) 
			newindex -= 1;
			
		if (newindex >= totalFiles) 
			newindex = 0;
		if (newindex < 0) 
			newindex = totalFiles-1;
		if (trackChange == TC_NEXT || trackChange == TC_PREVIOUS)
		{
			firstplay = newindex;
			goto bail;
		}
		if (newindex == firstplay)
		{
			newindex = -1;
			goto bail;
		}
	}

 bail:
	UNLOCK_PLAYMUTEX();

	return newindex;
}

// thread to automatically fetch next file to play.
static void * nextFileLoop(void * arg)
{
    char * fname = NULL;
	char path[PATH_MAX];
 	
	// wait till directory is initialized.
	LOCK_PLAYMUTEX();
	while (!dirInited) 
		pthread_cond_wait(&playdirCond, &playMutex);
	firstplay = fileIdx;
	UNLOCK_PLAYMUTEX();

	srand((int)time(0));

	while(1)
	{
		LOCK_PLAYMUTEX();
		if (!going)
		{
			UNLOCK_PLAYMUTEX();
			break;
		}
		UNLOCK_PLAYMUTEX();

		LOCK_PLAYMUTEX();
		while (trackChange == TC_DISABLE) 
			pthread_cond_wait(&nextFileCond, &playMutex);

		if (!going)
		{
			UNLOCK_PLAYMUTEX();
			break;
		}
		UNLOCK_PLAYMUTEX();

		int index =  getNewIndex();
		if(index < 0)
		{ 
			stopPlaying();
			break;
		}		

		LOCK_PLAYMUTEX();
		fileIdx = index;
		UNLOCK_PLAYMUTEX();
		
		fname = nextFileFromDir(index,path,PATH_MAX);		
		DBGMSG("fname : %s\n", fname);
		if (fname)
		{
			int ret;
			stopPlaying();
			ret = playFile(fname);
			if (ret == 0)
			{
				LOCK_PLAYMUTEX();
				trackChange = TC_DISABLE;
				UNLOCK_PLAYMUTEX();
			}
			else if (ret == 1) break; //if we get locked output, no point in keeping trying other files

			LOCK_PLAYMUTEX();
			fileCnt++;
			UNLOCK_PLAYMUTEX();
		}
		else break;
	}

	LOCK_PLAYMUTEX();
	going = 0;
	if (errorStatus == NMS_STATUS_OK)
		// player has been stopped by itself, not an error status.
		playState = NMS_STATUS_PLAYER_STOPPED;
	UNLOCK_PLAYMUTEX();
	
	
	pthread_exit(NULL);
}

/**
 * Play back specified file.
 *
 * @param file
 *        file path.
 * @return
 *        0 if file playback started, 1 if output was locked, other values on errors.
 */
int
SrvPlayFile( const char * file )
{
	int status;
	
	stopServer();
	
	LOCK_PLAYMUTEX();
	strcpy(trackName, file);
	playtype = NPT_FILE;
	going = 1;
	everPlayed = 0;
	UNLOCK_PLAYMUTEX();

	status = playFile(file);
	if (status) stopPlaying();
	else
	{
		LOCK_PLAYMUTEX();
		playState = NMS_STATUS_PLAYER_PLAY; 
		errorStatus = NMS_STATUS_OK;
		UNLOCK_PLAYMUTEX();
	}
	return status;
}

/**
 * Play back specified directory.
 * If file is specified, start playback that file.
 * If file is not specified, choose first file based on playmode.
 * Following playback only plays either video or audio, that is, playback
 * video only if first playback is video and playback audio only if first
 * playback is audio.
 *
 * @param dir
 *        directory path.
 * @return
 *        0 if directory playback started, in other words, at lease one
 *        playable file is found, otherwise nonzero.
 *        1 if any of the files found that output was locked when trying to play.
 */
int
SrvPlayDir( const char * dir )
{
	int status = -1;
	struct stat st;
	char * fname;
	char path[PATH_MAX];
	char loc_dirName[PATH_MAX];
	int isDir = 0;
	int loc_repeat;
	
	DBGMSG("play directory: [%s]", dir);
	stopServer();

	strcpy(loc_dirName, dir);

	LOCK_PLAYMUTEX();
	strcpy(dirName, dir);
	strcpy(trackName, dir);
	everPlayed = 0;
	dirInited = 0;
	going = 1;
	fileCnt = 1;
	playtype = NPT_DIR;
	dirPlayAV = DPAV_NOT_DETERMINED_;
	UNLOCK_PLAYMUTEX();

	// if dir corresponds to a file, start to play file immediately.
	// thus to remove the long startup delay.
	if ( 0 == stat(loc_dirName, &st) ) 
	{
		if ( S_ISDIR(st.st_mode) ) 
		{  
			isDir = 1;
			// it is a directory, continue
			//DBGMSG("directory, continue", dir);
		}
		else
		{
			char loc_trackName[PATH_MAX];

			LOCK_PLAYMUTEX();
			strcpy(loc_trackName, trackName);
			UNLOCK_PLAYMUTEX();

			isDir = 0;

			// it is a file, try to play it first.
			status = playFile(loc_trackName);
			
			if (0 == status) 
			{
				LOCK_PLAYMUTEX();
				dirPlayAV = CoolIsAudioFile(loc_trackName)? 1 : 2;
				UNLOCK_PLAYMUTEX();
			}

			// get the directory name
			{
				int len;
				char * p;
				
				LOCK_PLAYMUTEX();
				len = strlen(dirName);
				p = dirName+len;
				while(*p != '/') p--;
				*p = 0;
				UNLOCK_PLAYMUTEX();
			}
		}
	} 
	else 
	{ 
		WPRINT("Directory status not available!");	
		return -1;
	}
	
	LOCK_PLAYMUTEX();
	strcpy(loc_dirName, dirName);
	UNLOCK_PLAYMUTEX();

	if (stat(loc_dirName, &st) == -1)
	{
		LOCK_PLAYMUTEX();		
		errorStatus = NMS_STATUS_NOT_PLAYABLE;
		UNLOCK_PLAYMUTEX();
		return -1;
	}

	if (newThread(&nextFileThread, NULL, nextFileLoop, NULL))
	{
		return -1;
	}

	//DBGMSG("create directory init thread", dir);
	// Create thread to init directory.
	if (newThread(&dirInitThread, NULL, dirInit, NULL))
	{
		stopServer();
		return -1;
	}

	// file played?
	// If not, wait to get first file played, because we have to return
	// a status here to indicate whether the playback can be successfully 
	// started or not.
	if (status)
	{
		LOCK_PLAYMUTEX();
		loc_repeat = repeat;
		UNLOCK_PLAYMUTEX();
		
		// not single repeat or trackName is directory, wait to get first file played 
		if (loc_repeat != RM_REPEAT || isDir) 
		{
			int cnt; // counter of retry
			int idx = 0;

			//DBGMSG("playback not started yet", dir);
			// NO, wait till directory is initialized.
			LOCK_PLAYMUTEX();
			while (!dirInited) 
				pthread_cond_wait(&playdirCond, &playMutex);
			
			// Now try to get next file and play it.
			fileIdx = 0;
			cnt = totalFiles;
			UNLOCK_PLAYMUTEX();

			// try each file at most once.
			while(cnt--)
			{
				fname = nextFileFromDir(idx,path,PATH_MAX);
				if (!fname) break;
				
				status = playFile(fname);
				
				//DBGMSG("status = [%d]", status);
				if (!status) break;
				else
				{
					LOCK_PLAYMUTEX();
					idx++;
					fileIdx++;
					UNLOCK_PLAYMUTEX();
				}
			} 
			if (cnt <= 0)
			{
				LOCK_PLAYMUTEX();
				errorStatus = NMS_STATUS_NOT_PLAYABLE;
				UNLOCK_PLAYMUTEX();
			}
		}
	}
	
	// Stop server to clean up if not able to play.
	if (status)	stopServer();
	else
	{
		LOCK_PLAYMUTEX();
		playState = NMS_STATUS_PLAYER_PLAY; 
		errorStatus = NMS_STATUS_OK;
		UNLOCK_PLAYMUTEX();
	}

	return status;
}


/** play history .*/
int SrvPlayHistory(void)
{
	int status = -1;
	play_history_t his;
	
	stopServer();
	
	LOCK_PLAYMUTEX();	
	playState = NMS_STATUS_PLAYER_PLAY; 
	UNLOCK_PLAYMUTEX();
	
	if (!GetPlayHistory(&his))
	{
		int loc_playtype = playtype;
		
		LOCK_PLAYMUTEX();	
		loc_playtype =
			playtype = his.type;
		UNLOCK_PLAYMUTEX();	
		
		switch (loc_playtype)
		{
		case NPT_FILE: status = SrvPlayFile(his.path); break;
		case NPT_DIR:  status = SrvPlayDir(his.path); break;
		default: break;
		}
	}
	
	if (!status)
	{
		//DBGMSG("playing history data-type:[%d] idx:[%d]"
		//"mark: [%d] path:[%s]", 
		//his.type, his.fileIdx, his.mark, his.path); 
		SrvSeek(his.mark);
	}
	else
		// No history available, or history is unplayable
		// play the first available removable media.
	{
#if 0	  
		int * st;
		int num;
		
		DBGMSG("history not available, play from first storage.");
		// fetch available storage.
		num = CoolStorageAvailable(&st);
		if (num <= 1) return -1;
		
		num--;
		// now search to play available storage.
		do 
		{
			st++; 
			status = SrvPlayDir((char*)StoragePath(*st));
			if (!status) break;
		} while (--num);
#endif	
	}
	
	if (!status) 
	{
		LOCK_PLAYMUTEX();
		errorStatus = NMS_STATUS_OK;
		UNLOCK_PLAYMUTEX();
	}
	
	return status;
}

/**
 * Pause/unpause current playback.
 */
void
SrvPauseUnpause( void )
{
	LOCK_PLAYMUTEX();
	if (playing)
	{
		frameByFrame = 0;
		if (playState == NMS_STATUS_PLAYER_PLAY)
			playState = NMS_STATUS_PLAYER_PAUSE;
		else if (playState == NMS_STATUS_PLAYER_PAUSE)
			playState = NMS_STATUS_PLAYER_PLAY;
		else 
		{
			if (playedOrPaused == NMS_STATUS_PLAYER_PLAY)
			{
				playState = NMS_STATUS_PLAYER_PLAY;
				sfrwLevelFinal = sfrwLevel;
				ffrwLevel = 0;
				sfrwLevel = 0;
			}
			else
				playState = NMS_STATUS_PLAYER_PAUSE;
		}		
	}	
	UNLOCK_PLAYMUTEX();
}

/**
 * Stop current playback.
 */
void
SrvStop( void )
{
	int loc_going;
	int loc_fileIdx;
	int loc_everPlayed;
	int loc_playtype;

	LOCK_PLAYMUTEX();
	loc_going = going;
	loc_fileIdx = fileIdx;
	loc_playtype = playtype;
	UNLOCK_PLAYMUTEX();

	if (loc_going)
	  // if playing, set history before shutdown.
	{

		char * path = NULL;
		char buf[PATH_MAX];
		DBGMSG("Stop server playback.");
		switch (loc_playtype)
		{
		case NPT_FILE:
			LOCK_PLAYMUTEX();
			path = strcpy(buf, trackName); 
			UNLOCK_PLAYMUTEX();
			break;
		case NPT_DIR:
			path = nextFileFromDir(loc_fileIdx,buf,PATH_MAX); break;
		default: //history not supported.
			WPRINT("play history not supported.");
			break;
		}

		LOCK_PLAYMUTEX();
		loc_fileIdx = fileIdx;
		loc_everPlayed = everPlayed;
		UNLOCK_PLAYMUTEX();

		if (path)
		{
			int mark;

			LOCK_PLAYMUTEX();			
			mark = playtime;
			UNLOCK_PLAYMUTEX();

			if (loc_everPlayed) SetPlayHistory(loc_playtype, loc_fileIdx, mark, path);
		}
	}	

	stopServer();

	LOCK_PLAYMUTEX();
	// player has been stop upon user request.
	errorStatus = NMS_STATUS_PLAYER_STOPPED;
	UNLOCK_PLAYMUTEX();
}

/**
 * Seek in current playback.
 *
 * @param t
 *        time stamp in mili-seconds.
 * @return
 *        actual time stamp if successful, otherwise negative value.
 */
int
SrvSeek( int t )
{
	int ret_t = 0;	

	input_capability_t cap;
	InputGetCapability(&cap);
	if (!cap.can_fwd || !cap.can_rwd) goto bail;

	LOCK_PLAYMUTEX();
	if (playing)
	{
		ffrwLevel = 0;
		sfrwLevel = 0;
		iSeekFlag = 1;
		iBookMark = t;
		playState = NMS_STATUS_PLAYER_PLAY;	
	}
	UNLOCK_PLAYMUTEX();

 bail:
	return ret_t;
}

/** 
 * Fast forward/rewind playback , Frame-by-Frame
 * 
 * Fast forward/ rewind playback
 * @param level
 *         scan level.
 */
void
SrvFfRw( int level )
{
	input_capability_t cap;
	InputGetCapability(&cap);

	if ((!cap.can_fwd && level > 0) || (!cap.can_rwd && level < 0)) goto bail;

	LOCK_PLAYMUTEX();
	if (playing)
	{
		sfrwLevel = 0;
		frameByFrame = 0;
		iSeekFlag = 0;
		ffrwLevel = level;
		if (ffrwLevel == 0)
			playState = NMS_STATUS_PLAYER_PLAY;
		else if (ffrwLevel > 0)
			playState = NMS_STATUS_PLAYER_FF;
		else if (ffrwLevel < 0)
			playState = NMS_STATUS_PLAYER_RW;	
	}
	UNLOCK_PLAYMUTEX();

 bail:
	return;
}

/** 
 * Slow forward/rewind playback.
 *
 * @param level
 *         scan level.
 */
void
SrvSfRw( int level )
{
	input_capability_t cap;

	InputGetCapability(&cap);
	if ((!cap.can_fwd && level > 0) || (!cap.can_rwd && level < 0)) goto bail;

	LOCK_PLAYMUTEX();
	if (playing)
	{
		ffrwLevel = 0;
		iSeekFlag = 0;
		frameByFrame = 0;
		sfrwLevelFinal = 0;
		if (level == 0)
			sfrwLevelFinal = sfrwLevel;
		sfrwLevel = level;
		playState = NMS_STATUS_PLAYER_SF;
		if (sfrwLevel == 0)	
			playState = NMS_STATUS_PLAYER_PLAY;		
	}
	UNLOCK_PLAYMUTEX();

 bail:
	return;
}

/**
 * Frame-by-Frame playback
 * @param direction,
 *	direction == -1, rewind a frame,
 *	direction == 1,  forward a frame,
 *	direction == 0, disable Frame-by-Frame
 * @return
 *	0
 */
int
SrvFrameByFrame(int direction)
{
	LOCK_PLAYMUTEX();
	if (playing)
	{
		if (direction == 0)
		{
			frameByFrame = 0;
			playState = NMS_STATUS_PLAYER_PLAY;
		}
		else
		{
			playState = NMS_STATUS_PLAYER_VF;
			frameByFrame = 1;
			if (direction == -1)
			{
				// TODO: rewind a frame		
			}
			else if (direction == 1)
			{
				// TODO: forward a frame
			}
		}
	}
	UNLOCK_PLAYMUTEX();
	return 0;
}

/**
 * Repeat A-B
 *	if A and B both are set value, and B - A > 5 seconds, start Repeat A-B
 * @param ab,
 *	ab == -1,  set A = current play time
 *    ab == 1, 	   set B = current play time
 *	ab == 0,	   disable Repeat A-B and set A = 0, B = 0
 * @return
 *	0
 */
int
SrvRepeatAB(int ab)
{
	LOCK_PLAYMUTEX();
	if (playing)
	{
		if (ab == 0)
			rptState = NMS_PLAYBACK_REPEAT_OFF;
		else
		{
			if (ab == -1)
			{
				rptA = playtime;
				rptState = NMS_PLAYBACK_REPEAT_A;
			}
			else if (ab == 1)
			{
				rptB = playtime;
				if (rptB - rptA > 5 * 1000)
				{
					rptState = NMS_PLAYBACK_REPEAT_ON;
				}
				else
				{
					rptState = NMS_PLAYBACK_REPEAT_OFF;
				}
			}
		}
	}
	UNLOCK_PLAYMUTEX();
	return 0;
}

/** 
 * Track change playback.
 *
 * @param track
 *        -1: previous track
 *         0: restart current track.
 *         1: next track.
 * @return
 *        0 if track change successful, 1 if output locked, other values mean errors.
 *
 */
int
SrvTrackChange( int track )
{
	int ret = 0;
	int loc_going;
	int loc_playing;
	int loc_playtype;
	int loc_playtime;
	char loc_trackName[PATH_MAX];

	LOCK_PLAYMUTEX();
	loc_going = going;
	loc_playing = playing;
	loc_playtype = playtype;
	strcpy(loc_trackName, trackName);
	UNLOCK_PLAYMUTEX();

	if (NPT_FILE == loc_playtype) 
	{
		if (loc_playing) SrvSeek(0);
		else return	SrvPlayFile(loc_trackName);
	}
	else if (NPT_DIR == loc_playtype)
	{
		if (!loc_going) return SrvPlayDir(loc_trackName);
		switch (track)
		{
		case 1:
			LOCK_PLAYMUTEX();	
			trackChange = TC_NEXT;
			UNLOCK_PLAYMUTEX();
			pthread_cond_broadcast(&nextFileCond);
			break;
		case 0:
			SrvSeek(0); 
			break;
		case -1:  //previous
			LOCK_PLAYMUTEX();
			loc_playtime = playtime;
			UNLOCK_PLAYMUTEX();

			if( loc_playtime > 5 * 1000)
			{
				SrvSeek(0);
				ret = 1;
			}
			else
			{
				LOCK_PLAYMUTEX();
				trackChange = TC_PREVIOUS;
				UNLOCK_PLAYMUTEX();
				pthread_cond_broadcast(&nextFileCond);
			}
			break;
		}
	}
	return ret;
}

/**
 * Get current volume.
 *
 * @param left
 *        left channel volume.
 * @param right
 *        right channel volume.
 */
void
SrvGetVolume( int * left, int * right )
{
	OutputGetVolume(left, right);
}

/**
 * Set current volume.
 *
 * @param left
 *        left channel volume.
 * @param right
 *        right channel volume.
 */
void
SrvSetVolume( int left, int right )
{
	OutputSetVolume(left, right);
}

/**
 * Get current playback time stamp.
 *
 * @return
 *         playback time stamp in miliseconds.
 */
int
SrvGetPlaytime( void )
{
	int loc_playtime;

	LOCK_PLAYMUTEX();
	loc_playtime = OutputGetPlaytime();
	UNLOCK_PLAYMUTEX();

	return loc_playtime;
}

/**
 * Get the media infomation
 *
 * @param filename
 *        input file name
 * @param minfo
 *        media info buffer.
 * @return 
 *        return 0 if successful, nonzero otherwise.
 */
int    
SrvGetMediaInfo(const char *filename, void * minfo )
{
	int rlt;

	// is our format, info always available.
	((media_info_t *)minfo)->available = 1;

	LOCK_PLAYMUTEX();
	rlt = InputGetInfo( filename, minfo );
	UNLOCK_PLAYMUTEX();

	return rlt;	
}


/**
 * Tell if server is playing.
 *
 * @return
 *         1 if server is still playing, otherwise zero.
 */
int 
SrvIsPlaying( void )
{
	int loc_going;

	LOCK_PLAYMUTEX();
	loc_going = going;
	UNLOCK_PLAYMUTEX();

	return loc_going;
}


/**
 * Get current playmode.
 *
 * @return
 *        current playmode
 */
int
SrvGetPlaymode( void )
{
	int loc_playmode;

	LOCK_PLAYMUTEX();
	loc_playmode = playmode;
	UNLOCK_PLAYMUTEX();

    return loc_playmode;
}

/**
 * Set current playmode.
 *
 * @param mode
 *        playmode
 */
void
SrvSetPlaymode( int mode )
{
	LOCK_PLAYMUTEX();
    playmode =  mode;
	UNLOCK_PLAYMUTEX();
}

/**
 * Set edit mode.
 *
 * @param mode
 *      1  edit mode
 *      0  play mode
 */
void
SrvSetEditmode( int mode )
{
	LOCK_PLAYMUTEX();
    editmode =  mode;
	UNLOCK_PLAYMUTEX();
}


/**
 * Get repeat A-B status
 * @return
 *	REPEAT_OFF,
 *	REPEAT_ON,
 *	REPEAT_A,
 */
int
SrvGetRepeatABStatus(void)
{
	int loc_rptState;

	LOCK_PLAYMUTEX();
	loc_rptState = rptState;
	UNLOCK_PLAYMUTEX();

	return loc_rptState;
}

/**
 * Get current repeat mode.
 *
 * @return
 *        current repeat mode
 */
int
SrvGetRepeatmode( void )
{
	int loc_repeat;

	LOCK_PLAYMUTEX();
	loc_repeat = repeat;
	UNLOCK_PLAYMUTEX();

    return loc_repeat;
}

/**
 * Set current repeat mode.
 *
 * @param mode
 *        repeat mode
 */
void
SrvSetRepeatmode( int mode )
{
	LOCK_PLAYMUTEX();
    repeat = mode;	
	UNLOCK_PLAYMUTEX();
}

/**
 * Get total playable files.
 *
 */
int    
SrvGetTotalFiles(void)
{
	int loc_totalFiles;

	LOCK_PLAYMUTEX();
	loc_totalFiles = totalFiles;
	UNLOCK_PLAYMUTEX();

    return loc_totalFiles;
}

/**
 * Get current file index.
 *
 */
int    
SrvGetFileIndex(void)
{
	int idx = -1;
	int loc_going;
	int loc_playtype;

	LOCK_PLAYMUTEX();
	loc_going = going;
	loc_playtype = playtype;
	UNLOCK_PLAYMUTEX();

	if (loc_going)
	{
		switch (loc_playtype)
		{
		case NPT_FILE: idx = 0; break;
		case NPT_DIR: 
			{
				LOCK_PLAYMUTEX();				
				if (dirInited) idx = fileIdx;
				UNLOCK_PLAYMUTEX();
			}
			break;
		default: break;
		}
	}

	return idx;
}

/**
 * Get file path.
 *
 */
int    
SrvGetFilePath(int idx, void * pathbuf,const int bufsize)
{
	int ret = -1;
	char * pc = (char*)pathbuf;
	int loc_going;
	int loc_playtype;

	LOCK_PLAYMUTEX();
	loc_going = going;
	loc_playtype = playtype;
	UNLOCK_PLAYMUTEX();
	
	DBGMSG("get file path.");
	
	*pc = 0;
	if (loc_going)
	{
		if (loc_playtype == NPT_FILE)
		{
			LOCK_PLAYMUTEX();			
			strcpy(pc, trackName);
			UNLOCK_PLAYMUTEX();
			ret = 0;
		}
		else if (loc_playtype == NPT_DIR)
		{
			if (idx >= 0)
			{
				nextFileFromDir(idx,pc,bufsize);
				//DBGMSG("filepath = [%s]", path);
				ret = 0;
			}
		}
		else 
		{
			//FIXME:
			WPRINT("unknown play type.");
		}
	}
	return ret;
}

/**
 * get ffrwLevel
 */
int 
SrvGetFFRWLevel(void)
{
	int loc_ffrwLevel;

	LOCK_PLAYMUTEX();
	loc_ffrwLevel = ffrwLevel;
	UNLOCK_PLAYMUTEX();

	return loc_ffrwLevel;
}

/**
 * get sfrwLevel
 */
int 
SrvGetSFRWLevel(void)
{
	int loc_sfrwLevel;

	LOCK_PLAYMUTEX();
	loc_sfrwLevel = sfrwLevel;
	UNLOCK_PLAYMUTEX();

	return sfrwLevel;
}

/**
 * get server status, currently only used in play side
 */
int 
SrvGetSrvStatus(void)
{
	int status;

	LOCK_PLAYMUTEX();
	if (errorStatus == NMS_STATUS_OK)
	{
		if(frameByFrame)
			status = NMS_STATUS_PLAYER_VF;
		else
			status = playState;
	}
	else
		status = errorStatus;
	UNLOCK_PLAYMUTEX();
	
	return status;
}

/**
 * Set output proportions.
 * This only set an internal variable, setting will be effective on next video that is played.
 *
 * @param proportions The output proportions. 0 for normal 4:3, 1 for widescreen 16:9
 */
void	SrvSetProportions(int proportions)
{
	curProportions = proportions;
}

/**
 * Get output proportions.
 * This return the currently set value, which may not match the proportions of current playing movie.
 * See @ref SetOutputProportions for more details.
 * 
 * @return The output proportions. 0 for normal 4:3, 1 for widescreen 16:9
 */
int	SrvGetProportions()
{
	return curProportions;
}
