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
 * Neuros-Cooler platform nms slideshow server module.
 *
 * REVISION:
 * 
 * 2) Added in background preference support. ------------- 2007-08-07 MG
 * 1) Initial creation. ----------------------------------- 2007-05-29 CL 
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
#include "cooler-core.h"

static int imageShowed = 0;
static int inNTSC_PAL = -1;
static int outNTSC_PAL = -1;
static int errorStatus;
static media_desc_t       mdesc;
static char imageHistory[PATH_MAX];

#define VIDEO_WIDTH  540
#define VIDEO_HEIGHT 360

static int imageIsOurFile( const char *filename )
{
	char *ext;
	
	ext = strrchr(filename, '.');
	if (ext)
	  {
		if ((!strcasecmp(ext, ".jpg")) ||
			(!strcasecmp(ext, ".jpeg")) ||
			(!strcasecmp(ext, ".gif")) ||			
			(!strcasecmp(ext, ".png")) ||
			(!strcasecmp(ext, ".pic")) ||
			(!strcasecmp(ext, ".ico")) ||
			(!strcasecmp(ext, ".bmp")))
		  {
			return 0;
		  }
	  }
	
	return -1;
}

static int getImageData(const char* fname, media_buf_t *buf)
{
	int fsize;
	off_t offset = 0;
	int fd =  -1;

	fd = open(fname, O_RDONLY, 0);
	if (fd < 0)
		return -1;
	if (offset > 0)
	{
		off_t newoffset;
		newoffset = lseek(fd, offset, SEEK_SET);
		if (newoffset == (off_t)-1)
		{
		    DBGLOG(" Seeking to File Offset.\n");
		    goto out;
		}
	}

	fsize = read(fd, buf->vbuf.data, buf->vbuf.size);

	if (fsize < 0)
	{
		DBGLOG("Reading from File '%s'.\n", fname);
		fsize = -1;
	}

	buf->vbuf.size = fsize;
	DBGLOG(" Displaying fname='%s' fsize=%d.\n", fname, fsize);
	buf->curbuf = &buf->vbuf;
out:
	close(fd);
	fd = -1;
	return fsize;
}

static int imageshow(const char *filename)
{
	media_buf_t imgbuf;

	memset(&imgbuf, 0, sizeof(media_buf_t));

	if (1 == OutputGetBuffer(&imgbuf, 0, 0))
	{
		WARNLOG("GetBuffer failed.");
		return -1;
	}

	getImageData(filename, &imgbuf);
	OutputWrite(&imgbuf);

	return 0;
}

static void
stopSlideShow(void)
{
	if (imageShowed)
	{
		// always hides image instead of destroying it.
		OutputFinish(1);
		imageShowed = 0;
	}
}

/**
 *
 */
int
SrvStartSlideShow(void)
{
	int status = -1;

	if ((!imageShowed) ||
		(imageShowed && (inNTSC_PAL != EncInputGetMode())) ||
		(imageShowed && (outNTSC_PAL != OutputGetMode())))
	{
		//FIXME: 1. why do I need to care out the input mode in this case?
		if (imageShowed && 
			((inNTSC_PAL != EncInputGetMode()) ||
			 (outNTSC_PAL != OutputGetMode()))) stopSlideShow();
		
		if (imageIsOurFile(imageHistory)) return status;
		
		if (imageHistory[0] != '\0')
		{
			mdesc.vdesc.video_type = NMS_VC_JPEG;
			mdesc.vdesc.width = VIDEO_WIDTH;
			mdesc.vdesc.height = VIDEO_HEIGHT;
		}
		else
			return status;
		
		OutputSelect(NMS_PLUGIN_MULTIMEDIA);
		status = OutputInit(&mdesc,OutputGetMode(),0); //proportions are always 4:3 for image playback
		switch (status)
		{
		case 0: //everything is fine
			break;
		case 1:
		default:
			WARNLOG("OutputInit: DM320 is locked by something else. Please try again later.");
			errorStatus = NMS_STATUS_OUTPUT_LOCKED;
			return status;
		} 

		status = imageshow(imageHistory);
		imageShowed = 1;
		inNTSC_PAL = EncInputGetMode();
		outNTSC_PAL = OutputGetMode();
	}
	
	return status;
}

/**
 *
 */
void 
SrvSlideShowSetImage(const char* fname)
{
	memset(imageHistory, 0, sizeof(imageHistory));
	strncpy(imageHistory, fname, PATH_MAX);
}

void 
SrvStopSlideShow(void)
{
	stopSlideShow();
}

int 
SrvIsSlideShowActive(void)
{
	return imageShowed;
}

