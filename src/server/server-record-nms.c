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
 * Neuros-Cooler platform nms recording server module.
 *
 * REVISION:
 * 
 * 4) Cleaned up mutex usage. ----------------------------- 2007-12-14 MG
 * 3) new experimental sync algorithm. -------------------- 2007-03-20 MG
 * 2) Split a/v to its own thread. ------------------------ 2006-04-13 MG
 * 1) Initial creation. ----------------------------------- 2006-01-06 MG 
 *
 */

#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/statvfs.h>

#define CL_HACK 
/* the second audio timestamp is too bigger than first.
        * But the root cause should be capture side.*/
   
//#define LOG_TIME_STAMP__
//#define LOG_EACH_FRAME
//#define DEBUG_RESERVE
//#define OSD_DBG_MSG
#include "nc-err.h"

#include "cmd-nms.h"
#include "com-nms.h"
#include "server-nms.h"
#include "nmsplugin.h"
#include "plugin-internals.h"

#define PID_LEN 10
#define ENC_AUDIO_THREAD_PRIORITY 99 //may need the highest priority
#define PASSTHRU_PIP_RESIZE_THREAD_PRIORITY 90
#define IRESIZE_THREAD_PRIORITY 80 // must higher than nmsd thread priority(1) and lower 
																			// than encode audio thread priority(99)
#define IDMATF_THREAD_PRIORITY 80 // must higher than nmsd thread priority(1) and lower
																			// than encode audio thread priority(99)
#define PROC_IDMATF_PID "/proc/ingenient/imanage/idmatf_pid"
#define PROC_IRESIZE_PID "/proc/ingenient/ividio/iresize_pid"
#define PROC_AUDIO_ENC_PID "/proc/ingenient/iencode/audio_enc_pid"
#define PROC_PASSTHRU_PIP_RESIZE_PID "/proc/ingenient/iencode/passthru_pip_resize_pid"

typedef enum {
	RECORDER_STOPPED = -1,		/** in stopped status */
	RECORDER_RUNNING = 0,		/** in recorder status */
	
	/** Follow three number is hard code, NOT permit to modify. Actually, it is a flag */
	RECORDER_AUDIO_STOP = 1,	/** AUDIO thread stop flag */
	RECORDER_VIDEO_STOP = 2,	/** VIDEO thread stop flag */
	RECORDER_AV_STOP = 3		/** Both AUDIO thread and VIDEO thread stop flag */
} ENUM_STOP_STATUS_t;

typedef enum {
	RECORDER_UNPAUSE = 0,		/** in recorder status */
	RECORDER_TO_PAUSE = 1,	/** toggle to pause */
	RECORDER_PAUSE = 2,		/** in paused status */
	RECORDER_TO_UNPAUSE = 3	/** toggle to recorder */
} ENUM_PAUSE_STATUS_t;

static int                paused; 
static int                timestamp;
static int                timeoffset;
static int                stopped = RECORDER_STOPPED;
static int                going;
static NMS_SRV_ERROR_DETAIL lastRecErrorDetail;

static media_desc_t       mdesc;
static media_buf_t        mbuf;

/*
 * Recording finalization is using too much memory with low quality sometimes and the system crash
 * due to out of memory. So limit the maximum of recording length to hack it for now.
*/
#define MAX_LENGTH (4 * 60 * 60) // 4hours

#ifdef CL_HACK
#define SAMPLE_RATE 16000      // according to audio samplerate is 16000
#define TSMS_INCREMENT 128  // timestamp increment, according to audio samplerate is 16000
#define AV_SYNC 64                     // keep avsnyc, according to audi samplerate is 16000
static int     anumber = 0;
static int     atsms = 0;
static int     adelta = 0;
#endif

/* Used for initial and ongoing free space checks */
static unsigned long long diskFreeSpace;
static unsigned int       recordingSize;
//static unsigned int       initialScratchReq;
static encoding_requirements_t requirements;
#define FILE_SIZE_LIMIT  ((unsigned int)-1) //around 4Gb

static int                audReady;
static int                vidReady;
static pthread_t          audThread = (pthread_t)NULL;
static pthread_t          vidThread = (pthread_t)NULL;
static pthread_mutex_t    recordMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t     recordCond  = PTHREAD_COND_INITIALIZER;

#define LOCK_RMUTEX()  do {							\
		pthread_mutex_lock(&recordMutex);			\
	}while(0)
#define UNLOCK_RMUTEX()  do{					\
		pthread_mutex_unlock(&recordMutex);		\
	}while(0)


/*
Perform various disk-related checks before committing a frame of size commitSize to disk.
All these tests perform only a few arithmetical operations, and don't call any system functions or 
do I/O, so they should be quite lightweigth to perform even at each frame commit.

0 = we can go on recording
1 = we will break the 4Gb barrier if we don't stop recording now.
2 = we will not be able to finalize due to insufficient disk space if we don't stop recording now.
3 = we will break the 4 HOURS barrier if we don't stop recording now. 
*/
#ifdef DEBUG_RESERVE
static unsigned int reserve_dbg_count = 0;
#endif
static int preCommitChecks(unsigned int commitSize)
{
	int ret = 0;

	//Update requirements to match current recording status. This operation should be inexpensive.
	EncOutputGetRequirements(&requirements);

#ifdef DEBUG_RESERVE
	if (reserve_dbg_count++ % 1000 == 0) DBGLOG("finalization = %u, to_filemax: %u, size: %u\n", requirements.finalization, FILE_SIZE_LIMIT - requirements.finalization, recordingSize);
#endif

	// ** First test (4Gb limit).
	// Question: would we be able to finalize this file keeping it under 4Gb, if we were to finalize after committing ?
	if (recordingSize >= FILE_SIZE_LIMIT - (requirements.finalization + commitSize))
		ret = 1;
	
	// ** Second test (free disk space).
	// Question: would we have enough disk space to finalize this file, if we were to finalize after committing ?
	else if (recordingSize >= diskFreeSpace - (requirements.finalization + commitSize))
		ret = 2;
	else if ((timestamp / 1000) > MAX_LENGTH)
		ret = 3;
	
	return ret;
}

static void stop_server(void)
{
	LOCK_RMUTEX();
	if (RECORDER_STOPPED == stopped )
	{
		UNLOCK_RMUTEX();
		return;
	}

	DBGLOG("Stopping server. Current size: %u\n", recordingSize);
	if (mdesc.adesc.audio_type == NMS_AC_NO_AUDIO)
	{
		stopped = RECORDER_STOPPED;
		EncInputFinish();
		EncOutputFinish();
	}
	else
	{
		DBGLOG("stopping interface.");

	    if (RECORDER_AV_STOP == stopped)
		  {
			DBGLOG("interface stopped.");
			stopped = RECORDER_STOPPED;
			EncInputFinish();
			// TODO: The exit status of this is pretty important to report MP4 errors, so we need to send it out somehow.
			//       most likely with the same system as lastRecErrorDetail.
			EncOutputFinish();
		  }
	}
	UNLOCK_RMUTEX();

	pthread_cond_broadcast(&recordCond);
}

static int avsync_save_frame(void)
{
	BOOL save_audio_frame = FALSE;
	BOOL save_video_frame = FALSE;
	BOOL update_time_stamp = FALSE;
	int status = 0;

#ifdef LOG_EACH_FRAME
	DBGLOG("entering avsync frame.");
#endif
	LOCK_RMUTEX();
	if (mdesc.adesc.audio_type != NMS_AC_NO_AUDIO)
	{
		if ((!audReady || !vidReady))
		{
			WPRINT("a/v not ready yet.");
			goto out;
		}
	}
	if (mdesc.vdesc.video_type == NMS_VC_NO_VIDEO)
	{
		// audio only.
#ifdef LOG_EACH_FRAME
		DBGLOG("save audio only frame.");
#endif
		save_audio_frame = TRUE;
		mbuf.curbuf = &mbuf.abuf;
		update_time_stamp = TRUE;
	}
	else if (mdesc.adesc.audio_type == NMS_AC_NO_AUDIO)
	{
#ifdef LOG_EACH_FRAME
		DBGLOG("save video only frame.");
#endif
		save_video_frame = TRUE;
		mbuf.curbuf = &mbuf.vbuf;
		update_time_stamp = TRUE;
	}
	else
	{
		if (mbuf.abuf.tsms <= mbuf.vbuf.tsms)
		{
#ifdef LOG_EACH_FRAME
			DBGLOG("avsync save audio frame.");
#endif
			save_audio_frame = TRUE;
			mbuf.curbuf = &mbuf.abuf;
#ifdef CL_HACK
			int tsmsTemp = 0;
			if (anumber > 1)
			{
				mbuf.curbuf->tsms -= adelta;
			}
			else if (anumber == 1)
			{
				tsmsTemp = atsms + (SAMPLE_RATE * TSMS_INCREMENT) / mdesc.adesc.sample_rate;
				adelta = mbuf.curbuf->tsms - tsmsTemp;
				mbuf.curbuf->tsms = tsmsTemp;
				anumber = 2;
			}
			else
			{
				atsms = mbuf.curbuf->tsms;
				anumber = 1;
			}
            
			// keep async
			tsmsTemp = (AV_SYNC * SAMPLE_RATE) / mdesc.adesc.sample_rate;
			if (mbuf.curbuf->tsms > tsmsTemp)
				mbuf.curbuf->tsms -= tsmsTemp;
#endif			
		}
		else
		{
#ifdef LOG_EACH_FRAME
			DBGLOG("avsync save video frame.");
#endif
			save_video_frame = TRUE;
			mbuf.curbuf = &mbuf.vbuf;
			update_time_stamp = TRUE;
		}
	}
	
	// track time difference between paused state change.
	if (RECORDER_TO_PAUSE == paused)
	{
		timeoffset -= mbuf.curbuf->tsms;
		paused = RECORDER_PAUSE;
	}
	else if (RECORDER_TO_UNPAUSE == paused)
	{
		timeoffset += mbuf.curbuf->tsms;
		paused = RECORDER_UNPAUSE;
	}
	
	if (0 == paused)
	{
		// Before committing this frame, check if committing it would go over any disk space limits or file size limits.
		// Stop the recording cleanly if that happens and return a meaningful error so that recorder can handle correctly.
		
		int check = preCommitChecks(mbuf.curbuf->size);
		if (check != 0) 
		{
			DBGLOG("Pre-commit check exit with value %d. Current recordingSize: %u\n", check, recordingSize);
			
			lastRecErrorDetail.source = SRC_SERVER_RECPRECOMMIT;
			if (check == 1) lastRecErrorDetail.error = NMS_RECORD_FILE_SIZE; //we should finalize now or exceed 4GB file size
			else if (check == 2) lastRecErrorDetail.error = NMS_RECORD_OUT_DISKSPACE; //we should finalize or exceed free space
			else if (check == 3) lastRecErrorDetail.error = NMS_RECORD_FILE_LENGTH; // exceed 4hours
			else assert("Unexpected preCommitChecks return value");
			
			status = -1;
			going = 0;
			
			goto bail;
		}
		
		recordingSize += mbuf.curbuf->size;
		mbuf.curbuf->tsms -= timeoffset;
		
		int commitret = EncOutputCommit(&mbuf, timeoffset);
		if (commitret)
		{
			WPRINT("Data commit error (%d).", commitret);
			
			if (commitret == KNOWNERR_MAX_FRAMES_LIMIT) lastRecErrorDetail.error = NMS_RECORD_FRAME_LIMIT; 
			else lastRecErrorDetail.error = NMS_RECORD_OTHER_COMMIT_ERROR;
			lastRecErrorDetail.source = SRC_PLUG_OUT_COMMIT;
			lastRecErrorDetail.message = commitret;
			
			status = -1;
			going = 0;
			goto bail;
		}
		if (update_time_stamp) timestamp = mbuf.curbuf->tsms;
	}
 bail:
    if (save_audio_frame == TRUE)
	{
		EncInputPutBuffer(1, &mbuf); 
		audReady = 0;
	}
    else if (save_video_frame == TRUE)
	{
		EncInputPutBuffer(0, &mbuf);
		vidReady = 0;
	}
	
#ifdef LOG_TIME_STAMP__
	if (save_audio_frame == TRUE)
		DBGLOG("   aT = %d\n", mbuf.curbuf->tsms);
	else
		DBGLOG("---vT = %d\n", mbuf.curbuf->tsms);
#endif
	
 out:
#ifdef LOG_EACH_FRAME
	DBGLOG("leaving avsync frame.");
#endif
    UNLOCK_RMUTEX();
    pthread_cond_broadcast(&recordCond);
	return status;
}

static void * audLoop( void * arg )
{
	struct sched_param nmsd_param;

	sched_getparam(0, &nmsd_param);
	nmsd_param.sched_priority=1;
	sched_setscheduler(0,SCHED_RR,&nmsd_param);
	while (1)
	  {
#ifdef LOG_EACH_FRAME
		DBGLOG("audio encode thread.");
#endif
		LOCK_RMUTEX();
		if (audReady)
		{
			// Wait till previous audio framed is saved.
			while (audReady && going)
			{
#ifdef LOG_EACH_FRAME
				DBGLOG("audio thread waiting...");
#endif
				pthread_cond_wait(&recordCond, &recordMutex);
			}
		}
#ifdef LOG_EACH_FRAME
		DBGLOG("audio thread check point.");
#endif

		if (!going)
		{
			UNLOCK_RMUTEX();
			break;
		}
		UNLOCK_RMUTEX();

#ifdef LOG_EACH_FRAME
		DBGLOG("get audio buffer.");
#endif
		// do not need to protect mbuf because EncInputGetBuffer only
		// operates on either audio or video buffer.
		if ( 0 != EncInputGetBuffer(1, &mbuf, 100))
		{
			WPRINT("audio buffer empty!");
			continue;
		}

		LOCK_RMUTEX();
		audReady = 1;
		UNLOCK_RMUTEX();

		if (avsync_save_frame()) break;
	  }

	DBGLOG("audio thread exiting.");
	LOCK_RMUTEX();
	audReady = 0;
	stopped |= RECORDER_AUDIO_STOP;
	UNLOCK_RMUTEX();
	DBGLOG("audio thread exited.");
	
	stop_server();
	pthread_exit(NULL);
}

static void * vidLoop( void * arg )
{
	struct sched_param nmsd_param;

	sched_getparam(0, &nmsd_param);
	nmsd_param.sched_priority=1;
	sched_setscheduler(0,SCHED_RR,&nmsd_param);
	while (1)
	{
#ifdef LOG_EACH_FRAME
		DBGLOG("video encode thread.");
#endif
		
		LOCK_RMUTEX();
		if (vidReady)
		{
			// Wait till previous audio framed is saved.
			while (vidReady && going)
			{
#ifdef LOG_EACH_FRAME
				DBGLOG("video thread waiting...");
#endif
				pthread_cond_wait(&recordCond, &recordMutex);
			}
		}
#ifdef LOG_EACH_FRAME
		DBGLOG("video thread check point.");
#endif
		
		if (!going)
		{
			UNLOCK_RMUTEX();
			break;
		}
		UNLOCK_RMUTEX();
		
#ifdef LOG_EACH_FRAME
		DBGLOG("get video buffer.");
#endif
		if ( 0 != EncInputGetBuffer(0, &mbuf, 100))
		{
#ifdef LOG_EACH_FRAME
			WPRINT("video buffer empty!");
#endif
			continue;
		}
		
		LOCK_RMUTEX();
		vidReady = 1;
		UNLOCK_RMUTEX();
		if (avsync_save_frame()) break;
	}
	
	DBGLOG("video thread exiting.");
	LOCK_RMUTEX();
	vidReady = 0;
	stopped |= RECORDER_VIDEO_STOP;
	UNLOCK_RMUTEX();
	DBGLOG("video thread exited.");

	stop_server();
	pthread_exit(NULL);
}

static void revert_imedia_thread_prio(void)
{
	int pid;
	char buf[PID_LEN];
	struct sched_param nmsd_param;
	int fd=-1;
	if (mdesc.vdesc.video_type != NMS_AC_NO_AUDIO)
	{
		fd=open(PROC_AUDIO_ENC_PID, O_RDONLY, 0);
		if (fd!=-1)
		{
			memset(buf,0,PID_LEN);
			read(fd, buf, PID_LEN-1);
			pid=atoi(buf);
			if (pid!=-1)
			{
				sched_getparam(pid, &nmsd_param);
				nmsd_param.sched_priority=0;
				sched_setscheduler(pid, SCHED_RR,&nmsd_param);
			}
			close(fd);
		}
	}
	fd=open(PROC_IRESIZE_PID, O_RDONLY, 0);
	if (fd!=-1)
	{
		memset(buf,0,PID_LEN);
		read(fd, buf, PID_LEN-1);
		pid=atoi(buf);
		if (pid!=-1)
		{
			sched_getparam(pid, &nmsd_param);
			nmsd_param.sched_priority=0;
			sched_setscheduler(pid, SCHED_RR,&nmsd_param);
		}
		close(fd);
	}
}

static void increase_imedia_thread_prio(void)
{
	int pid;
	char buf[PID_LEN];
	struct sched_param nmsd_param;
	int fd=-1;
	if (mdesc.vdesc.video_type != NMS_AC_NO_AUDIO)
	{
		fd=open(PROC_AUDIO_ENC_PID, O_RDONLY, 0);
		if (fd!=-1)
		{
			memset(buf,0,PID_LEN);
			read(fd, buf, PID_LEN-1);
			pid=atoi(buf);
			if (pid!=-1)
			{
				sched_getparam(pid, &nmsd_param);
				nmsd_param.sched_priority=ENC_AUDIO_THREAD_PRIORITY;
				sched_setscheduler(pid, SCHED_RR,&nmsd_param);
			}
			close(fd);
		}
	}
	fd=open(PROC_IRESIZE_PID, O_RDONLY, 0);
	if (fd!=-1)
	{
		memset(buf,0,PID_LEN);
		read(fd, buf, PID_LEN-1);
		pid=atoi(buf);
		if (pid!=-1)
		{
			sched_getparam(pid, &nmsd_param);
			nmsd_param.sched_priority=IRESIZE_THREAD_PRIORITY;
			sched_setscheduler(pid, SCHED_RR,&nmsd_param);
		}
		close(fd);
	}
}

/**
 * Record into the specified file.
 *
 * @param ctrl
 *        Recording control parameters
 * @param fname
 *        Path of the file to record to
 * @param details
 *        More detailed error reporting (notice that the "error" field in this structure
 *        is always the same as the return value.
 * @return
 *        0 (NMS_SRV_RECORD_ERROR) if file recording started successfully.
 *        Other (nonzero) NMS_SRV_RECORD_ERROR codes otherwise.
 */
NMS_SRV_RECORD_ERROR
SrvRecord( rec_ctrl_t * ctrl, char * fname, NMS_SRV_ERROR_DETAIL *details )
{
	int pluginret;

	DBGLOG("Target file name: %s", fname);

	//stop recording interface if already started.
	SrvStopRecord();


	LOCK_RMUTEX();
	recordingSize = 0;

	// Reset last error info. This information is never changed in this function, only later during recording.
	lastRecErrorDetail.error = NMS_RECORD_OK;
	lastRecErrorDetail.source = SRC_SERVER_RECRUNNING;
	lastRecErrorDetail.message = 0;
	UNLOCK_RMUTEX();

	// This will return the following if successful. In case of errors, these will be updated.
	details->error = NMS_RECORD_START_OTHER;
	details->source = SRC_SERVER_RECSTART;
	details->message = 0;

	#ifdef CL_HACK
	atsms = 0;
	anumber = 0;
	#endif	

	memset(&mdesc, 0, sizeof(media_desc_t));
	if (!EncOutputIsOurFormat(ctrl, fname, &mdesc)) 
	{
		WARNLOG("No encoder output plugin can handle this recording parameters.");
		details->error = NMS_RECORD_PARAMS_UNSUPPORTED;
		return details->error;
	}
	DBGLOG("Encoder output plugin found.");

	mdesc.vdesc.capture_port = ctrl->is_pal;
	OutputSelect(NMS_PLUGIN_MULTIMEDIA);
	
	pluginret = EncOutputInit(&mdesc);
	if (pluginret != 0)
	{
		WARNLOG("Unable to initialize encoder output. Error: %d", pluginret);
		details->source = SRC_PLUG_OUT_INIT;
		details->message = pluginret;
		goto bail;
	}
	DBGLOG("Encoder output initialized.");
	
	pluginret = EncOutputStart();
	if (pluginret != 0) 
	{
		WARNLOG("Unable to start encoder output. Error: %d", pluginret);
		details->source = SRC_PLUG_OUT_START;
		details->message = pluginret;
		goto bail1;
	}
	DBGLOG("Encoder output started.");

	/* Get the amount of free space that is available on the disk where we want to record.
		Then ask the plugin how much space does it need (if any) to perform a clean finalization
		of the recording. At each a/v sync we will check this value before committing the
		data to disk to ensure we meet this finalization requirement. Please notice that we expect
		that nothing else is writing to disk while we perform a recording, aside for the plugin itself --nerochiaro
		TODO: to relax the above assumption, this can be modified to fetch again the free disk space at each N commits, if it's not too much overhead
	*/

	//NOTE: we are assuming the file on disk was already created. if not, this will fail.
	struct statvfs st;
	if (statvfs(fname, &st) != 0)
	{
		WARNLOG("Failed checking free disk space with statvfs(%s): %d, %s\n", fname, errno, strerror(errno));
		details->error = NMS_RECORD_OUT_DISKSPACE;
		details->message = 1;
		goto bail1;
	}
	else diskFreeSpace = (st.f_bfree * st.f_frsize);

	memset(&requirements, 0, sizeof(encoding_requirements_t));
	EncOutputGetRequirements(&requirements);
	
	// Do we have the necessary disk space to even start this recording ?
	if (diskFreeSpace <= requirements.disk_scratch_space + requirements.finalization) {
 		//TODO: Currently the way the init sequence of the plugins works doesn't allow us to know if the 
		//      free disk space is enough until after the init function, at which point it's too late since
		//      the file is already created on disk. The right way is to split Init and Start logic properly
		//      and do the check in between them (and create the file in Start only if the check was ok).
		//      For now just delete the initial file in this case.
		remove(fname);

		details->error = NMS_RECORD_OUT_DISKSPACE;
		details->message = 2;
		goto bail1;
	}

	LOCK_RMUTEX();
	//initialScratchReq = requirements.disk_scratch_space;

	// init controls.
	timeoffset = 0;
	timestamp = 0;
	stopped = RECORDER_RUNNING;
	paused = 0;
	going  = 1;
	audReady = 1;
	vidReady = 1;
	UNLOCK_RMUTEX();

	if (mdesc.adesc.audio_type != NMS_AC_NO_AUDIO)
	{
		if (pthread_create(&audThread, NULL, audLoop, NULL))
		{
			WARNLOG("Audio thread was not created!");
			details->message = 3;
			goto bail1;
		}
   }
	if (pthread_create(&vidThread, NULL, vidLoop, NULL))
	{
		WARNLOG("Video thread was not created!");
		details->message = 4;
		goto bail1;
	}

	// Prepare to start.
	LOCK_RMUTEX();

	pluginret = EncInputInit(&mdesc, OutputGetMode()); //TODO: input plugins should return more detailed error codes
	if (pluginret != 0)
	{
		WARNLOG("Unable to init encoder input!");
		UNLOCK_RMUTEX();
		details->source = SRC_PLUG_INP_INIT;
		details->message = pluginret;
		goto bail1;
	}
	DBGLOG("Encoder input initialized.");

	/* start Input */
	pluginret = EncInputStart(); //TODO: input plugins should return more detailed error codes
	if (pluginret != 0)
	{
		WARNLOG("Unable to start encoder input!");
		UNLOCK_RMUTEX();
		details->source = SRC_PLUG_INP_INIT;
		details->message = pluginret;
		goto bail2;
	}
	DBGLOG("Encoder input started.");

	// Initialization done, now go!
	audReady = 0;

	// video thread don't care if video is not available.
	if (mdesc.vdesc.video_type != NMS_VC_NO_VIDEO) vidReady = 0;
	
	UNLOCK_RMUTEX();
	pthread_cond_broadcast(&recordCond);

	increase_imedia_thread_prio();

	details->error = NMS_RECORD_OK;
	return details->error;

bail2:
	EncInputFinish();
bail1:
	EncOutputFinish(); //don't care of the return here, we're bailing out anyway 
bail:

	LOCK_RMUTEX();
	going = 0;
	UNLOCK_RMUTEX();

	return details->error;
}

/**
 * Pause/unpause current recording.
 */
void
SrvPauseRecord( int p )
{
   DBGLOG("pausing recording. %d", p);

    LOCK_RMUTEX();
	if ( p != (paused >> 1)) 
	{
		if (p)
			paused = RECORDER_TO_PAUSE;
		else
			paused = RECORDER_TO_UNPAUSE;
	}
	UNLOCK_RMUTEX();

	pthread_cond_broadcast(&recordCond);
}

/**
 * Stop current recording.
 */
void
SrvStopRecord( void )
{
    LOCK_RMUTEX();
	going = 0;
	UNLOCK_RMUTEX();
	pthread_cond_broadcast(&recordCond);

	if (audThread)
	{
		DBGLOG("Removing audio thread!");
		pthread_join(audThread, NULL);
		audThread = (pthread_t)NULL;
		DBGLOG("Audio thread removed!");
	}
	
	if (vidThread)
	{
		DBGLOG("Removing video thread!");
		pthread_join(vidThread, NULL);
		vidThread = (pthread_t)NULL;
		DBGLOG("Video thread removed!");
	}
	revert_imedia_thread_prio();
}

/**
 * Get current gain.
 *
 * @param left
 *        left channel gain.
 * @param right
 *        right channel gain.
 */
void
SrvGetGain( int * left, int * right )
{
    EncInputGetGain(left, right);
}

/**
 * Set current recording gain.
 *
 * @param left
 *        left channel gain.
 * @param right
 *        right channel gain.
 */
void
SrvSetGain( int left, int right )
{
    EncInputSetGain(left, right);
}

/**
 * Get current record time stamp.
 *
 * @return
 *         record time stamp in seconds.
 */
int
SrvGetRecordtime( void )
{
	int loc_timestamp;

	LOCK_RMUTEX();
	loc_timestamp = timestamp;
	UNLOCK_RMUTEX();

    return loc_timestamp;
}

/**
 * Get current record file size.
 *
 * @return
 *         record file size in byte.
 */
unsigned int
SrvGetRecordsize(void)
{
	unsigned int loc_recordingSize;

	LOCK_RMUTEX();
	loc_recordingSize = recordingSize;
	UNLOCK_RMUTEX();

	return loc_recordingSize;
}

/**
 * Get last recording error status
 *
 * @return
 *         last recording error status
 */
void
SrvGetRecordError(NMS_SRV_ERROR_DETAIL * details)
{
	LOCK_RMUTEX();
	memcpy(details, &lastRecErrorDetail, sizeof(NMS_SRV_ERROR_DETAIL));
	UNLOCK_RMUTEX();
}

/**
 * Tell if server is recording.
 *
 * @return
 *         1 if server is still recording, otherwise zero.
 */
int 
SrvIsRecording( void )
{
	int loc_going;

	LOCK_RMUTEX();
	loc_going = going;
	UNLOCK_RMUTEX();

	return loc_going;
}

