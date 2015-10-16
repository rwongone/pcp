/*
 * Copyright (c) 2014-2015 Red Hat.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <sys/stat.h>
#include <stdlib.h>
#include <regex.h>
#include "pmapi.h"
#include "impl.h"
#include "pmda.h"

#include "root.h"
#include "mesos.h"

/*
 * Container implementation for the Mesos engine
 */

void
mesos_setup(container_engine_t *dp)
{
	static const char *mesos_default = "/var/lib/mesos";
	const char *mesos = getenv("PCP_MESOS_DIR");
    DIR			*rundir;
    char		*path;
    char 		frameworksPath[120];
    struct dirent	*drp;

	if (!mesos)
		mesos = mesos_default;

	snprintf(frameworksPath, sizeof(frameworksPath), "%s/meta/slaves/latest/frameworks", mesos);

    if ((rundir = opendir(frameworksPath)) == NULL) {
   		if (pmDebug & DBG_TRACE_ATTR)
		    fprintf(stderr, "%s: skipping mesos path %s\n", pmProgname, frameworksPath);
		return;
    }

    while ((drp = readdir(rundir)) != NULL) {
		if (*(path = &drp->d_name[0]) == '.')
		    continue;
		snprintf(dp->path, sizeof(dp->path), "%s/%s/executors", frameworksPath, path);
		break;
	}
	dp->path[sizeof(dp->path)-1] = '\0';

	if (pmDebug & DBG_TRACE_ATTR)
		__pmNotifyErr(LOG_DEBUG, "mesos_setup: using path: %s\n", dp->path);
}

int
mesos_indom_changed(container_engine_t *dp)
{
    static int		lasterrno;
    static struct stat	lastsbuf;
    struct stat		statbuf;

    if (stat(dp->path, &statbuf) != 0) {
		if (oserror() == lasterrno)
		    return 0;
		lasterrno = oserror();
		return 1;
    }
    lasterrno = 0;
    if (!root_stat_time_differs(&statbuf, &lastsbuf))
		return 0;
    lastsbuf = statbuf;
    return 1;
}

void
mesos_insts_refresh(container_engine_t *dp, pmInDom indom)
{
    DIR			*rundir;
    char		*path;
    struct dirent	*drp;
    container_t		*cp;
    int			sts;
    FILE 		*cmd;
    char 		cgroup[128];
    char 		*buffer;

    if ((rundir = opendir(dp->path)) == NULL) {
		if (pmDebug & DBG_TRACE_ATTR)
		    fprintf(stderr, "%s: skipping mesos path %s\n", pmProgname, dp->path);
		return;
    }

    while ((drp = readdir(rundir)) != NULL) {
		if (*(path = &drp->d_name[0]) == '.')
		    continue;
		sts = pmdaCacheLookupName(indom, path, NULL, (void **)&cp);
		if (sts == PMDA_CACHE_ACTIVE)
		    continue;
		/* allocate space for values for this container and update indom */
		if (sts != PMDA_CACHE_INACTIVE) {
		    if (pmDebug & DBG_TRACE_ATTR)
				fprintf(stderr, "%s: adding mesos container %s\n", pmProgname, path);
		    if ((cp = calloc(1, sizeof(container_t))) == NULL)
				continue;
		    cp->engine = dp;

		    buffer = snprintf("head -n 1 /proc/%d/cgroup", cp->pid, 128);
		    cmd = popen(buffer, "r");
		    if (cmd && fgets(cgroup, sizeof(cgroup)-1, cmd)) {
		    	// <number>:<directory>:/mesos/<cgroup_id>
		    	sts = (int)(strchr(cgroup, '/') - cgroup) + 1; // index after first '/'
		    	buffer = (char*)malloc(strlen(cgroup)-sts+1);
		    	memcpy(buffer, &cgroup[sts], strlen(cgroup)-sts);
		    	buffer[strlen(cgroup)-sts] = '\0';
		    	snprintf(cp->cgroup, sizeof(cp->cgroup), "%s", buffer); // mesos/<cgroup_id>
		    	if (pmDebug & DBG_TRACE_ATTR)
					fprintf(stderr, "%s: found cgroup for %s: %s\n", pmProgname, path, cp->cgroup);
		    } else {
			    if (pmDebug & DBG_TRACE_ATTR)
					fprintf(stderr, "%s: failed to find cgroup for %s\n", pmProgname, path);
		    }
		}
		pmdaCacheStore(indom, PMDA_CACHE_ADD, path, cp);
    }
    closedir(rundir);
}

static int
mesos_values_changed(const char *path, container_t *values)
{
    struct stat		statbuf;

    if (stat(path, &statbuf) != 0) {
	memset(&values->stat, 0, sizeof(values->stat));
	return 1;
    }
    if (!root_stat_time_differs(&statbuf, &values->stat))
	return 0;
    values->stat = statbuf;
    return 1;
}

/*
 * Extract critical information (PID1, state) for a named container.
 * Name here is the unique identifier we've chosen to use for Mesos
 * container external instance names (i.e. long hash names).
 */
int
mesos_value_refresh(container_engine_t *dp,
	const char *name, container_t *values)
{
    int		sts;
    FILE	*fp;
    char	path[MAXPATHLEN];
	size_t maxGroups = 2;
	regex_t regex;
	regmatch_t matchGroup[maxGroups];

    snprintf(path, sizeof(path), "%s/%s/runs/latest/pids/forked.pid", dp->path, name);
    if (!mesos_values_changed(path, values))
		return 0;
    if (pmDebug & DBG_TRACE_ATTR)
		__pmNotifyErr(LOG_DEBUG, "mesos_value_refresh: file=%s\n", path);
    if ((fp = fopen(path, "r")) == NULL)
		return -oserror();

	// set container name property to Mesos task key, dash-delimited
	if (regcomp(&regex, "[^-]*-[^-]*-(.*)-.*-.*-.*-.*-.*", 0))
		__pmNotifyErr(LOG_DEBUG, "Failed to compile name regex.\n");
	if (regexec(&regex, name, maxGroups, matchGroup, 0) == 0) {
		strncpy(values->name, &name[matchGroup[1].rm_so], 
			matchGroup[1].rm_eo - matchGroup[1].rm_so);
	}
	regfree(&regex);

	// set values->pid
	sts = fscanf(fp, "%d", &values->pid);
    fclose(fp);
    
    if (sts < 0)
		return sts;

    if (pmDebug & DBG_TRACE_ATTR)
		__pmNotifyErr(LOG_DEBUG, "mesos_value_refresh: uptodate=%d of %d\n",
	    	values->uptodate, NUM_UPTODATE);

    return values->uptodate == NUM_UPTODATE ? 0 : PM_ERR_AGAIN;
}

/*
 * Given two strings, determine if they identify the same container.
 * This is called iteratively, passing over all container instances.
 *
 * Use a simple ranking scheme - the closer the match, the higher the
 * return value, up to a maximum of 100% (zero => no match).
 *
 * 'query' - the name supplied by the PCP monitoring tool.
 * 'username' - the name from the container_t -> name field.
 * 'instname' - the external instance identifier, lengthy hash.
 */
int
mesos_name_matching(struct container_engine *dp, const char *query,
	const char *username, const char *instname)
{
    unsigned int ilength, qlength, limit;
    int i, fuzzy = 0;
    char *slashUsername;

    if (strcmp(query, username) == 0)
		return 100;
    if (strcmp(query, instname) == 0)
		return 99;
	slashUsername = malloc(strlen(username));
	for (i = 0; i < strlen(username); i++) {
		slashUsername[i] = (username[i] == '/') ? '-' : username[i];
	}
	if (strcmp(query, slashUsername) == 0) {
		free(slashUsername);
		return 98;
	}

    qlength = strlen(query);
    ilength = strlen(instname);
    /* find the shortest of the three boundary conditions */
    if ((limit = (qlength < ilength) ? qlength : ilength) > 95)
		limit = 95;
    for (i = 0; i < limit; i++) {
		if (query[i] != instname[i])
		    break;
		fuzzy++;	/* bump for each matching character */
    }
    return fuzzy;
}
