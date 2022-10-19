// Copyright 2018 Dmitry Bogatov <KAction@gnu.org>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// This source file compiles to following binaries:
//
//  - /sbin/shutdown
//  - /sbin/halt
//  - /sbin/reboot

// They must work when /sbin/init is runit, but PID1 is not. It happens
// when bin:runit-init replaces bin:systemd-sysv or bin:sysvinit-core.
//
// Process of system rebooting/halting has nothing to do with PID1, it
// is all about reboot(2).
//
// reboot(8)/halt(8), provided by sysvinit, when invoked without -f,
// ask PID1 to terminate all processes. As last step, PID1 invokes
// /etc/rc0.d/K08halt or /etc/rc6.d/K08reboot, which, at their turn,
// invoke reboot(8)/halt(8) with -f option. With that option on,
// reboot(2) is called, causing Linux to reboot/halt.
//
// They are not needed for work in command line, since 'runit' uses
// another, more simple interface to system
// but they have been here

// /sbin/reboot binaries. Strictly speaking, they are not
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/reboot.h>
#include <assert.h>
#include <string.h>
#include <signal.h>

#define STOPIT "/run/runit.stopit"
#define NOSYNC "/etc/runit/nosync"
#define DOFSCK "/forcefsck"
#define NOFSCK "/fastboot"

#define write2(msg) write(2, msg, sizeof(msg))
typedef enum shutdown_action { ACTION_REBOOT, ACTION_HALT } shutdown_action;

/* Copied and slightly simplified from sysvinit-2.88dsf:/src/initreq.h */
struct sysv_request {
	int	magic;			/* Magic number                 */
	int	cmd;			/* What kind of request         */
	int	runlevel;		/* Runlevel to change to        */
	int	sleeptime;		/* Time between TERM and KILL   */
	char	unused[368];
};
#define SYSV_MAGIC              0x03091969
#define SYSV_CMD_RUNLVL         1
#define SYSV_FIFO               "/run/initctl"

/* Since we are forced to support more and more of command line options
 * expected by initscripts, let us make it more structured.
 */
struct config {
	enum shutdown_action action;
	bool force;
	bool nosync;
	bool nofsck;
	bool dofsck;
	bool noop;		/*stop and exit 0 without actually reboot or poweroff  */
};

struct stat s;

/* Luckily, systemd supports sysvinit pipe interface, so there
 * is no need to write separate function interfacing systemd.
 */
static bool
sysv_shutdown(shutdown_action action)
{
	struct sysv_request request;
	request.magic = SYSV_MAGIC;
	request.cmd = SYSV_CMD_RUNLVL;

	switch (action) {
	case ACTION_REBOOT: request.runlevel = '6'; break;
	case ACTION_HALT:   request.runlevel = '0'; break;
	default: return false;
	};

	int fd = open(SYSV_FIFO, O_WRONLY);
	if (fd < 0) {
		return false;
	}
	bool ok = write(fd, &request, sizeof(request)) == sizeof(request);
	close(fd);
	return ok;
}

static bool
syscall_shutdown(shutdown_action action)
{
	if (stat(NOSYNC,&s) == -1) {
		sync();
	}
	switch (action) {
	case ACTION_REBOOT:
		reboot(RB_AUTOBOOT);
		break;
	case ACTION_HALT:
#ifdef RB_POWER_OFF
		reboot(RB_POWER_OFF);
#else
#ifdef RB_HALT_SYSTEM
		reboot(RB_HALT_SYSTEM);
#else
#ifdef RB_HALT
		reboot(RB_HALT);
#else
		reboot(RB_AUTOBOOT);
#endif
#endif
#endif
		break;
	}
	return false;
}

static bool
runit_shutdown(shutdown_action action)
{
	char* args[] = {
		"/sbin/init", (action == ACTION_HALT) ? "0" : "6", NULL };
	execv("/sbin/init", args);
	return false;
}

int open_trunc(const char *fn)
{ return open(fn,O_WRONLY | O_NDELAY | O_TRUNC | O_CREAT,0644); }

static void
parse_command_line(struct config *cfg, int argc, char **argv)
{
	int i;

	cfg->action    = ACTION_HALT;
	cfg->force     = false;
	cfg->nosync = false;
	cfg->dofsck = false;
	cfg->nofsck = false;
	cfg->noop = false;

	if (strstr(argv[0], "reboot")) {
		cfg->action = ACTION_REBOOT;
	}

	for (i = 1; i != argc; ++i) {
		if (strstr(argv[0], "shutdown")) {
			if (strcmp(argv[i], "-r") == 0) {
				cfg->action = ACTION_REBOOT;
				continue;
			}
			if (strcmp(argv[i], "-f") == 0) {
				cfg->nofsck = true;
				continue;
			}
			if (strcmp(argv[i], "-F") == 0) {
				cfg->dofsck = true;
				continue;
			}
		}
		else {
			if (strcmp(argv[i], "-f") == 0) {
				if (stat(STOPIT,&s) == -1) {
					cfg->force = true;
				}
				else {
					cfg->noop = true;
					write2("WARNING: -f is a noop when runit is init\n");
				}
				continue;
			}
			if (strcmp(argv[i], "--force") == 0) {
				cfg->force = true;
				continue;
			}
			if (strcmp(argv[i], "-w") == 0) {
				write2("WARNING: writing wtmp with -w is not supported for now\n");
				cfg->noop = true;
				continue;
			}
			if (strcmp(argv[i], "--wtmp-only") == 0) {
				cfg->noop = true;
				continue;
			}
			if (strcmp(argv[i], "-n") == 0) {
				cfg->nosync = true;
				continue;
			}
		}
	}
}

int
main(int argc, char **argv)
{
	struct config cfg;
	bool ok;

	parse_command_line(&cfg, argc, argv);

	if (geteuid() != 0) {
		write2("shutdown: must be superuser.\n");
		return 2;
	}
	if (cfg.noop) {
		// No-Op: see #919699 and #899246
		return 0;
	}
	if (cfg.dofsck) {
		if (close(open_trunc(DOFSCK) == -1)) {
			write2("Failed to write forcefsck file.\n");
			return 1;
		}
	}
	if (cfg.nofsck) {
		if (close(open_trunc(NOFSCK) == -1)) {
			write2("Failed to write fastboot file.\n");
			return 1;
		}
	}
	if (cfg.nosync) {
		if (close(open_trunc(NOSYNC) == -1)) {
			write2("Failed to write runit.nosync file.\n");
			return 1;
		}
	}
	if (cfg.force) {
		ok = syscall_shutdown(cfg.action);
	} else {
		ok = sysv_shutdown(cfg.action)
			|| runit_shutdown(cfg.action);
		sleep(15);
		/* System should be already down, but still. */
		syscall_shutdown(cfg.action);
	}
	return ok ? 0 : 1;
}
