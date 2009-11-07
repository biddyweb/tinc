/*
    tincd.c -- the main file for tincd
    Copyright (C) 1998-2005 Ivo Timmermans
                  2000-2009 Guus Sliepen <guus@tinc-vpn.org>
                  2008      Max Rijevski <maksuf@gmail.com>
                  2009      Michael Tokarev <mjt@tls.msk.ru>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "system.h"

/* Darwin (MacOS/X) needs the following definition... */
#ifndef _P1003_1B_VISIBLE
#define _P1003_1B_VISIBLE
#endif

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#include LZO1X_H

#ifndef HAVE_MINGW
#include <pwd.h>
#include <grp.h>
#include <time.h>
#endif

#include <getopt.h>

#include "conf.h"
#include "control.h"
#include "crypto.h"
#include "device.h"
#include "logger.h"
#include "net.h"
#include "netutl.h"
#include "process.h"
#include "protocol.h"
#include "utils.h"
#include "xalloc.h"

/* The name this program was run with. */
char *program_name = NULL;

/* If nonzero, display usage information and exit. */
bool show_help = false;

/* If nonzero, print the version on standard output and exit.  */
bool show_version = false;

/* If nonzero, use null ciphers and skip all key exchanges. */
bool bypass_security = false;

/* If nonzero, disable swapping for this process. */
bool do_mlock = false;

/* If nonzero, chroot to netdir after startup. */
static bool do_chroot = false;

/* If !NULL, do setuid to given user after startup */
static const char *switchuser = NULL;

/* If nonzero, write log entries to a separate file. */
bool use_logfile = false;

char *identname = NULL;				/* program name for syslog */
char *logfilename = NULL;			/* log file location */
char *controlcookiename = NULL;
char **g_argv;					/* a copy of the cmdline arguments */

static int status;

static struct option const long_options[] = {
	{"config", required_argument, NULL, 'c'},
	{"net", required_argument, NULL, 'n'},
	{"help", no_argument, NULL, 1},
	{"version", no_argument, NULL, 2},
	{"no-detach", no_argument, NULL, 'D'},
	{"debug", optional_argument, NULL, 'd'},
	{"bypass-security", no_argument, NULL, 3},
	{"mlock", no_argument, NULL, 'L'},
	{"chroot", no_argument, NULL, 'R'},
	{"user", required_argument, NULL, 'U'},
	{"logfile", optional_argument, NULL, 4},
	{"controlcookie", required_argument, NULL, 5},
	{NULL, 0, NULL, 0}
};

#ifdef HAVE_MINGW
static struct WSAData wsa_state;
CRITICAL_SECTION mutex;
#endif

static void usage(bool status) {
	if(status)
		fprintf(stderr, "Try `%s --help\' for more information.\n",
				program_name);
	else {
		printf("Usage: %s [option]...\n\n", program_name);
		printf(	"  -c, --config=DIR              Read configuration options from DIR.\n"
				"  -D, --no-detach               Don't fork and detach.\n"
				"  -d, --debug[=LEVEL]           Increase debug level or set it to LEVEL.\n"
				"  -n, --net=NETNAME             Connect to net NETNAME.\n"
				"  -L, --mlock                   Lock tinc into main memory.\n"
				"      --logfile[=FILENAME]      Write log entries to a logfile.\n"
				"      --controlcookie=FILENAME  Write control socket cookie to FILENAME.\n"
				"      --bypass-security         Disables meta protocol security, for debugging.\n"
				"  -R, --chroot                  chroot to NET dir at startup.\n"
				"  -U, --user=USER               setuid to given USER at startup.\n"				"      --help                    Display this help and exit.\n"
				"      --version                 Output version information and exit.\n\n");
		printf("Report bugs to tinc@tinc-vpn.org.\n");
	}
}

static bool parse_options(int argc, char **argv) {
	int r;
	int option_index = 0;

	while((r = getopt_long(argc, argv, "c:DLd::n:RU:", long_options, &option_index)) != EOF) {
		switch (r) {
			case 0:				/* long option */
				break;

			case 'c':				/* config file */
				confbase = xstrdup(optarg);
				break;

			case 'D':				/* no detach */
				do_detach = false;
				break;

			case 'L':				/* no detach */
#ifndef HAVE_MLOCKALL
				logger(LOG_ERR, "%s not supported on this platform", "mlockall()");
				return false;
#else
				do_mlock = true;
				break;
#endif

			case 'd':				/* inc debug level */
				if(optarg)
					debug_level = atoi(optarg);
				else
					debug_level++;
				break;

			case 'n':				/* net name given */
				netname = xstrdup(optarg);
				break;

			case 'R':				/* chroot to NETNAME dir */
				do_chroot = true;
				break;

			case 'U':				/* setuid to USER */
				switchuser = optarg;
				break;

			case 1:					/* show help */
				show_help = true;
				break;

			case 2:					/* show version */
				show_version = true;
				break;

			case 3:					/* bypass security */
				bypass_security = true;
				break;

			case 4:					/* write log entries to a file */
				use_logfile = true;
				if(optarg)
					logfilename = xstrdup(optarg);
				break;

			case 5:					/* open control socket here */
				controlcookiename = xstrdup(optarg);
				break;

			case '?':
				usage(true);
				return false;

			default:
				break;
		}
	}

	return true;
}

/*
  Set all files and paths according to netname
*/
static void make_names(void) {
#ifdef HAVE_MINGW
	HKEY key;
	char installdir[1024] = "";
	long len = sizeof installdir;
#endif

	if(netname)
		xasprintf(&identname, "tinc.%s", netname);
	else
		identname = xstrdup("tinc");

#ifdef HAVE_MINGW
	if(!RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\tinc", 0, KEY_READ, &key)) {
		if(!RegQueryValueEx(key, NULL, 0, 0, installdir, &len)) {
			if(!logfilename)
				xasprintf(&logfilename, "%s/log/%s.log", identname);
			if(!confbase) {
				if(netname)
					xasprintf(&confbase, "%s/%s", installdir, netname);
				else
					xasprintf(&confbase, "%s", installdir);
			}
			if(!controlcookiename)
				xasprintf(&controlcookiename, "%s/cookie", confbase);
		}
		RegCloseKey(key);
		if(*installdir)
			return;
	}
#endif

	if(!logfilename)
		xasprintf(&logfilename, LOCALSTATEDIR "/log/%s.log", identname);

	if(netname) {
		if(!confbase)
			xasprintf(&confbase, CONFDIR "/tinc/%s", netname);
		else
			logger(LOG_INFO, "Both netname and configuration directory given, using the latter...");
	} else {
		if(!confbase)
			xasprintf(&confbase, CONFDIR "/tinc");
	}
}

static void free_names() {
	if (identname) free(identname);
	if (netname) free(netname);
	if (controlcookiename) free(controlcookiename);
	if (logfilename) free(logfilename);
	if (confbase) free(confbase);
}

static bool drop_privs() {
#ifdef HAVE_MINGW
	if (switchuser) {
		logger(LOG_ERR, "%s not supported on this platform", "-U");
		return false;
	}
	if (do_chroot) {
		logger(LOG_ERR, "%s not supported on this platform", "-R");
		return false;
	}
#else
	uid_t uid = 0;
	if (switchuser) {
		struct passwd *pw = getpwnam(switchuser);
		if (!pw) {
			logger(LOG_ERR, "unknown user `%s'", switchuser);
			return false;
		}
		uid = pw->pw_uid;
		if (initgroups(switchuser, pw->pw_gid) != 0 ||
		    setgid(pw->pw_gid) != 0) {
			logger(LOG_ERR, "System call `%s' failed: %s",
			       "initgroups", strerror(errno));
			return false;
		}
		endgrent();
		endpwent();
	}
	if (do_chroot) {
		tzset();	/* for proper timestamps in logs */
		if (chroot(confbase) != 0 || chdir("/") != 0) {
			logger(LOG_ERR, "System call `%s' failed: %s",
			       "chroot", strerror(errno));
			return false;
		}
		free(confbase);
		confbase = xstrdup("");
	}
	if (switchuser)
		if (setuid(uid) != 0) {
			logger(LOG_ERR, "System call `%s' failed: %s",
			       "setuid", strerror(errno));
			return false;
		}
#endif
	return true;
}

#ifdef HAVE_MINGW
# define setpriority(level) SetPriorityClass(GetCurrentProcess(), level)
#else
# define NORMAL_PRIORITY_CLASS 0
# define BELOW_NORMAL_PRIORITY_CLASS 10
# define HIGH_PRIORITY_CLASS -10
# define setpriority(level) nice(level)
#endif

int main(int argc, char **argv) {
	program_name = argv[0];

	if(!parse_options(argc, argv))
		return 1;
	
	make_names();

	if(show_version) {
		printf("%s version %s (built %s %s, protocol %d)\n", PACKAGE,
			   VERSION, __DATE__, __TIME__, PROT_CURRENT);
		printf("Copyright (C) 1998-2009 Ivo Timmermans, Guus Sliepen and others.\n"
				"See the AUTHORS file for a complete list.\n\n"
				"tinc comes with ABSOLUTELY NO WARRANTY.  This is free software,\n"
				"and you are welcome to redistribute it under certain conditions;\n"
				"see the file COPYING for details.\n");

		return 0;
	}

	if(show_help) {
		usage(false);
		return 0;
	}

#ifdef HAVE_MINGW
	if(WSAStartup(MAKEWORD(2, 2), &wsa_state)) {
		logger(LOG_ERR, "System call `%s' failed: %s", "WSAStartup", winerror(GetLastError()));
		return 1;
	}
#endif

	openlogger("tinc", use_logfile?LOGMODE_FILE:LOGMODE_STDERR);

	if(!event_init()) {
		logger(LOG_ERR, "Error initializing libevent!");
		return 1;
	}

	g_argv = argv;

	init_configuration(&config_tree);

	/* Slllluuuuuuurrrrp! */

	srand(time(NULL));
	crypto_init();

	if(!read_server_config())
		return 1;

	if(lzo_init() != LZO_E_OK) {
		logger(LOG_ERR, "Error initializing LZO compressor!");
		return 1;
	}

#ifdef HAVE_MINGW
	if(!do_detach || !init_service())
		return main2(argc, argv);
	else
		return 1;
}

int main2(int argc, char **argv) {
	InitializeCriticalSection(&mutex);
	EnterCriticalSection(&mutex);
#endif

	if(!detach())
		return 1;

#ifdef HAVE_MLOCKALL
	/* Lock all pages into memory if requested.
	 * This has to be done after daemon()/fork() so it works for child.
	 * No need to do that in parent as it's very short-lived. */
	if(do_mlock && mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
		logger(LOG_ERR, "System call `%s' failed: %s", "mlockall",
		   strerror(errno));
		return 1;
	}
#endif

	/* Setup sockets and open device. */

	if(!setup_network())
		goto end;

	if(!init_control())
		return 1;

	/* Initiate all outgoing connections. */

	try_outgoing_connections();

	/* Change process priority */

        char *priority = 0;

        if(get_config_string(lookup_config(config_tree, "ProcessPriority"), &priority)) {
                if(!strcasecmp(priority, "Normal"))
                        setpriority(NORMAL_PRIORITY_CLASS);
                else if(!strcasecmp(priority, "Low"))
                        setpriority(BELOW_NORMAL_PRIORITY_CLASS);
                else if(!strcasecmp(priority, "High"))
                        setpriority(HIGH_PRIORITY_CLASS);
                else {
                        logger(LOG_ERR, "Invalid priority `%s`!", priority);
                        goto end;
                }
        }

	/* drop privileges */
	if (!drop_privs())
		goto end;

	/* Start main loop. It only exits when tinc is killed. */

	status = main_loop();

	/* Shutdown properly. */

	ifdebug(CONNECTIONS)
		dump_device_stats();

	close_network_connections();

end:
	logger(LOG_NOTICE, "Terminating");

	exit_control();

	crypto_exit();

	exit_configuration(&config_tree);
	free_names();

	return status;
}
