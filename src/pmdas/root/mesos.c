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
#include <regex.h>
#include "pmapi.h"
#include "impl.h"
#include "pmda.h"

#include "root.h"
#include "jsmn.h"
#include "mesos.h"

/*
 * JSMN helper interfaces for efficiently extracting JSON configs
 */

static int
jsmneq(const char *js, jsmntok_t *tok, const char *s)
{
    if (tok->type != JSMN_STRING)
		return -1;
    if (strlen(s) == tok->end - tok->start &&
	strncasecmp(js + tok->start, s, tok->end - tok->start) == 0)
		return 0;
    return -1;
}

static int
jsmnflag(const char *js, jsmntok_t *tok, int *bits, int flag)
{
    if (tok->type != JSMN_PRIMITIVE)
		return -1;
    if (strncmp(js + tok->start, "true", sizeof("true")-1) == 0)
		*bits |= flag;
    else
		*bits &= ~flag;
    return 0;
}

static int
jsmnint(const char *js, jsmntok_t *tok, int *value)
{
    char	buffer[64];

    if (tok->type != JSMN_PRIMITIVE)
		return -1;
    strncpy(buffer, js + tok->start, tok->end - tok->start);
    buffer[tok->end - tok->start] = '\0';
    *value = (int)strtol(buffer, NULL, 0);
    return 0;
}

static int
jsmnstrdup(const char *js, jsmntok_t *tok, char **name)
{
    char	*s = *name;

    if (tok->type != JSMN_STRING)
		return -1;
    if (s)
		free(s);
    s = strndup(js + tok->start, tok->end - tok->start);
    return ((*name = s) == NULL) ? -1 : 0;
}

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
    struct dirent	*drp;

	if (!mesos)
		mesos = mesos_default;

	snprintf(dp->path, sizeof(dp->path), "%s/meta/slaves/latest/frameworks", mesos);

    if ((rundir = opendir(dp->path)) == NULL) {
   		if (pmDebug & DBG_TRACE_ATTR)
		    fprintf(stderr, "%s: skipping mesos path %s\n", pmProgname, dp->path);
		return;
    }

    while ((drp = readdir(rundir)) != NULL) {
		if (*(path = &drp->d_name[0]) == '.')
		    continue;
		snprintf(dp->path, sizeof(dp->path), "%s/%s/executors", dp->path, path);
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
		  //   snprintf(cp->cgroup, sizeof(cp->cgroup),
				// "system.slice/mesos-%s.scope", path);
		}
		pmdaCacheStore(indom, PMDA_CACHE_ADD, path, cp);
    }
    closedir(rundir);
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
	char 	*matchResult;

    snprintf(path, sizeof(path), "%s/%s/runs/latest/pids/forked.pid", dp->path, name);
    if (!mesos_values_changed(path, values))
		return 0;
    if (pmDebug & DBG_TRACE_ATTR)
		__pmNotifyErr(LOG_DEBUG, "mesos_value_refresh: file=%s\n", path);
    if ((fp = fopen(path, "r")) == NULL)
		return -oserror();

	// set values->name
	if (regcomp(&regex, "[^-]*-[^-]*-(.*)-.*-.*-.*-.*-.*"))
		__pmNotifyErr(LOG_DEBUG, "Failed to compile name regex.\n");
	if (regexec(&regex, name, maxGroups, matchGroup, 0) == 0) {
		strncpy(values->name, &name[matchGroup[1].rm_so], 
			matchGroup[1].rm_eo - matchGroup[1].rm_so);
	}
	regfree(&regex);

	// set values->pid
	fscanf(fp, "%d", &values->pid);

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
