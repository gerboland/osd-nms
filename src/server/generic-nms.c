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
 * Neuros-Cooler platform nms generic control interface.
 *
 * REVISION:
 * 
 * 2) Embedded cmd ACK with returned data if any. --------- 2006-04-14 MG
 * 1) Initial creation. ----------------------------------- 2005-09-19 MG 
 *
 */
#include <signal.h>
#include <unistd.h>

//#define OSD_DBG_MSG
#include "nc-err.h"

#include "cmd-nms.h"
#include "com-nms.h"
#include "server-nms.h"
#include "plugin-internals.h"
#include "cooler-core.h"
#include "server-monitor-internal.h"

static int       sessionId;
static int       cmdFd;

/*
 * Set up the command interface.
 *
 * @param sid
 *        session ID.
 * @return
 *        0 if interface successfully set up, otherwise non-zero.
 */
static int
SrvCmdSetup( int sid )
{
	struct sockaddr_un saddr;

	DBGMSG("setting up command interface. ");
    if ((cmdFd = socket(AF_UNIX, SOCK_STREAM, 0)) != -1) 
	{
	    saddr.sun_family = AF_UNIX;
		CoolCmdGetSockPath(sid, saddr.sun_path);
		
		if (bind(cmdFd, (struct sockaddr *) &saddr, sizeof (saddr)) != -1) 
		{
		    listen(cmdFd, 100);
			return 0;
		} 
		else 
		{
		    close(cmdFd);
			cmdFd = 0;
			EPRINT("Unable to bind socket! ");
		}	
	} 
	else 
	{
	    EPRINT("Unable to create socket! ");
	}
	
    return 1;
}

/*
 * Stop the command interface.
 *
 * @param sid
 *        Session ID.
 */
static void
SrvCmdStop( int sid )
{
	if (cmdFd)
	{
 	    char path[200];
		close(cmdFd);
		CoolCmdGetSockPath(sid, path);
		unlink(path);
		cmdFd = 0;
	}
}

/*
 * Start the command interface.
 */
static void
SrvCmdStart( void )
{
	int                fd, len;
	fd_set             set;
	struct timeval     tv;
	struct sockaddr_un saddr;		
	pkt_node_t         pkt;

	while ( 1 )
	{
		//NOTSURE: maybe we should move this out of the loop, see comment in
		//         in client-nms.c.
		FD_ZERO(&set);
		FD_SET(cmdFd, &set);
		tv.tv_sec = 0;
		tv.tv_usec = 100000;
		len = sizeof(saddr);
		if ((select(cmdFd + 1, &set, NULL, NULL, &tv) <= 0) ||
		    ((fd = accept(cmdFd, (struct sockaddr *)&saddr, &len)) == -1))
			continue;
		
		if (sizeof(pkt_hdr_t) != CoolCmdGetAll(fd, &pkt.hdr, sizeof(pkt_hdr_t)))
		{
			WPRINT("Incomplete command dropped! ");
			continue;
		}
		if (pkt.hdr.dataLen)
		{
			pkt.data = (void *)calloc(pkt.hdr.dataLen, sizeof(char));
			if (CoolCmdGetAll(fd, pkt.data, pkt.hdr.dataLen) != pkt.hdr.dataLen)
			{
				free(pkt.data);
				WPRINT("Incomplete data dropped! ");
				continue;
			}
		}
		
		pkt.fd = fd;
		
		if ( 15 == SrvRxCmd((void *)&pkt) ) break;
	}

	// stop server.
	SrvCmdStop(sessionId);
	SrvStopMonitorGarbageCollector();
}

static void signal_handler(int signum)
{
	WARNLOG("------- signal caught:[%d] --------\n", signum);
	exit(0);
}

static void signal_init(void)
{
	struct sigaction sa;
	
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	
    if (sigaction(SIGHUP, &sa, NULL)){}
    if (sigaction(SIGINT, &sa, NULL)){}
    if (sigaction(SIGQUIT, &sa, NULL)){}
    if (sigaction(SIGABRT, &sa, NULL)){}
    if (sigaction(SIGTERM, &sa, NULL)){}
}

/**
 * Initialize NMS server.
 * @param argc The same as nmsd's main() argc
 * @param argv The same as nmsd's main() argv
 * @return
 *        0 if successful, otherwise non-zero.
 */
int 
NmsSrvInit(int argc, char** argv)
{
    char path[108];
    int ii;

	/* load various plugins. */
	PluginLoad();

	signal_init();

	for ( ii = 0;; ii++ ) 
	  {
		CoolCmdGetSockPath(ii, path);
		if ( FALSE == CoolCmdIsSessionRunning(ii) )
		  {
			unlink(path);
		  }
		else if ( NMS_isAllowMultipleInstances() )
		  {
			DBGMSG("Allow multiple instances, continue. ");
			continue;
		  }
		else 
		  {
		    EPRINT("One session is active already! ");
			return 2;
		  }

		// All this monitor setup has to be done before the command interface is started since
		// it's not supposed to be safe to run with incoming requests.
		int outmode;
		if (argc > 1 && strncmp(argv[1], "pal", strlen("pal")) == 0) outmode = 1; //PAL
		else outmode = 0; //NTSC, default
		SrvMonitorInitOutputMode(outmode);
		SrvStartMonitorGarbageCollector();

		DBGMSG("starting command server. ");
		if ( SrvCmdSetup(ii) ) 
		{
			SrvStopMonitorGarbageCollector();
			EPRINT("Unable to start command server! ");
			return 1;
		}
		else break;
	}

	sessionId = ii;
	return 0;
}

/**
 * Start the nms server.
 */
void
NmsSrvStart( void )
{
    SrvCmdStart();
}
