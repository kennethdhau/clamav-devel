/*
 *  Copyright (C) 2002, 2003 Tomasz Kojm <zolw@konarski.edu.pl>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>

#include "cfgfile.h"
#include "others.h"
#include "defaults.h"
#include "scanner.h"
#include "server.h"
#include "clamuko.h"
#include "tests.h"

#define THREXIT					    \
    close(ths[tharg->sid].desc);		    \
    ths[tharg->sid].active = 0;			    \
    /* this mutex is rather useless */		    \
    /* pthread_mutex_unlock(&ths[tharg->sid].mutex);   */ \
    free(tharg);				    \
    return NULL

#define CMD1 "SCAN"
#define CMD2 "RAWSCAN"
#define CMD3 "QUIT"
#define CMD4 "RELOAD"
#define CMD5 "PING"
#define CMD6 "CONTSCAN"
#define CMD7 "VERSION"
#define CMD8 "STREAM"

#ifdef CLAMUKO
pthread_t clamukoid;
#endif

void *threadscanner(void *arg)
{
	struct thrarg *tharg = (struct thrarg *) arg;
	char buff[32769];
	sigset_t sigset;
	int bread, options, maxwait = CL_DEFAULT_MAXWHILEWAIT;


    /* ignore all signals */
    sigfillset(&sigset);
    pthread_sigmask(SIG_SETMASK, &sigset, NULL);

    if(reload) {
	logg("*Session(%d): database reloading (waiting).\n", tharg->sid);
	ths[tharg->sid].reload = 1;
	while(reload && maxwait--) /* wait during reloading */
	    sleep(1);

	if(!maxwait && reload) {
	    logg("!Database reloading failed, time exceeded. Forcing quit.\n");
	    kill(progpid, SIGTERM);
	}

	ths[tharg->sid].reload = 0;
	logg("*Session(%d): database reloaded.\n", tharg->sid);
    }

    if((bread = read(ths[tharg->sid].desc, buff, 1024)) == -1) {
	logg("!Session(%d): read() failed.\n", tharg->sid);
	THREXIT;
    }

    buff[bread] = 0;
    chomp(buff);

    if(!strncmp(buff, CMD1, strlen(CMD1))) { /* SCAN */
	scan(buff + strlen(CMD1) + 1, NULL, tharg->root, tharg->limits, tharg->options, tharg->copt, ths[tharg->sid].desc, 0);

    } else if(!strncmp(buff, CMD2, strlen(CMD2))) { /* RAWSCAN */
	options = tharg->options & ~CL_ARCHIVE;
	scan(buff + strlen(CMD2) + 1, NULL, tharg->root, NULL, options, tharg->copt, ths[tharg->sid].desc, 0);

    } else if(!strncmp(buff, CMD3, strlen(CMD3))) { /* QUIT */
	if(!progexit)
	    kill(progpid, SIGTERM);

    } else if(!strncmp(buff, CMD4, strlen(CMD4))) { /* RELOAD */
	mdprintf(ths[tharg->sid].desc, "RELOADING\n");
	reload = 1;

    } else if(!strncmp(buff, CMD5, strlen(CMD5))) { /* PING */
	mdprintf(ths[tharg->sid].desc, "PONG\n");

    } else if(!strncmp(buff, CMD6, strlen(CMD6))) { /* CONTSCAN */
	scan(buff + strlen(CMD6) + 1, NULL, tharg->root, tharg->limits, tharg->options, tharg->copt, ths[tharg->sid].desc, 1);

    } else if(!strncmp(buff, CMD7, strlen(CMD7))) { /* VERSION */
	mdprintf(ths[tharg->sid].desc, "clamd / ClamAV version "VERSION"\n");

    } else if(!strncmp(buff, CMD8, strlen(CMD8))) { /* STREAM */
	scanstream(ths[tharg->sid].desc, NULL, tharg->root, tharg->limits, tharg->options, tharg->copt);
    }


    THREXIT;
}

/* this function takes care for threads, exit and various checks */

void *threadwatcher(void *arg)
{
	struct thrwarg *thwarg = (struct thrwarg *) arg;
	struct cfgstruct *cpt;
	sigset_t sigset;
	int i, j, ret, maxwait;
	unsigned long int timer = 0;
	unsigned int timeout, threads, virnum, selfchk;
	short int need_wait = 0, do_loop = 0, db_problem = 0;
	const char *dbdir;
	struct cl_stat dbstat;


    /* ignore all signals */
    sigfillset(&sigset);
    pthread_sigmask(SIG_SETMASK, &sigset, NULL);

#ifdef C_LINUX
    logg("*ThreadWatcher: Started in process %d\n", getpid());
#endif

    if((cpt = cfgopt(thwarg->copt, "MaxThreads")))
	threads = cpt->numarg;
    else
	threads = CL_DEFAULT_MAXTHREADS;

    if((cpt = cfgopt(thwarg->copt, "SelfCheck")))
	selfchk = cpt->numarg;
    else
	selfchk = CL_DEFAULT_SELFCHECK;

    if(!selfchk) {
	logg("^Self checking disabled.\n");
    } else
	logg("Self checking every %d seconds.\n", selfchk);

    if((cpt = cfgopt(thwarg->copt, "ThreadTimeout")))
	timeout = cpt->numarg;
    else
	timeout = CL_DEFAULT_SCANTIMEOUT;

    if(!timeout) {
	logg("^Timeout disabled.\n");
    } else
	logg("Timeout set to %d seconds.\n", timeout);

    if((cpt = cfgopt(thwarg->copt, "DataDirectory")))
	dbdir = cpt->strarg;
    else
	dbdir = cl_retdbdir();

    memset(&dbstat, 0, sizeof(struct cl_stat));
    cl_statinidir(dbdir, &dbstat);

    for(i = 0; ; i++) {

        if(i == threads)
	    i = 0;

	/* check time */
        if(ths[i].active) /* races are harmless here (timeout is re-set) */
	    if(time(NULL) - ths[i].start > timeout) {
		pthread_cancel(ths[i].id);
		mdprintf(ths[i].desc, "Session(%d): Time out ERROR\n", i);
		close(ths[i].desc);
		logg("Session %d stopped due to timeout.\n", i);
		ths[i].active = 0;
//		pthread_mutex_unlock(&ths[i].mutex);
	    }

	/* cancel all threads in case of quit */
	if(progexit == 1) {
#ifdef CLAMUKO
	    /* stop clamuko */
	    if(clamuko_running) {
		logg("Stopping Clamuko... (id %d)\n", clamukoid);
		pthread_kill(clamukoid, SIGUSR1);
		/* we must wait for Dazuko unregistration */
		maxwait = CL_DEFAULT_MAXWHILEWAIT * 5;
		while(clamuko_running && maxwait--)
		    usleep(200000);

		if(!maxwait && clamuko_running)
		    logg("!Critical error: Can't stop Clamuko.\n");
	    }
#endif

	    for(j = 0; j < threads; j++)
		if(ths[j].active) {
		    pthread_cancel(ths[j].id);
		    mdprintf(ths[j].desc, "Session(%d): Stopped (exiting)\n", j);
		    close(ths[j].desc);
		    logg("Session %d stopped (exiting).\n", j);
//		    pthread_mutex_unlock(&ths[j].mutex);
		}
#ifndef C_BSD
	    logg("*Freeing trie structure.\n");
	    cl_freetrie(*thwarg->root);
#endif
	    logg("*Shutting down the main socket.\n");
	    shutdown(thwarg->socketd, 2);
	    logg("*Closing the main socket.\n");
	    close(thwarg->socketd);
	    if((cpt = cfgopt(thwarg->copt, "LocalSocket"))) {
		if(unlink(cpt->strarg) == -1)
		    logg("!Can't unlink the socket file %s\n", cpt->strarg);
		else
		    logg("Socket file removed.\n");
	    }

	    if((cpt = cfgopt(thwarg->copt, "PidFile"))) {
		if(unlink(cpt->strarg) == -1)
		    logg("!Can't unlink the pid file %s\n", cpt->strarg);
		else
		    logg("Pid file removed.\n");
	    }

	    progexit = 2;
	    logg("*Exit level %d, ThreadWatcher termination.\n", progexit);
	    return NULL;
	}


	/* do self checks */
	if(selfchk && (db_problem || !(timer % selfchk))) {
	    /* check the integrity of the database */
	    if(!reload) {

		if(cl_statchkdir(&dbstat) == 1) {
		    logg("SelfCheck: Database modification detected. Forcing reload.\n");
		    reload = 1;
		    cl_statfree(&dbstat);
		    cl_statinidir(dbdir, &dbstat);
		} else
		    logg("SelfCheck: Database status OK.\n");

		if(!testsignature(*thwarg->root)) {
		    if(db_problem) {
			logg("!SelfCheck: Unable to repair internal structure. Exiting.\n");
			kill(progpid, SIGTERM);
			continue;
		    }
		    /* oops */
		    logg("!SelfCheck: Unable to detect test signature, forcing database reload.\n");
		    db_problem = 1;
		    reload = 1;
		} else {
		    logg("*SelfCheck: Integrity OK\n");
		    db_problem = 0;
		}
	    }
	}

	timer++;

	/* reload the database(s) */
	if(reload) {
	    do {
		need_wait = 0;
		for(j = 0; j < threads; j++)
		    if(ths[j].active) {
			if(time(NULL) - ths[j].start > timeout) {
			    do_loop = 1;
			    break;
			} else if(!ths[j].reload)
			    need_wait = 1;
		    }

#ifdef CLAMUKO
		if(clamuko_scanning && !clamuko_reload)
		    need_wait = 1;
#endif

		if(need_wait)
		    usleep(200000);

		if(progexit == 1)
		    break;

	    } while(need_wait);

	    if(progexit == 1) {
		reload = 0;
		continue;
	    }

	    if(do_loop) {
		/* some threads must be stopped in the next iteration,
		 * reload is still == 1
		 */
		do_loop = 0;
		continue;
	    }

	    /* relase old structure */
	    cl_freetrie(*thwarg->root);
	    *thwarg->root = NULL;

	    /* reload */

	    logg("Reading databases from %s\n", dbdir);

	    if((ret = cl_loaddbdir(dbdir, &*thwarg->root, &virnum))) {
		logg("!%s\n", cl_strerror(ret));
		kill(progpid, SIGTERM);
		/* we stay in reload == 1, so all threads are waiting */
		continue;
	    }

	    if(! *thwarg->root) {
		logg("!Database initialization problem.\n");
		kill(progpid, SIGTERM);
	    } else {
		cl_buildtrie(*thwarg->root);
		/* check integrity */

		logg("Database correctly reloaded (%d viruses)\n", virnum);
	    }

	    reload = 0;
	}

	sleep(1);
    }

    return NULL;
}

int threads;
pthread_t watcherid;

int acceptloop(int socketd, struct cl_node *root, const struct cfgstruct *copt)
{
	int acceptd, i, options = 0, maxwait;
	struct cfgstruct *cpt;
	struct thrarg *tharg;
	struct thrwarg thwarg;
	struct cl_limits limits;
	pthread_attr_t thattr;
	struct sigaction sigact, sigsegvact;
	sigset_t sigset;
	mode_t old_umask;

#if defined(C_BIGSTACK) || defined(C_BSD)
	size_t stacksize;
#endif


    /* save the PID */
    if((cpt = cfgopt(copt, "PidFile"))) {
	    FILE *fd;
	    old_umask = umask(0006);
	if((fd = fopen(cpt->strarg, "w")) == NULL) {
	    logg("!Can't save PID in file %s\n", cpt->strarg);
	} else {
	    fprintf(fd, "%d", getpid());
	    fclose(fd);
	}
	umask(old_umask);
    }

    logg("*Listening daemon: PID: %d\n", getpid());

    if((cpt = cfgopt(copt, "MaxThreads")))
	threads = cpt->numarg;
    else
	threads = CL_DEFAULT_MAXTHREADS;

    logg("Maximal number of threads: %d\n", threads);

    ths = (struct thrsession *) mcalloc(threads, sizeof(struct thrsession));
 
    for(i = 0; i < threads; i++)
	pthread_mutex_init(&ths[i].mutex, NULL);


    if(cfgopt(copt, "ScanArchive") || cfgopt(copt, "ClamukoScanArchive")) {

	/* set up limits */

	if((cpt = cfgopt(copt, "ArchiveMaxFileSize"))) {
	    if((limits.maxfilesize = cpt->numarg))
		logg("Archive: Archived file size limit set to %d bytes.\n", limits.maxfilesize);
	    else
		logg("^Archive: File size limit protection disabled.\n");
	} else {
	    limits.maxfilesize = 10485760;
	    logg("^USING HARDCODED LIMIT: Archive: Archived file size limit set to %d bytes.\n", limits.maxfilesize);
	}

	if((cpt = cfgopt(copt, "ArchiveMaxRecursion"))) {
	    if((limits.maxreclevel = cpt->numarg))
		logg("Archive: Recursion level limit set to %d.\n", limits.maxreclevel);
	    else
		logg("^Archive: Recursion level limit protection disabled.\n");
	} else {
	    limits.maxreclevel = 5;
	    logg("^USING HARDCODED LIMIT: Archive: Recursion level set to %d.\n", limits.maxreclevel);
	}

	if((cpt = cfgopt(copt, "ArchiveMaxFiles"))) {
	    if((limits.maxfiles = cpt->numarg))
		logg("Archive: Files limit set to %d.\n", limits.maxfiles);
	    else
		logg("^Archive: Files limit protection disabled.\n");
	} else {
	    limits.maxfiles = 1000;
	    logg("^USING HARDCODED LIMIT: Archive: Files limit set to %d.\n", limits.maxfiles);
	}

	if(cfgopt(copt, "ArchiveLimitMemoryUsage")) {
	    limits.archivememlim = 1;
	    logg("Archive: Limited memory usage.\n", limits.maxfiles);
	} else
	    limits.archivememlim = 0;
    }

    if(cfgopt(copt, "ScanArchive")) { 
	logg("Archive support enabled.\n");
	options |= CL_ARCHIVE;
    } else {
	logg("Archive support disabled.\n");
    }

    if(cfgopt(copt, "ScanMail")) { 
	logg("Mail files support enabled.\n");
	options |= CL_MAIL;
    } else {
	logg("Mail files support disabled.\n");
    }

    /* initialize important global variables */
    progexit = 0;
    progpid = 0;
    reload = 0;
#ifdef CLAMUKO
    clamuko_scanning = 0;
    clamuko_running = 0;
#endif

    pthread_attr_init(&thattr);
    pthread_attr_setdetachstate(&thattr, PTHREAD_CREATE_DETACHED);

    /* run clamuko */
    if(cfgopt(copt, "ClamukoScanOnLine"))
#ifdef CLAMUKO
    {
	tharg = (struct thrarg *) mcalloc(1, sizeof(struct thrarg));
	tharg->copt = copt;
	tharg->root = root;
	tharg->limits = &limits;
	tharg->options = options;
	clamuko_reload = 0;

	pthread_create(&clamukoid, &thattr, clamukoth, tharg);
    }
#else
	logg("!Clamuko is not available.\n");
#endif

    /* start thread watcher */
    thwarg.socketd = socketd;
    thwarg.copt = copt;
    thwarg.root = &root;
    pthread_create(&watcherid, &thattr, threadwatcher, &thwarg);

    /* set up signal handling */

    sigfillset(&sigset);
    sigdelset(&sigset, SIGINT);
    sigdelset(&sigset, SIGTERM);
    sigdelset(&sigset, SIGSEGV);
    sigprocmask(SIG_SETMASK, &sigset, NULL);

    /* SIGINT, SIGTERM, SIGSEGV */
    sigact.sa_handler = sigexit;
    sigsegvact.sa_handler = sigsegv;
    sigemptyset(&sigact.sa_mask);
    sigemptyset(&sigsegvact.sa_mask);
    sigaddset(&sigact.sa_mask, SIGINT);
    sigaddset(&sigact.sa_mask, SIGTERM);
    sigaddset(&sigsegvact.sa_mask, SIGSEGV);
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGSEGV, &sigsegvact, NULL);

    /* we need to save program's PID, because under Linux each thread
     * has another PID, it works with other OSes as well
     */
    progpid = getpid();

#if defined(C_BIGSTACK) || defined(C_BSD)
    /*
     * njh@bandsman.co.uk:
     * libclamav/scanners.c uses a *huge* buffer
     * (128K not BUFSIZ from stdio.h).
     * We need to allow for that.
     */
    pthread_attr_getstacksize(&thattr, &stacksize);
    cli_dbgmsg("set stacksize to %u\n", stacksize + BUFFSIZE + 64 * 1024);
    pthread_attr_setstacksize(&thattr, stacksize + BUFFSIZE + 64 * 1024);
#endif

    while(1) {

	/* find a free session */
	for(i = 0; ; i++) {
	    if(i == threads)
		i = 0;

	    if(!ths[i].active) {
		/* logg("*Found free slot: %d\n", i); */
		break;
	    }
	}


	if((acceptd = accept(socketd, NULL, NULL)) == -1) {
	    logg("!accept() failed.\n");
	    /* exit ? */
	    continue;
	}


	if(reload) { /* do not start new threads */
	    logg("*Main thread: database reloading (waiting).\n");
	    maxwait = CL_DEFAULT_MAXWHILEWAIT;
	    while(reload && maxwait--)
		sleep(1);

	    if(!maxwait && reload) {
		logg("!Database reloading failed (time exceeded). Exit forced.\n");
		progexit = 1;
		sleep(10);
		exit(1);
	    }

	    logg("*Main thread: database reloaded.\n");
	}

	tharg = (struct thrarg *) mcalloc(1, sizeof(struct thrarg));
	tharg->copt = copt;
	tharg->sid = i;
	tharg->root = root;
	tharg->limits = &limits;
	tharg->options = options;

	//pthread_mutex_lock(&ths[i].mutex);
	ths[i].desc = acceptd;
	ths[i].reload = 0;
	pthread_create(&ths[i].id, &thattr, threadscanner, tharg);
	ths[i].start = time(NULL);
	ths[i].active = 1;
    }
}

void sigexit(int sig)
{
	time_t currtime;
	int maxwait = CL_DEFAULT_MAXWHILEWAIT * 5;

    progexit = 1;
    logg("*Signal %d caught -> exiting.\n", sig);

    while(progexit != 2 && maxwait--)
	usleep(200000);

    if(!maxwait && progexit != 2)
	logg("!Critical error: Cannot reach exit level 2.\n");

    time(&currtime);
    logg("--- Stopped at %s", ctime(&currtime));
    exit(0);
}

void sigsegv(int sig)
{
	int i;

    logg("Segmentation fault :-( Bye..\n");

    for(i = 0; i < threads; i++)
	if(ths[i].active)
	    pthread_kill(ths[i].id, 9);

    pthread_kill(watcherid, 9);
    exit(11); /* probably not reached at all */
}
