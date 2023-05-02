/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <unistd.h>

#include "alloc-util.h"
#include "bus-common-errors.h"
#include "bus-error.h"
#include "dbus-unit.h"
#include "strv.h"
#include "terminal-util.h"
#include "transaction.h"

static void transaction_unlink_job(Transaction *tr, Job *j, bool delete_dependencies);

static void transaction_delete_job(Transaction *tr, Job *j, bool delete_dependencies) {
        assert(tr);
        assert(j);

        /* Deletes one job from the transaction */

        transaction_unlink_job(tr, j, delete_dependencies);

        job_free(j);
}

static void transaction_delete_unit(Transaction *tr, Unit *u) {
        Job *j;

        /* Deletes all jobs associated with a certain unit from the
         * transaction */

        while ((j = hashmap_get(tr->jobs, u)))
                transaction_delete_job(tr, j, true);
}

void transaction_abort(Transaction *tr) {
        Job *j;

        assert(tr);

        while ((j = hashmap_first(tr->jobs)))
                transaction_delete_job(tr, j, false);

        assert(hashmap_isempty(tr->jobs));
}

static void transaction_find_jobs_that_matter_to_anchor(Job *j, unsigned generation) {
        assert(j);

        /* A recursive sweep through the graph that marks all units
         * that matter to the anchor job, i.e. are directly or
         * indirectly a dependency of the anchor job via paths that
         * are fully marked as mattering. */

        j->matters_to_anchor = true;
        j->generation = generation;

        LIST_FOREACH(subject, l, j->subject_list) {

                /* This link does not matter */
                if (!l->matters)
                        continue;

                /* This unit has already been marked */
                if (l->object->generation == generation)
                        continue;

                transaction_find_jobs_that_matter_to_anchor(l->object, generation);
        }
}

static void transaction_merge_and_delete_job(Transaction *tr, Job *j, Job *other, JobType t) {
        JobDependency *last;

        assert(j);
        assert(other);
        assert(j->unit == other->unit);
        assert(!j->installed);

        /* Merges 'other' into 'j' and then deletes 'other'. */

        j->type = t;
        j->state = JOB_WAITING;
        j->irreversible = j->irreversible || other->irreversible;
        j->matters_to_anchor = j->matters_to_anchor || other->matters_to_anchor;

        /* Patch us in as new owner of the JobDependency objects */
        last = NULL;
        LIST_FOREACH(subject, l, other->subject_list) {
                assert(l->subject == other);
                l->subject = j;
                last = l;
        }

        /* Merge both lists */
        if (last) {
                last->subject_next = j->subject_list;
                if (j->subject_list)
                        j->subject_list->subject_prev = last;
                j->subject_list = other->subject_list;
        }

        /* Patch us in as new owner of the JobDependency objects */
        last = NULL;
        LIST_FOREACH(object, l, other->object_list) {
                assert(l->object == other);
                l->object = j;
                last = l;
        }

        /* Merge both lists */
        if (last) {
                last->object_next = j->object_list;
                if (j->object_list)
                        j->object_list->object_prev = last;
                j->object_list = other->object_list;
        }

        /* Kill the other job */
        other->subject_list = NULL;
        other->object_list = NULL;
        transaction_delete_job(tr, other, true);
}

_pure_ static bool job_is_conflicted_by(Job *j) {
        assert(j);

        /* Returns true if this job is pulled in by a least one
         * ConflictedBy dependency. */

        LIST_FOREACH(object, l, j->object_list)
                if (l->conflicts)
                        return true;

        return false;
}

static int delete_one_unmergeable_job(Transaction *tr, Job *job) {
        assert(job);

        /* Tries to delete one item in the linked list
         * j->transaction_next->transaction_next->... that conflicts
         * with another one, in an attempt to make an inconsistent
         * transaction work. */

        /* We rely here on the fact that if a merged with b does not
         * merge with c, either a or b merge with c neither */
        LIST_FOREACH(transaction, j, job)
                LIST_FOREACH(transaction, k, j->transaction_next) {
                        Job *d;

                        /* Is this one mergeable? Then skip it */
                        if (job_type_is_mergeable(j->type, k->type))
                                continue;

                        /* Ok, we found two that conflict, let's see if we can
                         * drop one of them */
                        if (!j->matters_to_anchor && !k->matters_to_anchor) {

                                /* Both jobs don't matter, so let's
                                 * find the one that is smarter to
                                 * remove. Let's think positive and
                                 * rather remove stops then starts --
                                 * except if something is being
                                 * stopped because it is conflicted by
                                 * another unit in which case we
                                 * rather remove the start. */

                                log_unit_debug(j->unit,
                                               "Looking at job %s/%s conflicted_by=%s",
                                               j->unit->id, job_type_to_string(j->type),
                                               yes_no(j->type == JOB_STOP && job_is_conflicted_by(j)));
                                log_unit_debug(k->unit,
                                               "Looking at job %s/%s conflicted_by=%s",
                                               k->unit->id, job_type_to_string(k->type),
                                               yes_no(k->type == JOB_STOP && job_is_conflicted_by(k)));

                                if (j->type == JOB_STOP) {

                                        if (job_is_conflicted_by(j))
                                                d = k;
                                        else
                                                d = j;

                                } else if (k->type == JOB_STOP) {

                                        if (job_is_conflicted_by(k))
                                                d = j;
                                        else
                                                d = k;
                                } else
                                        d = j;

                        } else if (!j->matters_to_anchor)
                                d = j;
                        else if (!k->matters_to_anchor)
                                d = k;
                        else
                                return -ENOEXEC;

                        /* Ok, we can drop one, so let's do so. */
                        log_unit_debug(d->unit,
                                       "Fixing conflicting jobs %s/%s,%s/%s by deleting job %s/%s",
                                       j->unit->id, job_type_to_string(j->type),
                                       k->unit->id, job_type_to_string(k->type),
                                       d->unit->id, job_type_to_string(d->type));
                        transaction_delete_job(tr, d, true);
                        return 0;
                }

        return -EINVAL;
}

static int transaction_merge_jobs(Transaction *tr, sd_bus_error *e) {
        Job *j;
        int r;

        assert(tr);

        /* First step, check whether any of the jobs for one specific
         * task conflict. If so, try to drop one of them. */
        HASHMAP_FOREACH(j, tr->jobs) {
                JobType t;

                t = j->type;
                LIST_FOREACH(transaction, k, j->transaction_next) {
                        if (job_type_merge_and_collapse(&t, k->type, j->unit) >= 0)
                                continue;

                        /* OK, we could not merge all jobs for this
                         * action. Let's see if we can get rid of one
                         * of them */

                        r = delete_one_unmergeable_job(tr, j);
                        if (r >= 0)
                                /* Ok, we managed to drop one, now
                                 * let's ask our callers to call us
                                 * again after garbage collecting */
                                return -EAGAIN;

                        /* We couldn't merge anything. Failure */
                        return sd_bus_error_setf(e, BUS_ERROR_TRANSACTION_JOBS_CONFLICTING,
                                                 "Transaction contains conflicting jobs '%s' and '%s' for %s. "
                                                 "Probably contradicting requirement dependencies configured.",
                                                 job_type_to_string(t),
                                                 job_type_to_string(k->type),
                                                 k->unit->id);
                }
        }

        /* Second step, merge the jobs. */
        HASHMAP_FOREACH(j, tr->jobs) {
                JobType t = j->type;

                /* Merge all transaction jobs for j->unit */
                LIST_FOREACH(transaction, k, j->transaction_next)
                        assert_se(job_type_merge_and_collapse(&t, k->type, j->unit) == 0);

                Job *k;
                while ((k = j->transaction_next)) {
                        if (tr->anchor_job == k) {
                                transaction_merge_and_delete_job(tr, k, j, t);
                                j = k;
                        } else
                                transaction_merge_and_delete_job(tr, j, k, t);
                }

                assert(!j->transaction_next);
                assert(!j->transaction_prev);
        }

        return 0;
}

static void transaction_drop_redundant(Transaction *tr) {
        bool again;

        /* Goes through the transaction and removes all jobs of the units whose jobs are all noops. If not
         * all of a unit's jobs are redundant, they are kept. */

        assert(tr);

        do {
                Job *j;

                again = false;

                HASHMAP_FOREACH(j, tr->jobs) {
                        bool keep = false;

                        LIST_FOREACH(transaction, k, j)
                                if (tr->anchor_job == k ||
                                    !job_type_is_redundant(k->type, unit_active_state(k->unit)) ||
                                    (k->unit->job && job_type_is_conflicting(k->type, k->unit->job->type))) {
                                        keep = true;
                                        break;
                                }

                        if (!keep) {
                                log_trace("Found redundant job %s/%s, dropping from transaction.",
                                          j->unit->id, job_type_to_string(j->type));
                                transaction_delete_job(tr, j, false);
                                again = true;
                                break;
                        }
                }
        } while (again);
}

_pure_ static bool unit_matters_to_anchor(Unit *u, Job *job) {
        assert(u);
        assert(job);
        assert(!job->transaction_prev);

        /* Checks whether at least one of the jobs for this unit
         * matters to the anchor. */

        LIST_FOREACH(transaction, j, job)
                if (j->matters_to_anchor)
                        return true;

        return false;
}

static char* merge_unit_ids(const char* unit_log_field, char * const* pairs) {
        _cleanup_free_ char *ans = NULL;
        size_t size = 0;

        STRV_FOREACH_PAIR(unit_id, job_type, pairs) {
                size_t next;

                if (size > 0)
                        ans[size - 1] = '\n';

                next = strlen(unit_log_field) + strlen(*unit_id);
                if (!GREEDY_REALLOC(ans, size + next + 1))
                        return NULL;

                sprintf(ans + size, "%s%s", unit_log_field, *unit_id);
                size += next + 1;
        }

        if (!ans)
                return strdup("");

        return TAKE_PTR(ans);
}

static int transaction_verify_order_one(Transaction *tr, Job *j, Job *from, unsigned generation, sd_bus_error *e) {

        static const UnitDependencyAtom directions[] = {
                UNIT_ATOM_BEFORE,
                UNIT_ATOM_AFTER,
        };

        int r;

        assert(tr);
        assert(j);
        assert(!j->transaction_prev);

        /* Does a recursive sweep through the ordering graph, looking for a cycle. If we find a cycle we try
         * to break it. */

        /* Have we seen this before? */
        if (j->generation == generation) {
                Job *k, *delete = NULL;
                _cleanup_free_ char **array = NULL, *unit_ids = NULL;

                /* If the marker is NULL we have been here already and decided the job was loop-free from
                 * here. Hence shortcut things and return right-away. */
                if (!j->marker)
                        return 0;

                /* So, the marker is not NULL and we already have been here. We have a cycle. Let's try to
                 * break it. We go backwards in our path and try to find a suitable job to remove. We use the
                 * marker to find our way back, since smart how we are we stored our way back in there. */
                for (k = from; k; k = ((k->generation == generation && k->marker != k) ? k->marker : NULL)) {

                        /* For logging below */
                        if (strv_push_pair(&array, k->unit->id, (char*) job_type_to_string(k->type)) < 0)
                                log_oom();

                        if (!delete && hashmap_get(tr->jobs, k->unit) && !unit_matters_to_anchor(k->unit, k))
                                /* Ok, we can drop this one, so let's do so. */
                                delete = k;

                        /* Check if this in fact was the beginning of the cycle */
                        if (k == j)
                                break;
                }

                unit_ids = merge_unit_ids(j->manager->unit_log_field, array); /* ignore error */

                STRV_FOREACH_PAIR(unit_id, job_type, array)
                        /* logging for j not k here to provide a consistent narrative */
                        log_struct(LOG_WARNING,
                                   LOG_UNIT_MESSAGE(j->unit,
                                                    "Found %s on %s/%s",
                                                    unit_id == array ? "ordering cycle" : "dependency",
                                                    *unit_id, *job_type),
                                   "%s", strna(unit_ids));

                if (delete) {
                        const char *status;
                        /* logging for j not k here to provide a consistent narrative */
                        log_struct(LOG_ERR,
                                   LOG_UNIT_MESSAGE(j->unit,
                                                    "Job %s/%s deleted to break ordering cycle starting with %s/%s",
                                                    delete->unit->id, job_type_to_string(delete->type),
                                                    j->unit->id, job_type_to_string(j->type)),
                                   "%s", strna(unit_ids));

                        if (log_get_show_color())
                                status = ANSI_HIGHLIGHT_RED " SKIP " ANSI_NORMAL;
                        else
                                status = " SKIP ";

                        unit_status_printf(delete->unit,
                                           STATUS_TYPE_NOTICE,
                                           status,
                                           "Ordering cycle found, skipping %s",
                                           unit_status_string(delete->unit, NULL));
                        transaction_delete_unit(tr, delete->unit);
                        return -EAGAIN;
                }

                log_struct(LOG_ERR,
                           LOG_UNIT_MESSAGE(j->unit, "Unable to break cycle starting with %s/%s",
                                            j->unit->id, job_type_to_string(j->type)),
                           "%s", strna(unit_ids));

                return sd_bus_error_setf(e, BUS_ERROR_TRANSACTION_ORDER_IS_CYCLIC,
                                         "Transaction order is cyclic. See system logs for details.");
        }

        /* Make the marker point to where we come from, so that we can
         * find our way backwards if we want to break a cycle. We use
         * a special marker for the beginning: we point to
         * ourselves. */
        j->marker = from ? from : j;
        j->generation = generation;

        /* Actual ordering of jobs depends on the unit ordering dependency and job types. We need to traverse
         * the graph over 'before' edges in the actual job execution order. We traverse over both unit
         * ordering dependencies and we test with job_compare() whether it is the 'before' edge in the job
         * execution ordering. */
        for (size_t d = 0; d < ELEMENTSOF(directions); d++) {
                Unit *u;

                UNIT_FOREACH_DEPENDENCY(u, j->unit, directions[d]) {
                        Job *o;

                        /* Is there a job for this unit? */
                        o = hashmap_get(tr->jobs, u);
                        if (!o) {
                                /* Ok, there is no job for this in the transaction, but maybe there is
                                 * already one running? */
                                o = u->job;
                                if (!o)
                                        continue;
                        }

                        /* Cut traversing if the job j is not really *before* o. */
                        if (job_compare(j, o, directions[d]) >= 0)
                                continue;

                        r = transaction_verify_order_one(tr, o, j, generation, e);
                        if (r < 0)
                                return r;
                }
        }

        /* Ok, let's backtrack, and remember that this entry is not on
         * our path anymore. */
        j->marker = NULL;

        return 0;
}

static int transaction_verify_order(Transaction *tr, unsigned *generation, sd_bus_error *e) {
        Job *j;
        int r;
        unsigned g;

        assert(tr);
        assert(generation);

        /* Check if the ordering graph is cyclic. If it is, try to fix
         * that up by dropping one of the jobs. */

        g = (*generation)++;

        HASHMAP_FOREACH(j, tr->jobs) {
                r = transaction_verify_order_one(tr, j, NULL, g, e);
                if (r < 0)
                        return r;
        }

        return 0;
}

static void transaction_collect_garbage(Transaction *tr) {
        bool again;

        assert(tr);

        /* Drop jobs that are not required by any other job */

        do {
                Job *j;

                again = false;

                HASHMAP_FOREACH(j, tr->jobs) {
                        if (tr->anchor_job == j)
                                continue;

                        if (!j->object_list) {
                                log_trace("Garbage collecting job %s/%s", j->unit->id, job_type_to_string(j->type));
                                transaction_delete_job(tr, j, true);
                                again = true;
                                break;
                        }

                        log_trace("Keeping job %s/%s because of %s/%s",
                                  j->unit->id, job_type_to_string(j->type),
                                  j->object_list->subject ? j->object_list->subject->unit->id : "root",
                                  j->object_list->subject ? job_type_to_string(j->object_list->subject->type) : "root");
                }

        } while (again);
}

static int transaction_is_destructive(Transaction *tr, JobMode mode, sd_bus_error *e) {
        Job *j;

        assert(tr);

        /* Checks whether applying this transaction means that
         * existing jobs would be replaced */

        HASHMAP_FOREACH(j, tr->jobs) {

                /* Assume merged */
                assert(!j->transaction_prev);
                assert(!j->transaction_next);

                if (j->unit->job && (mode == JOB_FAIL || j->unit->job->irreversible) &&
                    job_type_is_conflicting(j->unit->job->type, j->type))
                        return sd_bus_error_setf(e, BUS_ERROR_TRANSACTION_IS_DESTRUCTIVE,
                                                 "Transaction for %s/%s is destructive (%s has '%s' job queued, but '%s' is included in transaction).",
                                                 tr->anchor_job->unit->id, job_type_to_string(tr->anchor_job->type),
                                                 j->unit->id, job_type_to_string(j->unit->job->type), job_type_to_string(j->type));
        }

        return 0;
}

static void transaction_minimize_impact(Transaction *tr) {
        Job *head;

        assert(tr);

        /* Drops all unnecessary jobs that reverse already active jobs
         * or that stop a running service. */

rescan:
        HASHMAP_FOREACH(head, tr->jobs) {
                LIST_FOREACH(transaction, j, head) {
                        bool stops_running_service, changes_existing_job;

                        /* If it matters, we shouldn't drop it */
                        if (j->matters_to_anchor)
                                continue;

                        /* Would this stop a running service?
                         * Would this change an existing job?
                         * If so, let's drop this entry */

                        stops_running_service =
                                j->type == JOB_STOP && UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(j->unit));

                        changes_existing_job =
                                j->unit->job &&
                                job_type_is_conflicting(j->type, j->unit->job->type);

                        if (!stops_running_service && !changes_existing_job)
                                continue;

                        if (stops_running_service)
                                log_unit_debug(j->unit,
                                               "%s/%s would stop a running service.",
                                               j->unit->id, job_type_to_string(j->type));

                        if (changes_existing_job)
                                log_unit_debug(j->unit,
                                               "%s/%s would change existing job.",
                                               j->unit->id, job_type_to_string(j->type));

                        /* Ok, let's get rid of this */
                        log_unit_debug(j->unit,
                                       "Deleting %s/%s to minimize impact.",
                                       j->unit->id, job_type_to_string(j->type));

                        transaction_delete_job(tr, j, true);
                        goto rescan;
                }
        }
}

static int transaction_apply(
                Transaction *tr,
                Manager *m,
                JobMode mode,
                Set *affected_jobs) {

        Job *j;
        int r;

        /* Moves the transaction jobs to the set of active jobs */

        if (IN_SET(mode, JOB_ISOLATE, JOB_FLUSH)) {

                /* When isolating first kill all installed jobs which
                 * aren't part of the new transaction */
                HASHMAP_FOREACH(j, m->jobs) {
                        assert(j->installed);

                        if (j->unit->ignore_on_isolate)
                                continue;

                        if (hashmap_get(tr->jobs, j->unit))
                                continue;

                        /* Not invalidating recursively. Avoids triggering
                         * OnFailure= actions of dependent jobs. Also avoids
                         * invalidating our iterator. */
                        job_finish_and_invalidate(j, JOB_CANCELED, false, false);
                }
        }

        HASHMAP_FOREACH(j, tr->jobs) {
                /* Assume merged */
                assert(!j->transaction_prev);
                assert(!j->transaction_next);

                r = hashmap_ensure_put(&m->jobs, NULL, UINT32_TO_PTR(j->id), j);
                if (r < 0)
                        goto rollback;
        }

        while ((j = hashmap_steal_first(tr->jobs))) {
                Job *installed_job;

                /* Clean the job dependencies */
                transaction_unlink_job(tr, j, false);

                installed_job = job_install(j);
                if (installed_job != j) {
                        /* j has been merged into a previously installed job */
                        if (tr->anchor_job == j)
                                tr->anchor_job = installed_job;
                        hashmap_remove_value(m->jobs, UINT32_TO_PTR(j->id), j);
                        job_free(j);
                        j = installed_job;
                }

                job_add_to_run_queue(j);
                job_add_to_dbus_queue(j);
                job_start_timer(j, false);
                job_shutdown_magic(j);

                /* When 'affected' is specified, let's track all in it all jobs that were touched because of
                 * this transaction. */
                if (affected_jobs)
                        (void) set_put(affected_jobs, j);
        }

        return 0;

rollback:

        HASHMAP_FOREACH(j, tr->jobs)
                hashmap_remove_value(m->jobs, UINT32_TO_PTR(j->id), j);

        return r;
}

int transaction_activate(
                Transaction *tr,
                Manager *m,
                JobMode mode,
                Set *affected_jobs,
                sd_bus_error *e) {

        Job *j;
        int r;
        unsigned generation = 1;

        assert(tr);

        /* This applies the changes recorded in tr->jobs to
         * the actual list of jobs, if possible. */

        /* Reset the generation counter of all installed jobs. The detection of cycles
         * looks at installed jobs. If they had a non-zero generation from some previous
         * walk of the graph, the algorithm would break. */
        HASHMAP_FOREACH(j, m->jobs)
                j->generation = 0;

        /* First step: figure out which jobs matter */
        transaction_find_jobs_that_matter_to_anchor(tr->anchor_job, generation++);

        /* Second step: Try not to stop any running services if
         * we don't have to. Don't try to reverse running
         * jobs if we don't have to. */
        if (mode == JOB_FAIL)
                transaction_minimize_impact(tr);

        /* Third step: Drop redundant jobs */
        transaction_drop_redundant(tr);

        for (;;) {
                /* Fourth step: Let's remove unneeded jobs that might
                 * be lurking. */
                if (mode != JOB_ISOLATE)
                        transaction_collect_garbage(tr);

                /* Fifth step: verify order makes sense and correct
                 * cycles if necessary and possible */
                r = transaction_verify_order(tr, &generation, e);
                if (r >= 0)
                        break;

                if (r != -EAGAIN)
                        return log_warning_errno(r, "Requested transaction contains an unfixable cyclic ordering dependency: %s", bus_error_message(e, r));

                /* Let's see if the resulting transaction ordering
                 * graph is still cyclic... */
        }

        for (;;) {
                /* Sixth step: let's drop unmergeable entries if
                 * necessary and possible, merge entries we can
                 * merge */
                r = transaction_merge_jobs(tr, e);
                if (r >= 0)
                        break;

                if (r != -EAGAIN)
                        return log_warning_errno(r, "Requested transaction contains unmergeable jobs: %s", bus_error_message(e, r));

                /* Seventh step: an entry got dropped, let's garbage
                 * collect its dependencies. */
                if (mode != JOB_ISOLATE)
                        transaction_collect_garbage(tr);

                /* Let's see if the resulting transaction still has
                 * unmergeable entries ... */
        }

        /* Eights step: Drop redundant jobs again, if the merging now allows us to drop more. */
        transaction_drop_redundant(tr);

        /* Ninth step: check whether we can actually apply this */
        r = transaction_is_destructive(tr, mode, e);
        if (r < 0)
                return log_notice_errno(r, "Requested transaction contradicts existing jobs: %s", bus_error_message(e, r));

        /* Tenth step: apply changes */
        r = transaction_apply(tr, m, mode, affected_jobs);
        if (r < 0)
                return log_warning_errno(r, "Failed to apply transaction: %m");

        assert(hashmap_isempty(tr->jobs));

        if (!hashmap_isempty(m->jobs)) {
                /* Are there any jobs now? Then make sure we have the
                 * idle pipe around. We don't really care too much
                 * whether this works or not, as the idle pipe is a
                 * feature for cosmetics, not actually useful for
                 * anything beyond that. */

                if (m->idle_pipe[0] < 0 && m->idle_pipe[1] < 0 &&
                    m->idle_pipe[2] < 0 && m->idle_pipe[3] < 0) {
                        (void) pipe2(m->idle_pipe, O_NONBLOCK|O_CLOEXEC);
                        (void) pipe2(m->idle_pipe + 2, O_NONBLOCK|O_CLOEXEC);
                }
        }

        return 0;
}

static Job* transaction_add_one_job(Transaction *tr, JobType type, Unit *unit, bool *is_new) {
        Job *j, *f;

        assert(tr);
        assert(unit);

        /* Looks for an existing prospective job and returns that. If
         * it doesn't exist it is created and added to the prospective
         * jobs list. */

        f = hashmap_get(tr->jobs, unit);

        LIST_FOREACH(transaction, i, f) {
                assert(i->unit == unit);

                if (i->type == type) {
                        if (is_new)
                                *is_new = false;
                        return i;
                }
        }

        j = job_new(unit, type);
        if (!j)
                return NULL;

        j->generation = 0;
        j->marker = NULL;
        j->matters_to_anchor = false;
        j->irreversible = tr->irreversible;

        LIST_PREPEND(transaction, f, j);

        if (hashmap_replace(tr->jobs, unit, f) < 0) {
                LIST_REMOVE(transaction, f, j);
                job_free(j);
                return NULL;
        }

        if (is_new)
                *is_new = true;

        log_trace("Added job %s/%s to transaction.", unit->id, job_type_to_string(type));

        return j;
}

static void transaction_unlink_job(Transaction *tr, Job *j, bool delete_dependencies) {
        assert(tr);
        assert(j);

        if (j->transaction_prev)
                j->transaction_prev->transaction_next = j->transaction_next;
        else if (j->transaction_next)
                hashmap_replace(tr->jobs, j->unit, j->transaction_next);
        else
                hashmap_remove_value(tr->jobs, j->unit, j);

        if (j->transaction_next)
                j->transaction_next->transaction_prev = j->transaction_prev;

        j->transaction_prev = j->transaction_next = NULL;

        while (j->subject_list)
                job_dependency_free(j->subject_list);

        while (j->object_list) {
                Job *other = j->object_list->matters ? j->object_list->subject : NULL;

                job_dependency_free(j->object_list);

                if (other && delete_dependencies) {
                        log_unit_debug(other->unit,
                                       "Deleting job %s/%s as dependency of job %s/%s",
                                       other->unit->id, job_type_to_string(other->type),
                                       j->unit->id, job_type_to_string(j->type));
                        transaction_delete_job(tr, other, delete_dependencies);
                }
        }
}

void transaction_add_propagate_reload_jobs(Transaction *tr, Unit *unit, Job *by, bool ignore_order, sd_bus_error *e) {
        JobType nt;
        Unit *dep;
        int r;

        assert(tr);
        assert(unit);

        UNIT_FOREACH_DEPENDENCY(dep, unit, UNIT_ATOM_PROPAGATES_RELOAD_TO) {
                nt = job_type_collapse(JOB_TRY_RELOAD, dep);
                if (nt == JOB_NOP)
                        continue;

                r = transaction_add_job_and_dependencies(tr, nt, dep, by, false, false, false, ignore_order, e);
                if (r < 0) {
                        log_unit_warning(dep,
                                         "Cannot add dependency reload job, ignoring: %s",
                                         bus_error_message(e, r));
                        sd_bus_error_free(e);
                }
        }
}

int transaction_add_job_and_dependencies(
                Transaction *tr,
                JobType type,
                Unit *unit,
                Job *by,
                bool matters,
                bool conflicts,
                bool ignore_requirements,
                bool ignore_order,
                sd_bus_error *e) {

        bool is_new;
        Unit *dep;
        Job *ret;
        int r;

        assert(tr);
        assert(type < _JOB_TYPE_MAX);
        assert(type < _JOB_TYPE_MAX_IN_TRANSACTION);
        assert(unit);

        /* Before adding jobs for this unit, let's ensure that its state has been loaded
         * This matters when jobs are spawned as part of coldplugging itself (see e. g. path_coldplug()).
         * This way, we "recursively" coldplug units, ensuring that we do not look at state of
         * not-yet-coldplugged units. */
        if (MANAGER_IS_RELOADING(unit->manager))
                unit_coldplug(unit);

        if (by)
                log_trace("Pulling in %s/%s from %s/%s", unit->id, job_type_to_string(type), by->unit->id, job_type_to_string(by->type));

        /* Safety check that the unit is a valid state, i.e. not in UNIT_STUB or UNIT_MERGED which should only be set
         * temporarily. */
        if (!UNIT_IS_LOAD_COMPLETE(unit->load_state))
                return sd_bus_error_setf(e, BUS_ERROR_LOAD_FAILED, "Unit %s is not loaded properly.", unit->id);

        if (type != JOB_STOP) {
                r = bus_unit_validate_load_state(unit, e);
                /* The time-based cache allows to start new units without daemon-reload,
                 * but if they are already referenced (because of dependencies or ordering)
                 * then we have to force a load of the fragment. As an optimization, check
                 * first if anything in the usual paths was modified since the last time
                 * the cache was loaded. Also check if the last time an attempt to load the
                 * unit was made was before the most recent cache refresh, so that we know
                 * we need to try again — even if the cache is current, it might have been
                 * updated in a different context before we had a chance to retry loading
                 * this particular unit.
                 *
                 * Given building up the transaction is a synchronous operation, attempt
                 * to load the unit immediately. */
                if (r < 0 && manager_unit_cache_should_retry_load(unit)) {
                        sd_bus_error_free(e);
                        unit->load_state = UNIT_STUB;
                        r = unit_load(unit);
                        if (r < 0 || unit->load_state == UNIT_STUB)
                                unit->load_state = UNIT_NOT_FOUND;
                        r = bus_unit_validate_load_state(unit, e);
                }
                if (r < 0)
                        return r;
        }

        if (!unit_job_is_applicable(unit, type))
                return sd_bus_error_setf(e, BUS_ERROR_JOB_TYPE_NOT_APPLICABLE,
                                         "Job type %s is not applicable for unit %s.",
                                         job_type_to_string(type), unit->id);

        /* First add the job. */
        ret = transaction_add_one_job(tr, type, unit, &is_new);
        if (!ret)
                return -ENOMEM;

        ret->ignore_order = ret->ignore_order || ignore_order;

        /* Then, add a link to the job. */
        if (by) {
                if (!job_dependency_new(by, ret, matters, conflicts))
                        return -ENOMEM;
        } else {
                /* If the job has no parent job, it is the anchor job. */
                assert(!tr->anchor_job);
                tr->anchor_job = ret;
        }

        if (is_new && !ignore_requirements && type != JOB_NOP) {
                Set *following;

                /* If we are following some other unit, make sure we
                 * add all dependencies of everybody following. */
                if (unit_following_set(ret->unit, &following) > 0) {
                        SET_FOREACH(dep, following) {
                                r = transaction_add_job_and_dependencies(tr, type, dep, ret, false, false, false, ignore_order, e);
                                if (r < 0) {
                                        log_unit_full_errno(dep, r == -ERFKILL ? LOG_INFO : LOG_WARNING, r,
                                                            "Cannot add dependency job, ignoring: %s",
                                                            bus_error_message(e, r));
                                        sd_bus_error_free(e);
                                }
                        }

                        set_free(following);
                }

                /* Finally, recursively add in all dependencies. */
                if (IN_SET(type, JOB_START, JOB_RESTART)) {
                        UNIT_FOREACH_DEPENDENCY(dep, ret->unit, UNIT_ATOM_PULL_IN_START) {
                                r = transaction_add_job_and_dependencies(tr, JOB_START, dep, ret, true, false, false, ignore_order, e);
                                if (r < 0) {
                                        if (r != -EBADR) /* job type not applicable */
                                                goto fail;

                                        sd_bus_error_free(e);
                                }
                        }

                        UNIT_FOREACH_DEPENDENCY(dep, ret->unit, UNIT_ATOM_PULL_IN_START_IGNORED) {
                                r = transaction_add_job_and_dependencies(tr, JOB_START, dep, ret, false, false, false, ignore_order, e);
                                if (r < 0) {
                                        /* unit masked, job type not applicable and unit not found are not considered as errors. */
                                        log_unit_full_errno(dep,
                                                            IN_SET(r, -ERFKILL, -EBADR, -ENOENT) ? LOG_DEBUG : LOG_WARNING,
                                                            r, "Cannot add dependency job, ignoring: %s",
                                                            bus_error_message(e, r));
                                        sd_bus_error_free(e);
                                }
                        }

                        UNIT_FOREACH_DEPENDENCY(dep, ret->unit, UNIT_ATOM_PULL_IN_VERIFY) {
                                r = transaction_add_job_and_dependencies(tr, JOB_VERIFY_ACTIVE, dep, ret, true, false, false, ignore_order, e);
                                if (r < 0) {
                                        if (r != -EBADR) /* job type not applicable */
                                                goto fail;

                                        sd_bus_error_free(e);
                                }
                        }

                        UNIT_FOREACH_DEPENDENCY(dep, ret->unit, UNIT_ATOM_PULL_IN_STOP) {
                                r = transaction_add_job_and_dependencies(tr, JOB_STOP, dep, ret, true, true, false, ignore_order, e);
                                if (r < 0) {
                                        if (r != -EBADR) /* job type not applicable */
                                                goto fail;

                                        sd_bus_error_free(e);
                                }
                        }

                        UNIT_FOREACH_DEPENDENCY(dep, ret->unit, UNIT_ATOM_PULL_IN_STOP_IGNORED) {
                                r = transaction_add_job_and_dependencies(tr, JOB_STOP, dep, ret, false, false, false, ignore_order, e);
                                if (r < 0) {
                                        log_unit_warning(dep,
                                                         "Cannot add dependency job, ignoring: %s",
                                                         bus_error_message(e, r));
                                        sd_bus_error_free(e);
                                }
                        }
                }

                if (IN_SET(type, JOB_STOP, JOB_RESTART)) {
                        UnitDependencyAtom atom;
                        JobType ptype;

                        /* We propagate STOP as STOP, but RESTART only as TRY_RESTART, in order not to start
                         * dependencies that are not around. */
                        if (type == JOB_RESTART) {
                                atom = UNIT_ATOM_PROPAGATE_RESTART;
                                ptype = JOB_TRY_RESTART;
                        } else {
                                ptype = JOB_STOP;
                                atom = UNIT_ATOM_PROPAGATE_STOP;
                        }

                        UNIT_FOREACH_DEPENDENCY(dep, ret->unit, atom) {
                                JobType nt;

                                nt = job_type_collapse(ptype, dep);
                                if (nt == JOB_NOP)
                                        continue;

                                r = transaction_add_job_and_dependencies(tr, nt, dep, ret, true, false, false, ignore_order, e);
                                if (r < 0) {
                                        if (r != -EBADR) /* job type not applicable */
                                                goto fail;

                                        sd_bus_error_free(e);
                                }
                        }
                }

                if (type == JOB_RELOAD)
                        transaction_add_propagate_reload_jobs(tr, ret->unit, ret, ignore_order, e);

                /* JOB_VERIFY_ACTIVE requires no dependency handling */
        }

        return 0;

fail:
        return r;
}

static bool shall_stop_on_isolate(Transaction *tr, Unit *u) {
        assert(tr);
        assert(u);

        if (u->ignore_on_isolate)
                return false;

        /* Is there already something listed for this? */
        if (hashmap_get(tr->jobs, u))
                return false;

        return true;
}

int transaction_add_isolate_jobs(Transaction *tr, Manager *m) {
        Unit *u;
        char *k;
        int r;

        assert(tr);
        assert(m);

        HASHMAP_FOREACH_KEY(u, k, m->units) {
                Unit *o;

                /* Ignore aliases */
                if (u->id != k)
                        continue;

                /* No need to stop inactive units */
                if (UNIT_IS_INACTIVE_OR_FAILED(unit_active_state(u)) && !u->job)
                        continue;

                if (!shall_stop_on_isolate(tr, u))
                        continue;

                /* Keep units that are triggered by units we want to keep around. */
                bool keep = false;
                UNIT_FOREACH_DEPENDENCY(o, u, UNIT_ATOM_TRIGGERED_BY)
                        if (!shall_stop_on_isolate(tr, o)) {
                                keep = true;
                                break;
                        }
                if (keep)
                        continue;

                r = transaction_add_job_and_dependencies(tr, JOB_STOP, u, tr->anchor_job, true, false, false, false, NULL);
                if (r < 0)
                        log_unit_warning_errno(u, r, "Cannot add isolate job, ignoring: %m");
        }

        return 0;
}

int transaction_add_triggering_jobs(Transaction *tr, Unit *u) {
        Unit *trigger;
        int r;

        assert(tr);
        assert(u);

        UNIT_FOREACH_DEPENDENCY(trigger, u, UNIT_ATOM_TRIGGERED_BY) {

                /* No need to stop inactive jobs */
                if (UNIT_IS_INACTIVE_OR_FAILED(unit_active_state(trigger)) && !trigger->job)
                        continue;

                /* Is there already something listed for this? */
                if (hashmap_get(tr->jobs, trigger))
                        continue;

                r = transaction_add_job_and_dependencies(tr, JOB_STOP, trigger, tr->anchor_job, true, false, false, false, NULL);
                if (r < 0)
                        log_unit_warning_errno(u, r, "Cannot add triggered by job, ignoring: %m");
        }

        return 0;
}

Transaction *transaction_new(bool irreversible) {
        Transaction *tr;

        tr = new0(Transaction, 1);
        if (!tr)
                return NULL;

        tr->jobs = hashmap_new(NULL);
        if (!tr->jobs)
                return mfree(tr);

        tr->irreversible = irreversible;

        return tr;
}

void transaction_free(Transaction *tr) {
        assert(hashmap_isempty(tr->jobs));
        hashmap_free(tr->jobs);
        free(tr);
}
