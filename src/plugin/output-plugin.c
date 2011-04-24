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
 * plugin output support module.
 *
 * REVISION:
 * 
 * 4) Added support for setting output proportions -------- 2008-04-10 nerochiaro
 * 3) Added in background preference support. ------------- 2007-08-07 MG
 * 2) Added encoder interfaces. --------------------------- 2006-01-09 MG
 * 1) Initial creation. ----------------------------------- 2005-09-23 MG 
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include "nmsplugin.h"
#include "plugin-internals.h"

//#define LOG_EVERY_FRAME
//#define OSD_DBG_MSG
#include "nc-err.h"


static int outNTSC_PAL;

media_output_plugin_ctrl_t * outputPlugin = 
  (media_output_plugin_ctrl_t *)OutputPlugin();

/**
 * Select output plugin for given input type.
 *
 * @param type
 *        input type.
 * @return
 *        0 if output plugin found, otherwise nonzero.
 */
int
OutputSelect( int type )
{
	slist_t * head;
	media_output_plugin_t * op;

	head = outputPlugin->head;
	while (head)
	  {
		op = (media_output_plugin_t*)head->data;
		DBGLOG("checking output plugin - %s", op->brief);
		if (type == op->type)
		  {
			outputPlugin->actv = op;
			return 0;
		  }
		else head = head->next;
	  }

	return -1;
}

/**
 * Output mode set
 * @param mode
 * 		0 : NTSC, 1 : PAL
 * @return
 *		0 if successful
 */
void
OutputSetMode(int mode)
{
	outNTSC_PAL = mode;
}

/**
 * Output mode get
 * @return
 * 		0 : NTSC, 1 : PAL
 */
 int 
 OutputGetMode(void)
{
	return outNTSC_PAL;
}

/**
 * output mode active
 * @param tracking_video_in
 *        0:to set output mode for video decoding only. 
 *        1:to set output mode to match input video.
 * @return 
 *      0 successful, otherwise nonzero.
 */
 int
 OutputActivateMode(int tracking_video_in)
{
	return outputPlugin->actv->setOutputMode(outNTSC_PAL, tracking_video_in);
}

/**
 * Initialize interface. 
 *
 * @param params
 *        media parameters.
 * @param mode
 *	    output mode, 
 *		 0 : ntsc,
 *		 1 : pal
 * @param proportions
 *        0: normal (4:3)
 *        1: widescreen (16:9)
 * @return
 *        0 if successful.
 *        1 if output already locked by others.
 *        -1 on errors.
 */
int
OutputInit( const media_desc_t * mdesc, int mode, int proportions)
{
	return outputPlugin->actv->init(mdesc, mode, proportions);
}

/**
 * Start output device.
 */
void
OutputStart()
{
    outputPlugin->actv->start();
}

/**
 * Get output buffers.
 *
 * @param buf
 *        media buffer.
 * @param timeout
 *        polling timeout value.
 * @param preload
 *        1 to indicate in the middle of preloading, otherwise 0.
 * @return
 *        0 if successful, 1 if all buffers are full, otherwise negative value.
 */
int
OutputGetBuffer(media_buf_t * buf, int timeout, int preload)
{
    return outputPlugin->actv->getBuffer(buf, timeout, preload);
}

/**
 * Write to output device.
 *
 * @param buf
 *        media buffer.
 */
void
OutputWrite(const media_buf_t * buf)
{
    outputPlugin->actv->write(buf);
}

/**
 * Pause/unpause output.
 *
 * @param p
 *        1 to pause, 0 to unpause.
 */
void
OutputPause( int p )
{
	outputPlugin->actv->pause(p);
}

/**
 * Mute/unmute output.
 *
 * @param p
 *        1 to mute, 0 to unmute.
 */
void
OutputMute( int p )
{
	outputPlugin->actv->mute(p);
}

/**
 * Stop current output.
 */
void
OutputFinish( int wait )
{	
	outputPlugin->actv->finish(wait);
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
OutputGetVolume( int * left, int * right )
{
	outputPlugin->actv->getVolume(left, right);
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
OutputSetVolume( int left, int right )
{
	outputPlugin->actv->setVolume(left, right);
}

/**
 * Get current playback time stamp.
 *
 * @return
 *         playback time stamp in miliseconds.
 */
int
OutputGetPlaytime( void )
{
	return outputPlugin->actv->outputTime();
}

/**
 * Flush the output buffer.
 *
 * @param t
 *        output time stamp in milisecond.
 */
void
OutputFlush( int t )
{
	outputPlugin->actv->flush((long long)t);
}

/**
 * Get amount of data in output buffer
 *
 * @return
 *        The amount of bytes still in output buffer
 */
unsigned long
OutputGetBufferedSize( )
{
	return outputPlugin->actv->bufferedData(NMS_BUFFER_VIDEO);
}

/******************************************************************/
/*--------------------- encoder interface. -----------------------*/
/******************************************************************/
media_enc_output_plugin_ctrl_t * encOutputPlugin = 
  (media_enc_output_plugin_ctrl_t *)EncOutputPlugin();
audio_encode_plugin_ctrl_t * aencodePlugin = 
  (audio_encode_plugin_ctrl_t *)AudioEncodePlugin();


/**
 * Check if any encoder ouput plugin can handle the specified recording parameters
 *
 * @param ctrl
 *        encoder controls.
 * @param fname
 *        output file name.
 * @param params
 *        media parameters.
 * @return
 *        1 if format is valid, otherwise 0.
 */ 
int EncOutputIsOurFormat( rec_ctrl_t* ctrl, void * fname, media_desc_t * mdesc)
{
	slist_t * head;
	media_enc_output_plugin_t * eop;
	int ret = 0;

	head = encOutputPlugin->head;
	while (head)
	{
		eop = (media_enc_output_plugin_t*)head->data;
		DBGLOG("Checking output plugin: %s.", eop->brief);

		if ( eop->isOurFormat(ctrl, fname, mdesc) ) 
		{
			encOutputPlugin->actv = eop;
			if(!EncInputSelect(eop->type)) ret = 1;
			break;
		}
		else head = head->next;
	}

	return ret;
}

void EncOutputGetRequirements( encoding_requirements_t * requirements )
{
	encOutputPlugin->actv->getRequirements(requirements);
#ifdef LOG_EVERY_FRAME
	DBGLOG("Enc req: scratch: %u, final: %u\n", requirements->disk_scratch_space, requirements->finalization);
#endif
}

/**
 * Check to initialize current output plugin.
 *
 * @param params
 *        media parameters.
 * @return
 *        0 if successful.
 */
int EncOutputInit( const media_desc_t * mdesc )
{
	slist_t * head;
	audio_encode_plugin_t * aep;
	int target_codec;

    DBGLOG("Initializaing encoder output...");
#if BUILD_TARGET_ARM
	if (0 == encOutputPlugin->actv->init())
	  {
		/* logic to detect and  init audio plugins.*/
		aencodePlugin->actv = NULL;
		if (NMS_AC_NO_AUDIO == mdesc->adesc.audio_type) 
		{
			DBGLOG("no audio codec detected.");
			return 0;
		}

		target_codec = mdesc->adesc.audio_type;

		DBGLOG("searching for audio codec...");
		DBGLOG("target codec = %d", target_codec);
		/* search for audio codec plugin. */
		head = aencodePlugin->head;
		while (head)
		  {
			aep = (audio_encode_plugin_t*)head->data;
			DBGLOG("\tplugin codec = %d", aep->codec);
			if ( aep->codec == target_codec ) 
			  {
				aencodePlugin->actv = aep;
				aep->init(&mdesc->adesc);
				return 0;
			  }
			else head = head->next;
		  }
		WARNLOG("unable to find audio codec!");
		/* use DSP side codec? */
		return 0;
	  }
#endif /*BUILD_TARGET_ARM*/
	return -1;
}

/** Start encoder output interface.
 *
 * @return 
 *         0 if successful.
 */
int EncOutputStart( void )
{
    return (encOutputPlugin->actv->start());
}

/**
 * Finish encoder output interface.
 *
 */
int EncOutputFinish( void )
{
	if (aencodePlugin->actv) 
		aencodePlugin->actv->finish();
    return (encOutputPlugin->actv->finish());
}

/**
 * Encoder output data commiting interface.
 *
 * @param buf
 *        media data buffer.
 * @param timeoffset
 *        
 * @return
 *        0 if successfully commit.
 *        otherwise an error that is plugin specific
 */
int EncOutputCommit(media_buf_t * buf, int timeoffset )
{
    return (encOutputPlugin->actv->commit(buf));
}

media_capture_plugin_ctrl_t * capturePlugin = 
  (media_capture_plugin_ctrl_t *)CapturePlugin();

int CaptureInit( capture_desc_t * cadesc )
{
	capturePlugin->actv = (media_capture_plugin_t*)capturePlugin->head->data;
	return (capturePlugin->actv->init(cadesc));
}

int CaptureGetFrame( frame_desc_t * fdesc )
{
	if(!capturePlugin->actv)
		return -1;
	return (capturePlugin->actv->getframe(fdesc));
}

int CaptureReleaseFrame()
{
	if(!capturePlugin->actv)
		return -1;
	return (capturePlugin->actv->releaseframe());
}

int CaptureFinish( void )
{
	if(!capturePlugin->actv)
		return -1;
	return (capturePlugin->actv->finish());
}



