/*-
 * xnumon - monitor macOS for malicious activity
 * https://www.roe.ch/xnumon
 *
 * Copyright (c) 2017-2019, Daniel Roethlisberger <daniel@roe.ch>.
 * All rights reserved.
 *
 * Licensed under the Open Software License version 3.0.
 */

/*
 * Monitoring core for specific techniques.
 */

#include "hackmon.h"

#include "work.h"
#include "atomic.h"

#include <strings.h>
#include <assert.h>

static config_t *config;

static uint64_t events_received;    /* number of events received */
static uint64_t events_processed;   /* number of events processed */
static atomic64_t ooms;             /* counts events impaired due to OOM */

strset_t *suppress_process_access_by_subject_ident;
strset_t *suppress_process_access_by_subject_path;

static void process_access_free(process_access_t *);
static int process_access_work(process_access_t *);

static process_access_t *
process_access_new() {
	process_access_t *pa;

	pa = malloc(sizeof(*pa));
	if (!pa)
		return NULL;
	bzero(pa, sizeof(*pa));
	pa->hdr.code = LOGEVT_PROCESS_ACCESS;
	pa->hdr.le_work = (__typeof__(pa->hdr.le_work))process_access_work;
	pa->hdr.le_free = (__typeof__(pa->hdr.le_free))process_access_free;
	return pa;
}

static void
process_access_free(process_access_t *pa) {
	if (pa->subject_image_exec)
		image_exec_free(pa->subject_image_exec);
	if (pa->object_image_exec)
		image_exec_free(pa->object_image_exec);
	free(pa);
}

/*
 * Executed by worker thread.
 *
 * Returns -1 if this work item should not be logged, 0 otherwise.
 */
static int
process_access_work(process_access_t *pa) {
	if (pa->subject_image_exec && image_exec_match_suppressions(
	                              pa->subject_image_exec,
	                              suppress_process_access_by_subject_ident,
	                              suppress_process_access_by_subject_path))
		return -1;
	return 0;
}

static void
log_event_process_access(struct timespec *tv,
                         audit_proc_t *subject,
                         audit_proc_t *object, /* may be NULL */
                         pid_t objectpid,
                         const char *method) {
	process_access_t *pa;

	pa = process_access_new();
	if (!pa) {
		atomic64_inc(&ooms);
		return;
	}
	pa->subject_image_exec = image_exec_by_pid(subject->pid);
	pa->object_image_exec = image_exec_by_pid(objectpid);
	pa->subject = *subject;
	if (object) {
		pa->object = *object;
	} else {
		pa->objectpid = objectpid;
	}
	pa->method = method;
	pa->hdr.tv = *tv;
	work_submit(pa);
}

static void
hackmon_process_access(struct timespec *tv,
                       audit_proc_t *subject,
                       audit_proc_t *object, /* may be NULL */
                       pid_t objectpid,
                       const char *method) {
	events_received++;

	if (objectpid <= 0)
		return;
	if (subject->pid == objectpid)
		return;

	events_processed++;
	log_event_process_access(tv, subject, object, objectpid, method);
}

/*
 * Called for task_for_pid invocations.
 */
void
hackmon_taskforpid(struct timespec *tv,
                   audit_proc_t *subject,
                   audit_proc_t *object, /* may be NULL */
                   pid_t objectpid) {
	hackmon_process_access(tv, subject, object, objectpid, "task_for_pid");
}

/*
 * Called for ptrace invocations.
 */
void
hackmon_ptrace(struct timespec *tv,
               audit_proc_t *subject,
               audit_proc_t *object, /* may be NULL */
               pid_t objectpid) {
	hackmon_process_access(tv, subject, object, objectpid, "ptrace");
}

void
hackmon_init(config_t *cfg) {
	config = cfg;
	ooms = 0;
	events_received = 0;
	events_processed = 0;
	suppress_process_access_by_subject_ident =
		&cfg->suppress_process_access_by_subject_ident;
	suppress_process_access_by_subject_path =
		&cfg->suppress_process_access_by_subject_path;
}

void
hackmon_fini(void) {
	if (!config)
		return;
	config = NULL;
}

void
hackmon_stats(hackmon_stat_t *st) {
	assert(st);

	st->receiveds = events_received;
	st->processeds = events_processed;
	st->ooms = (uint64_t)ooms;
}

