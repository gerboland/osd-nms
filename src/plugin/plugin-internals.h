#ifndef NMS_PLUGIN_CTRL_H
#define NMS_PLUGIN_CTRL_H
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
 * plugin control module header.
 *
 * REVISION:
 * 
 * 4) Added support for setting output proportions -------- 2008-04-10 nerochiaro 
 * 3) Added in background preference support. ------------- 2007-08-07 MG
 * 2) Added in encoder data structures. ------------------- 2006-01-09 MG
 * 1) Initial creation. ----------------------------------- 2005-10-13 MG 
 *
 */

#include "list.h"
#include "nmsplugin.h"

typedef struct
{
	slist_t *               head;
	slist_t *               tail;  
	void  *                 actv;
} media_plugin_ctrl_t;

typedef struct
{
	slist_t *               head;
	slist_t *               tail;  
	media_input_plugin_t *  actv;
} media_input_plugin_ctrl_t;

typedef struct
{
	slist_t *                head;
	slist_t *                tail;
	media_output_plugin_t *  actv;
} media_output_plugin_ctrl_t;

typedef struct
{
	slist_t *                head;
	slist_t *                tail;
	audio_decode_plugin_t *  actv;
} audio_decode_plugin_ctrl_t;

typedef struct
{
	slist_t *               head;
	slist_t *               tail;  
	media_enc_input_plugin_t *  actv;
} media_enc_input_plugin_ctrl_t;

typedef struct
{
	slist_t *                head;
	slist_t *                tail;
	media_enc_output_plugin_t *  actv;
} media_enc_output_plugin_ctrl_t;

typedef struct
{
	slist_t *                head;
	slist_t *                tail;
	audio_encode_plugin_t *  actv;
} audio_encode_plugin_ctrl_t;

typedef struct
{
	slist_t *                head;
	slist_t *                tail;
	media_capture_plugin_t *  actv;
} media_capture_plugin_ctrl_t;


/** Generic decoder status report structure. */
typedef struct
  {
	/** decoder bitmap status flags. */
	int flags;
} decoder_status_t;


/* plugin control references.
 * Do NOT change this reference without looking at "PLUGIN_TAB" 
 * definition in nms-plugin.c.
 */
extern media_plugin_ctrl_t mediaPlugins[];
#define InputPlugin()       (&mediaPlugins[0])
#define OutputPlugin()      (&mediaPlugins[1])
#define AudioDecodePlugin() (&mediaPlugins[2])
#define EncInputPlugin()    (&mediaPlugins[3])
#define EncOutputPlugin()   (&mediaPlugins[4])
#define AudioEncodePlugin() (&mediaPlugins[5])
#define CapturePlugin()     (&mediaPlugins[6])


int             PluginLoad(void);
void            PluginUnload(void);

int             InputIsOurFile(const char *);
int             InputInit(const char *,media_desc_t*);
void            InputFinish(void);
int             InputStart(const char*);
int             InputGetData(media_buf_t*);
int             InputSeek(int);
int             InputGetInfo(const char *, void *);
int             InputGetCapability(input_capability_t *);

int             OutputSelect(int);
int             OutputInit(const media_desc_t*, int, int);
void            OutputFinish(int);
void            OutputStart(void);
int             OutputGetBuffer(media_buf_t*, int, int);
void            OutputWrite(const media_buf_t*);
void            OutputPause(int);
void            OutputMute(int);
int 			OutputActivateMode(int);
int 			OutputGetMode(void);
void			OutputSetMode(int mode);
void            OutputGetVolume(int *, int *);
void            OutputSetVolume(int, int);
int             OutputGetPlaytime(void);
void            OutputFlush(int);
unsigned long   OutputGetBufferedSize(void);
int             EncOutputFinish( void );
int             EncInputSelect(int);
int		EncInputGetMode(void);
void		EncInputSetMode(int);
void            EncInputGetGain(int*, int*);
void            EncInputSetGain(int, int);
int             EncInputStart(void);
void            EncInputFinish(void);
int             EncInputGetBuffer(int, media_buf_t *, int);
void            EncInputPutBuffer(int, const media_buf_t *);
int				EncInputInit(const media_desc_t * mdesc, int is_pal);

int             EncOutputIsOurFormat(rec_ctrl_t*, void*,media_desc_t *);
void            EncOutputGetRequirements( encoding_requirements_t * requirements );
int             EncOutputInit(const media_desc_t *);
int             EncOutputStart(void);
int             EncOuputFinish(void);
int             EncOutputCommit(media_buf_t *, int);
void            EncOutputGetRequirements( encoding_requirements_t * requirements );

int             CaptureInit( capture_desc_t * cadesc );
int             CaptureGetFrame( frame_desc_t * fdesc );
int             CaptureReleaseFrame();
int             CaptureFinish( void );



#endif
