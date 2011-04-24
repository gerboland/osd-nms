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
 * plugin generic routines.
 *
 * REVISION:
 * 
 * 2) Added in encoder interfaces. ------------------------ 2006-01-10 MG
 * 1) Initial creation. ----------------------------------- 2005-09-23 MG 
 *
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "nmsplugin.h"
#include "plugin-internals.h"
#include "dirtree.h"
#include "nc-config.h"

//#define OSD_DBG_MSG
#include "nc-err.h"


#define NUM_PLUGINS     7

typedef void * (*pf_t) (void);

slist_t * libHead;
slist_t * libTail;

const char * PLUGIN_TAB[NUM_PLUGINS] = 
{ 
	NMS_PLUGIN_SYMBOL_INPUT,
	NMS_PLUGIN_SYMBOL_OUTPUT,
	NMS_PLUGIN_SYMBOL_AUDIODEC,
	NMS_PLUGIN_SYMBOL_ENCINPUT,
	NMS_PLUGIN_SYMBOL_ENCOUTPUT,
	NMS_PLUGIN_SYMBOL_AUDIOENC,
	NMS_PLUGIN_SYMBOL_CAPTURE
};

media_plugin_ctrl_t  mediaPlugins[NUM_PLUGINS];

static void InitPlugins( void )
{
	int ii;
	media_plugin_ctrl_t * p;
	
	p = &mediaPlugins[0];
	for (ii = 0; ii < NUM_PLUGINS; ii++)
	{
		p->head = p->tail = NULL;
		p->actv = NULL;
		p++;
	}
	
	libHead = libTail = NULL;
}

static void AddLib( void * lib )
{
	slist_t * list;
	
	list = CoolSlistNew(lib);
	if(libTail) CoolSlistInsert(libTail, list);
	else libHead = libTail = list;
}

static void LoadPlugin( int plugin, void * ld )
{
	void * p;
	slist_t * list;
	slist_t * head;
	slist_t * tail;
	
	p = (void *) (((pf_t)ld)());
	list = CoolSlistNew(p);
	
	DBGLOG("loading plugin type: %d", plugin);
	head = mediaPlugins[plugin].head;
	tail = mediaPlugins[plugin].tail;
	
	if (tail) tail = CoolSlistInsert(tail, list);
	else {head = tail = list;}
	
	mediaPlugins[plugin].head = head;
	mediaPlugins[plugin].tail = tail;
}

/**
 * Load various plugins to initialize.
 *
 * @return
 *        0 if necessary plugins are loaded, otherwise nonzero.
 */
int 
PluginLoad( void )
{
	dir_node_t node;
	char path[256];
	int i;
	void * lib;
	void * ld;
	
	InitPlugins();
	
	if (CoolBlockOpenDir(NMS_PLUGIN_DIR, &node) > 0)
	{
		CoolFilterDirectory(&node, CoolDirFilterHiddenEntry, DF_ALL);
	}
	else
	{
		ERRLOG("unable to open plugin directory: [%s] .", NMS_PLUGIN_DIR);
	}
	while(node.ffnum--)
	{
		i = node.ffindex[node.ffnum];
		strcpy(path, NMS_PLUGIN_DIR);
		strcat(path, node.namelist[i]->d_name);
		
		lib = dlopen(path, RTLD_LAZY);
		DBGLOG("opening %s", path);
		if (lib)
		{		
			i = 0;
			while (i < NUM_PLUGINS)
			{
				DBGLOG("finding symbol: %s", PLUGIN_TAB[i]);
				ld = dlsym(lib, PLUGIN_TAB[i]);
				if (ld)
				{
					DBGLOG("found symbol: %s", PLUGIN_TAB[i]);
					LoadPlugin(i, ld);
					break;
				}
				else i++;
			}
			if (i == NUM_PLUGINS) 
			{
				WARNLOG("%s: is not our plugin.", path);
				dlclose(lib);
			}
			else AddLib(lib);
		}
		else
		{
			WARNLOG("DLERROR: %s", dlerror());
		}
	}
	
	CoolCloseDirectory(&node);
	
	if ( (mediaPlugins[0].head==NULL)||(mediaPlugins[1].head==NULL) )
	{
		ERRLOG("Input/output plugins are mandatory!");
		return -1;
	}
	DBGLOG("plugins loaded!");
	return 0;
}

/**
 * Unload all plugins.
 */
void 
PluginUnload( void )
{
	slist_t * head;
	int ii;
	
	while (libHead)
	{
		dlclose(libHead->data);
		libHead = CoolSlistRemove(libHead, libHead);
	}
	
	for (ii = 0; ii < NUM_PLUGINS; ii++)
	{
		head = mediaPlugins[ii].head;
		while (head) {head = CoolSlistRemove(head, head);}
	}
}
