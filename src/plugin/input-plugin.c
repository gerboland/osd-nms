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
 * plugin input support module.
 *
 * REVISION:
 * 
 * 3) Proper handling of dm320 locked status -------------- 2007-05-25 nerochiaro
 * 2) Modified plugin controls structure. ----------------- 2006-01-10 MG
 * 1) Initial creation. ----------------------------------- 2005-09-23 MG 
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include "nmsplugin.h"
#include "plugin-internals.h"

//#define OSD_DBG_MSG
#include "nc-err.h"

media_input_plugin_ctrl_t *  inputPlugin = 
  (media_input_plugin_ctrl_t *)InputPlugin();
audio_decode_plugin_ctrl_t * adecodePlugin = 
  (audio_decode_plugin_ctrl_t *)AudioDecodePlugin();

/**
 * Check to see if file has associated plugin.
 *
 * @param file
 *        file name.
 * @param params
 *        media parameters.
 * @return
 *        1 if file is supported, otherwise 0.
 */
int
InputIsOurFile( const char * file )
{
	slist_t * head;
	media_input_plugin_t * ip;
	
	head = inputPlugin->head;
	while (head)
	{
		ip = (media_input_plugin_t*)head->data;
		DBGLOG("checking input plugin - %s", ip->brief);
		if ( ip->isOurFile(file) ) 
		{
			inputPlugin->actv = ip;
			if(OutputSelect(ip->type)) return 0;
			else return 1;
		}
		else head = head->next;
	}
	
	return 0;
}

/**
 * Check to initialize current input plugin.
 *
 * @params params
 *         media parameters.
 * @return
 *         0 if successful, 1 is dm320 locked, otherwise errors.
 */
int
InputInit( const char *filename , media_desc_t * mdesc )
{
	slist_t * head;
	audio_decode_plugin_t * adp;
	int target_codec;
	int status = -1;

	DBGLOG("fetching paramters...");
#if BUILD_TARGET_ARM
	status = inputPlugin->actv->init(filename,mdesc);
	if (0 == status)
	{		
		if (mdesc->vdesc.video_type == NMS_VC_JPEG)
		{
			return 0;
		}
		
		adecodePlugin->actv = NULL;
		if (NMS_AC_NO_AUDIO == mdesc->adesc.audio_type) 
		{
			return 0;
		}
		
		/* grouping codecs.*/
		switch (mdesc->adesc.audio_type)
		{
		case NMS_AC_ARM_MP1:
		case NMS_AC_ARM_MP2:
		case NMS_AC_ARM_MP3:
			target_codec = NMS_AC_ARM_MP3;
			break;
		case NMS_AC_G726:
			target_codec = NMS_AC_G726;
			break;
		default: 
			target_codec = mdesc->adesc.audio_type;
			break;
		}
		
		DBGLOG("searching for audio codec...");
		//DBGLOG("target codec = %d", target_codec);
		/* search for audio codec plugin. */
		head = adecodePlugin->head;
		while (head)
		{
			adp = (audio_decode_plugin_t*)head->data;
			//DBGLOG("\tplugin codec = %d", adp->codec);
			if ( adp->codec == target_codec ) 
			{
				adecodePlugin->actv = adp;
				if (adp->init(&mdesc->adesc))
				{
					adecodePlugin->actv = NULL;
					return -1;
				}
				return 0;
			}
			else head = head->next;
		}
		DBGLOG("unable to find audio codec!");
		/* mute audio and play back video.*/
		return 0;
	}
	else
		inputPlugin->actv = NULL;
#endif /*BUILD_TARGET_ARM*/
	return status;
}

/**
 * Finish current input interface
 */
void
InputFinish( void )
{
	if (adecodePlugin->actv) 
		adecodePlugin->actv->finish();
	if (inputPlugin->actv)
		inputPlugin->actv->finish();
}

/**
 * Start current input interface
 *
 * @param filename
 *        input file name.
 * @param params
 *        media parameters.
 * @param return
 *        0 if successful, otherwise nonzero.
 */
int
InputStart( const char * filename)
{
    return inputPlugin->actv->start(filename);
}

/**
 * Seek to give time in mili-second. 
 *
 * @param time
 *        pointer to time stamp in mili-seconds.
 * @return
 *        actual time stamp if successful, otherwise negative value.
 */
int
InputSeek( int time )
{
	return inputPlugin->actv->seek(time);
}

/**
 * Get media data, return length in bytes.
 *
 * @param buf
 *        media buffer.
 * @param params
 *        media parameters.
 * @return
 *        available data length, -1 to indicate EOF.
 */
int
InputGetData( media_buf_t * buf )
{
	int bytes;
	int eof;
#if BUILD_TARGET_ARM
	do
	{
		DBGLOG("get data from plugin.");
		bytes = inputPlugin->actv->getData(buf, &eof);
		DBGLOG("bytes returned = %d\n", bytes);
		
		if ((buf->curbuf == &buf->abuf) && (bytes>0))
		{
			if (adecodePlugin->actv && buf->abuf.data) 
			{
				/* copy the audio parameters carried by the vbuf over */
				q_buf_t * aud = &buf->abuf;
				q_buf_t * vid = &buf->vbuf;
				
				//DBGLOG("abuf size: %d,\tvbuf size: %d", aud->size, vid->size);
				aud->tsms = vid->tsms;			
				aud->sample_rate = vid->sample_rate;
				//aud->bytes_per_sec = vid->bytes_per_sec;
				// do not modify the audio buffer size.
				//aud->size = vid->size;
				
				bytes = adecodePlugin->actv->decode(&buf->vbuf,
													&buf->abuf);
				DBGLOG("bytes decoded = %d\n", bytes);
			}
			else if (buf->abuf.data)
			{
				void * abuffer = buf->abuf.data;
				DBGLOG("DSP side audio data...");
				memcpy(&buf->abuf, &buf->vbuf, sizeof(buf->vbuf));
				/* audio decoding is handled by the output, copy data over.*/
				buf->abuf.data = abuffer;
				memcpy(buf->abuf.data, buf->vbuf.data, buf->vbuf.size);
			}
		}
		//} while ((!eof) && (0 == bytes));
	} while(0);
#endif /*BUILD_TARGET_ARM*/
	if (eof && !bytes) bytes = -1;
	return bytes;
}

/**
 * Get media info.
 *
 * @param filename
 *        input file name.
 * @param minfo
 *        input media_info_t struct.
 * @return 
 *        0 if get info success,otherwise failed
 */
int
InputGetInfo( const char * filename, void * minfo)
{
	slist_t * head;
	media_input_plugin_t * ip;
	
	head = inputPlugin->head;
	while (head)
	{
		ip = (media_input_plugin_t*)head->data;
		//DBGLOG("checking input plugin - %s", ip->brief);
		if ( ip->isOurFile(filename) ) 
		{
			return ip->getInfo(filename, (media_info_t*)minfo);
		}
		else head = head->next;
	}
	
	return -1;
}

/**
 * Get input plugin capability.
 *
 * @param cap
 *        capability buffer.
 * @return
 *        0 if successful, -1 otherwise.
 */
int
InputGetCapability( input_capability_t * cap )
{
	return inputPlugin->actv->getCapability(cap);
}


/******************************************************************/
/*--------------------- encoder interface. -----------------------*/
/******************************************************************/
media_enc_input_plugin_ctrl_t * encInputPlugin = 
  (media_enc_input_plugin_ctrl_t *)EncInputPlugin();

extern media_enc_output_plugin_ctrl_t * encOutputPlugin;
extern audio_encode_plugin_ctrl_t * aencodePlugin;
static int inNTSC_PAL;

/**
 * Select encoder input plugin for given output type.
 *
 * @param type
 *        output type.
 * @return
 *        0 if encoder input plugin found, otherwise nonzero.
 */
int
EncInputSelect( int type )
{
	slist_t * head;
	media_enc_input_plugin_t * eip;

	DBGLOG("selecting input.");
	head = encInputPlugin->head;
	while (head)
	{
		eip = (media_enc_input_plugin_t*)head->data;
		if (type == eip->type)
		{
			encInputPlugin->actv = eip;
			return 0;
		}
		else head = head->next;
	}

	return -1;
}

/**
 * Initialize input interface.
 *
 * @param params
 *        media parameters.
 * @param is_pal
 *		0 : ntsc output, 1 : pal output
 * @return
 *        0 if successful.
 */
int EncInputInit(const media_desc_t * mdesc, int is_pal)
{
    return encInputPlugin->actv->init(mdesc, is_pal);
}

/** Start encoder input.
 *
 * @return
 *       0 if successful.
 */
int EncInputStart( void )
{
    return encInputPlugin->actv->start();
}

/**
 * Finish encoder input interface.
 */
void EncInputFinish( void )
{
    return encInputPlugin->actv->finish();
}

/**
 * Set input mode
 * @param is_pal
 *	0 : ntsc input, 1 : pal input
 */
void EncInputSetMode(int is_pal)
{
	inNTSC_PAL = is_pal;
}

/**
 * Get input mode
 * @return
 *	0 : ntsc input, 1 : pal input
 */
int EncInputGetMode(void)
{
	return inNTSC_PAL;
}

#define FRAMEBUFFER_SIZE (200 * 1024)
static uint8_t framebuffer[FRAMEBUFFER_SIZE];
/**
 * Get intput data buffers.
 * 
 * @param av
 *        1: audio, 0: video
 * @param buf
 *        media buffer.
 * @param timeout
 *        polling timeout value.
 * @return
 *        0 if successful.
 */
int
EncInputGetBuffer(int av, media_buf_t * buf, int timeout)
{
	int ret = -1;
#if BUILD_TARGET_ARM
    if (av)
	{
		int bytes;
		if (0 == encInputPlugin->actv->getAudioBuffer(buf, timeout))
		{
			bytes = buf->abuf.size;
			//DBGLOG("bytes returned = %d", bytes);
#if 0
			if (bytes > 8192)
			{
				WARNLOG("buffer is too big");
				return ret;
			}
#endif
			if ((bytes>0))
			{
				//DBGLOG("is audio encoder available?");
				if (aencodePlugin->actv) 
				{
					q_buf_t * aud = &buf->abuf;
					q_buf_t aout;
					
					//DBGLOG("abuf size: %d,\taout size: %d", aud->size, FRAMEBUFFER_SIZE);
					aout.data = &framebuffer[0];
					aout.size = FRAMEBUFFER_SIZE;
					
					bytes = aencodePlugin->actv->encode(aud, &aout);
					/* remap audio buffer. */
					aud->data = aout.data;
					aud->size = aout.size;
					aud->tsms = aout.tsms;
					//DBGLOG("bytes encoded to: %d", bytes);
					if (aout.size > FRAMEBUFFER_SIZE) ERRLOG("input plugin buffer overflow");
					ret = 0;
				}
			}
			else WARNLOG("empty audio buffer.");
		}
	}
	else
		ret = encInputPlugin->actv->getVideoBuffer(buf, timeout);
#endif /*BUILD_TARGET_ARM*/
	return ret;
}

/**
 * Release intput data buffers.
 *
 * @param av
 *        1: audio, 0: video
 * @param buf
 *        media buffer.
 */
void
EncInputPutBuffer(int av, const media_buf_t * buf)
{
    av?
		encInputPlugin->actv->putAudioBuffer(buf):
		encInputPlugin->actv->putVideoBuffer(buf);
}

/**
 * Get current recording gain.
 *
 * @param left
 *        left channel gain.
 * @param right
 *        right channel gain.
 */
void
EncInputGetGain( int * left, int * right )
{
	encInputPlugin->actv->getGain(left, right);
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
EncInputSetGain( int left, int right )
{
	encInputPlugin->actv->setGain(left, right);
}
