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
 * Main App routines.
 *
 * REVISION:
 * 
 *
 * 1) Initial creation. ----------------------------------- 2006-12-06 MG
 *
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include "nc-type.h"

#define OSD_DBG_MSG
#include "nc-err.h"
#include "server-nms.h"

int main(int argc, char** argv) {

	/* Our process ID and Session ID */
	pid_t pid, sid;

	/* Fork off the parent process */
	pid = fork();
	if (pid < 0) {
		exit(EXIT_FAILURE);
	}
	/* If we got a good PID, then
	   we can exit the parent process. */
	if (pid > 0) {
		/* let's sleep for a second before returning, thus leave
		   some time for server to be initialized properly.
		*/
		sleep(1);
		exit(EXIT_SUCCESS);
	}
	
	/* Change the file mode mask */
	umask(0);
    
	/* Open any logs here */        
    
	/* Create a new SID for the child process */
	sid = setsid();
	if (sid < 0) {
		/* Log the failure */
		exit(EXIT_FAILURE);
	}   
	    
	/* Change the current working directory */
	if ((chdir("/")) < 0) {
		/* Log the failure */
		exit(EXIT_FAILURE);
	}

	/* Close all files and remap the standard file descriptors */
#ifndef OSD_DBG_MSG
	{        
		int ii;

		for (ii=getdtablesize();ii>=0;--ii) close(ii); /* close all descriptors */
		ii=open("/dev/null",O_RDWR); /* open stdin */
		dup(ii);                     /* stdout */
		dup(ii);                     /* stderr */
	}
#endif

	/* Daemon-specific initialization goes here */
	if (NmsSrvInit(argc, argv)) 
	{
		EPRINT("NMS initialization failed");
	}
	else
	{
		DBGMSG("nms server started.");
		NmsSrvStart();	
	}
	exit(EXIT_SUCCESS);
}



