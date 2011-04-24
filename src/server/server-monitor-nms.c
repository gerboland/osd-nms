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
 * Neuros-Cooler platform nms recording server module.
 *
 * REVISION:
 * 
 * 6) Changed monitor logic to improve stability ---------- 2008-01-03 nerochiaro
 * 5) Cleaned up mutex usage. ----------------------------- 2007-12-14 MG
 * 4) Added in background preference support. ------------- 2007-08-07 MG
 * 3) Start if not started and stop if not stopped. ------- 2006-07-31 MG
 * 2) Use new plugin interface.---------------------------- 2006-03-21 MG
 * 1) Initial creation. ----------------------------------- 2006-02-17 MG 
 *
 */

#include <pthread.h>
#include <signal.h>

#define OSD_DBG_MSG
#include "cooler-media.h"
#include "nc-err.h"
#include "cmd-nms.h"
#include "com-nms.h"
#include "server-nms.h"
#include "nmsplugin.h"
#include "plugin-internals.h"

// thread safe variables.
static int              monitor = 0;
static int              inNTSC_PAL = -1;
static int              outNTSC_PAL = -1;

// non-thread-safe variables
static int				mReady;

static pthread_t   		mThread 	= (pthread_t)NULL;
static pthread_mutex_t 	mMutex  	= PTHREAD_MUTEX_INITIALIZER;

#define LOCK_MMUTEX()  do {						\
		pthread_mutex_lock(&mMutex);			\
	}while(0)
#define UNLOCK_MMUTEX()  do{				\
		pthread_mutex_unlock(&mMutex);		\
	}while(0)

/* Handling monitor suppression requests */

static pthread_t        garbageThread     = (pthread_t)NULL;
static unsigned int     garbageThreadQuit = 0;

static pthread_cond_t   garbageCond   =  PTHREAD_COND_INITIALIZER;
static pthread_mutex_t  garbageMutex  = PTHREAD_MUTEX_INITIALIZER;
#define GARBAGE_CHECK_WAIT 2 //seconds (not sure if we shouldn't do it more frequently)

typedef struct {
	int pid;
	//NOTE: may add process cmd line here to protect in part against PID re-use (if it turns out to be an issue)
} suppression_request_t;
static dlist_t          *suppressionRequests = NULL;
static unsigned int     suppressionCount = 0;

static void * mLoop(void *arg)
{
	static media_buf_t        buf;
	while (1)
	{
		LOCK_MMUTEX();
		if (!mReady) 
		{
			UNLOCK_MMUTEX();
			break;
		}
		UNLOCK_MMUTEX();

		if (0 == EncInputGetBuffer(0, &buf, 100))
		{
			EncInputPutBuffer(0, &buf);
		}
		//We do NOT deal with audio now because,
		// 1. audio passthru is enabled automatically when video is enabled.
		// 2. if audio buffer is fetch/released here, we'll need first to 
		//    initialize the corresponding audio encoder.
	}
	pthread_exit(NULL);
}

static void stop_mthread(void)
{
	if (mThread)
	{
		LOCK_MMUTEX();
		mReady = 0;		
		UNLOCK_MMUTEX();

		DBGLOG("Removing Monitor thread.");

		pthread_join(mThread, NULL);
		mThread = (pthread_t)NULL;
		DBGLOG("Removed Monitor thread.");
	}
}

static void stop_srv(void)
{
	DBGLOG("Stopping monitor loop.");
	stop_mthread();
	EncInputFinish();
	monitor = 0;
}

static int start_srv(void)
{
	media_desc_t mdesc;
	int out_ntsc_pal;

	DBGLOG("Requested to start video monitor. (active: %d).\n", monitor);
	if ((!monitor) ||
		(monitor && (inNTSC_PAL != EncInputGetMode())) ||
		(monitor && (outNTSC_PAL != OutputGetMode())))
	{
		// stop monitor if monitor was started.
		if (monitor) {
			DBGLOG("Monitor was started, but input/output modes don't match anymore. Restarting it.\n");
			stop_srv();
		}
		else DBGLOG("Monitor was stopped, starting it.\n");

		memset(&mdesc, 0, sizeof(mdesc));

		// dont care, however set them to enable the loopback.
		mdesc.adesc.audio_type = NMS_AC_ARM_MP3;
		mdesc.vdesc.video_type = NMS_VC_MPEG4;
		out_ntsc_pal=OutputGetMode();
		mdesc.vdesc.width = TV_INPUT_WIDTH;
		if(!out_ntsc_pal)
			mdesc.vdesc.height = NTSC_TV_INPUT_HEIGHT;
		else
			mdesc.vdesc.height = PAL_TV_INPUT_HEIGHT;

		mdesc.vdesc.capture_port = EncInputGetMode();
		if(mdesc.vdesc.capture_port)
			mdesc.vdesc.frame_rate = PAL_FRAMERATE;    //pal
		else
			mdesc.vdesc.frame_rate = NTSC_FRAMERATE;	//ntsc
		mdesc.vdesc.bitrate = 2000;

		EncInputSelect(NMS_PLUGIN_MULTIMEDIA);
 		OutputSelect(NMS_PLUGIN_MULTIMEDIA);
		DBGLOG("Output activate mode: %d.\n", monitor);
		if (OutputActivateMode(1)) {
			return -1;
		}

		DBGLOG("Input initializing.\n");
		if (!EncInputInit(&mdesc, out_ntsc_pal))
		{
			EncInputStart();

			LOCK_MMUTEX();
			mReady = 1;	
			UNLOCK_MMUTEX();

			if (pthread_create(&mThread, NULL, mLoop, NULL))
			{
				ERRLOG("Monitor thread was not created!");
				EncInputFinish();
				return -1;
			}
			monitor = 1;
			inNTSC_PAL = EncInputGetMode();
			outNTSC_PAL = OutputGetMode();
		}
		else
		{
			WARNLOG("Unable to initialize video monitor.");
			inNTSC_PAL = -1;
			outNTSC_PAL = -1;
			return -1;
		}
	}

	DBGLOG("Video monitor started successfully.\n");
	return 0;
}

#define SIGNAL_AND_UNLOCK(condition, mutex) { pthread_cond_signal(condition); pthread_mutex_unlock(mutex); }

/**
 * Inform monitor that the process identified by PID does not need the monitor to remain stopped anymore.
 * If there are no more processes requesting the monitor to remain stopped, then the monitor will restart.
 * If there are other processes that need the monitor to remain stopped, then monitor will NOT restart.
 *
 * @param pid The process identifier of the client that called this function.
 * @return
 *      0 if restarted successfully.
 *      1 if not started because there were other processes requesting monitor to stay stopped.
 *      -1 on errors.
 */
int
SrvStartMonitor( int pid )
{
	pthread_mutex_lock(&garbageMutex);

	// Look in the suppression requests list and remove item matching the PID calling this function.
	DBGLOG("[MONGC] Trying to remove PID %d from suppression request list.\n", pid);
	if (suppressionRequests != NULL) 
	{
		dlist_t *cur = suppressionRequests;
		do {
			suppression_request_t *sr = cur->data;
			if (sr->pid == pid) 
			{
				DBGLOG("[MONGC] Removed PID %d from suppression request list.\n", pid);
				if (cur == suppressionRequests) suppressionRequests = cur->next;
				free(cur->data);
				CoolDlistRemove(cur);
				if (suppressionCount > 0) suppressionCount--;
				break;
			}
			cur = cur->next;
		} while (cur != NULL);
		if (cur == NULL) DBGLOG("[MONGC] PID %d was not in suppression request list.\n", pid);
	}
	else DBGLOG("[MONGC] Suppression request list was already empty.\n");

	// TODO: Do not do anything and quit if there are still suppression requests in queue
	if (suppressionCount > 0) {
		DBGLOG("[MONGC] There are still %d suppression requests in queue. Not starting monitor.\n", suppressionCount);
		SIGNAL_AND_UNLOCK(&garbageCond, &garbageMutex); //signal that garbage collection thread should wake up since list may have changed
		return 1;
	}
	
	//NOTE: we do not unlock the mutex protecting the suppression list while we restart monitor. This
	//is effectively blocking any other request to start/stop the monitor until we are done. We should
	//do this to ensure consistent state even though it may block some apps for a little --nerochiaro
	int ret = start_srv();

	//monitor has been started successfully.
	SIGNAL_AND_UNLOCK(&garbageCond, &garbageMutex);
	return ret;
}

/* NOTE: Strictly for internal usage.
It will force the monitor to start or be restarted regardless if there are applications wanting it to stay down.
Basically it's here ONLY because we need to call this internally when we are changing output mode.
*/
int 
SrvStartMonitorInternal( )
{
	return start_srv();
}

/**
 * Stop video monitor and add PID to the list of processes that are requesting to keep the monitor stopped.
 * As long as you don't call StartMonitor for the same process, monitor will NEVER restart until this application exits.
 * 
 * @param pid The PID of the client process that called this function.
 */
void
SrvStopMonitor( int pid )
{
	pthread_mutex_lock(&garbageMutex);
	//NOTE: we do not unlock the mutex protecting the suppression list while we stop monitor. This
	//is effectively blocking any other request to start/stop the monitor until we are done. We should
	//do this to ensure consistent state even though it may block some apps for a little --nerochiaro

	DBGLOG("Stopping video monitor...");
	if (monitor)
	{
		stop_srv();
	}

	// Look in the suppression requests list to see if this PID is already included. If not, add it.
	DBGLOG("[MONGC] Trying to add PID %d to suppression request list.\n", pid);
	int alreadyPresent = 0;
	if (suppressionRequests != NULL)
	{
		dlist_t *cur = suppressionRequests;
		do {
			suppression_request_t *sr = cur->data;
			if (sr->pid == pid) 
			{
				alreadyPresent = 1;
				break;
			}
			cur = cur->next;
		} while (cur != NULL);
	}

	if (alreadyPresent) DBGLOG("[MONGC] PID %d was already in suppression request list. Not adding it again.\n", pid);
	else
	{
		suppression_request_t *sr = calloc(1, sizeof(suppression_request_t));
		if (sr == NULL) {
			WARNLOG("[MONGC] Failed allocating suppression request item. Bailing, monitor may not stay stopped.\n");
			goto bail;
		}
		sr->pid = pid;
		
		dlist_t *item = CoolDlistNew(sr);
		if (sr == NULL) {
			WARNLOG("[MONGC] Failed allocating suppression request list item. Bailing, monitor may not stay stopped.\n");
			free(sr);
			goto bail;
		}

		if (suppressionRequests == NULL) suppressionRequests = item;
		else CoolDlistInsert(suppressionRequests, item, DLIP_AFTER); //add as head of list
		suppressionCount++;
		DBGLOG("[MONGC] PID %d added to suppression request list successfully.\n", pid);
	}

bail:
	//signal that garbage collection thread should wake up since list may have changed
	pthread_cond_signal(&garbageCond);
	pthread_mutex_unlock(&garbageMutex);
}

/**
 * Is monitor
 * @param return
 *	0 : not monitor, 1 : monitor
 */
int
SrvIsMonitorActive(void)
{
	return monitor;
}

static void * garbageLoop(void *arg)
{
	int rc;

	while (TRUE)
	{
		pthread_mutex_lock(&garbageMutex);
		if (garbageThreadQuit) 
		{
			pthread_mutex_unlock(&garbageMutex);
			break;
		}

		// Check if any PID in the suppression request list are referring to dead processes.
		// If they are, remove them from list so they will not hold the monitor stopped.
		// NOTE: CHECK: it can happen that PIDs of dead processes re-used again for other processes. if a process dies and
		// another takes its PID between two runs of this garbage collector, we can't notice it and we keep the monitor down wrongly.
		// I'm not sure if this is a real issue in normal usage, but we should keep an eye on this. If it turns out to be an issue,
		// we can do the checks by PID and command line, or something like that. --nerochiaro
		if (suppressionRequests != NULL)
		{
			dlist_t *tmp, *cur = suppressionRequests;
			do {
				suppression_request_t *sr = cur->data;

				//this actually sends no signal to the process, but still checks if the process is there.
				if (kill(sr->pid, 0) == -1 && errno == ESRCH) //process does not exist anymore
				{
					tmp = cur->next;

					DBGLOG("[MONGC] PID %d appears dead. Removing from suppression request list (%d remain).\n", sr->pid, suppressionCount-1);
					if (cur == suppressionRequests) suppressionRequests = cur->next;
					free(cur->data);
					CoolDlistRemove(cur);
					if (suppressionCount > 0) suppressionCount--;

					cur = tmp;
				}
				else cur = cur->next;

			} while (cur != NULL);
		}
		else DBGLOG("[MONGC] ==== count is %d\n", suppressionCount);

		// notice here the mutex is still locked, so we're sure no one can be add suppression
		// requests while we check this, possibly restart the server and finally enter condition wait.
		if (suppressionCount == 0)
		{
			start_srv(); //if server is already started, it will be noticed and nothing will be done.

			// the condition wait unlocks the mutex, allowing more suppression requests to come in.
			// when they come in, they will signal the condition.
			rc = pthread_cond_wait(&garbageCond, &garbageMutex);
			if (rc != 0) { //what else can we do ?
				WARNLOG("Failure during condition wait in monitor garbage thread: %d (%s). Exiting thread.\n", rc, strerror(rc));
				pthread_mutex_unlock(&garbageMutex);
				goto bail;
			}
			pthread_mutex_unlock(&garbageMutex);

			// if we are released from the this condition wait, it means we just got at least one suppression request, 
			// so it's ok to enter into the garbage collection condition wait below.
		}
		else pthread_mutex_unlock(&garbageMutex);

		// The state of the suppression queue might have changed between the last unlock and here. But it's ok,
		// we will find out again later after the timed condition wait is done. It will take just a bit more.
		
		// Perform a timed condition wait before the next garbage collection cycle, so we don't do it too often.
		// The wait will be also interrupted immediately if suppression requests are added or removed.
		pthread_mutex_lock(&garbageMutex);
		if (garbageThreadQuit)
		{
			pthread_mutex_unlock(&garbageMutex);
			break;
		}

		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += GARBAGE_CHECK_WAIT;

		rc = pthread_cond_timedwait(&garbageCond, &garbageMutex, &ts);
		if (rc != ETIMEDOUT && rc != 0) { //what else can we do ?
			WARNLOG("Failure during timed condition wait in monitor garbage thread: %d (%s). Exiting thread.\n", rc, strerror(rc));
			pthread_mutex_unlock(&garbageMutex);
 			goto bail;
		}

		pthread_mutex_unlock(&garbageMutex);
	}

bail:
	pthread_exit(NULL);
}

/* 
You should call this once when starting the server, before the monitor startup request are allowed to come in.
Then system will start the monitor and do its best to keep it running until someone requests it down.
*/
void SrvStartMonitorGarbageCollector()
{
	DBGLOG("[MONGC] Starting monitor garbage collector.\n");

	if (suppressionRequests != NULL) {
		CoolDlistFree(suppressionRequests, TRUE); //just for safety
		suppressionRequests = NULL;
	}
	suppressionCount = 0;
	garbageThreadQuit = 0;

	if (pthread_create(&garbageThread, NULL, garbageLoop, NULL))
	{
		ERRLOG("[MONGC] Monitor garbage collector thread was not created!");
	}
	else DBGLOG("[MONGC] Monitor garbage collector created successfully.\n");
}

/* 
You should call this only when stopping the server, when no more requests can come in.
*/
void SrvStopMonitorGarbageCollector()
{
	DBGLOG("[MONGC] Stopping monitor garbage collector.\n");
	// raise the quit flag, then signal the thread so that it exits any blocking wait it's in.

	pthread_mutex_lock(&garbageMutex);
	garbageThreadQuit = 1;
	pthread_mutex_unlock(&garbageMutex);
	pthread_cond_signal(&garbageCond);

	// wait for the thread to actually finish.
	pthread_join(garbageThread, NULL);
	garbageThread = (pthread_t)NULL;

	if (suppressionRequests != NULL) {
		CoolDlistFree(suppressionRequests, TRUE);
		suppressionRequests = NULL;
	}

	DBGLOG("[MONGC] Monitor garbage collector stopped.\n");
}

/**
This is used when we start NMSD, to avoid resetting the video always to NTSC. 
@param mode NTSC=0, PAL=1
*/
void SrvMonitorInitOutputMode(int mode)
{
	OutputSetMode(mode); // This just sets the initial mode in the plugin. OutputActivateMode will make that effective.
	EncInputSetMode(mode);
	outNTSC_PAL = mode;
}
