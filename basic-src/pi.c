/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2012-2013                                Inria
 * Copyright (C) 2010-2013,2015-2017                      CNRS
 * Copyright (C) 2010-2011,2013-2015                      Université de Bordeaux
 * Copyright (C) 2010                                     Mehdi Juhoor
 *
 * StarPU is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * StarPU is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */

/*
 * This computes Pi by using drawing random coordinates (thanks to the sobol
 * generator) and check whether they fall within one quarter of a circle.  The
 * proportion gives an approximation of Pi. For each task, we draw a number of
 * coordinates, and we gather the number of successful draws.
 *
 * TODO: use curandGenerateUniform instead of the sobol generator, like pi_redux.c does
 */

#include "SobolQRNG/sobol.h"
#include "SobolQRNG/sobol_gold.h"
#include "pi.h"
#include <starpu.h>

#include <sched_policies/fifo_queues.h>
#include <starpu_scheduler.h>
#include <limits.h>
#include <stdlib.h>

#ifdef STARPU_USE_CUDA
void cuda_kernel(void **descr, void *cl_arg);
#endif

#define FPRINTF(ofile, fmt, ...) do { if (!getenv("STARPU_SSILENT")) {fprintf(ofile, fmt, ## __VA_ARGS__); }} while(0)




#ifdef STARPU_QUICK_CHECK

#define NTASKS	320

#elif !defined(STARPU_LONG_CHECK)

#define NTASKS	3200

#else

#define NTASKS	32000

#endif

#define FPRINTF(ofile, fmt, ...) do { if (!getenv("STARPU_SSILENT")) {fprintf(ofile, fmt, ## __VA_ARGS__); }} while(0)




#ifndef DBL_MIN
#define DBL_MIN __DBL_MIN__
#endif

#ifndef DBL_MAX
#define DBL_MAX __DBL_MAX__
#endif

struct _starpu_dmda_data
{
	double alpha;
	double beta;
	double _gamma;
	double idle_power;

	struct _starpu_fifo_taskq **queue_array;
	starpu_pthread_mutex_t policy_mutex;
	struct starpu_task_list main_list;
	long int total_task_cnt;
	long int ready_task_cnt;
	long int eager_task_cnt; /* number of tasks scheduled without model */
	int num_priorities;
};

/* The dmda scheduling policy uses
 *
 * alpha * T_computation + beta * T_communication + gamma * Consumption
 *
 * Here are the default values of alpha, beta, gamma
 */

#define _STARPU_SCHED_ALPHA_DEFAULT 1.0
#define _STARPU_SCHED_BETA_DEFAULT 1.0
#define _STARPU_SCHED_GAMMA_DEFAULT 1000.0

#ifdef STARPU_USE_TOP
static double alpha = _STARPU_SCHED_ALPHA_DEFAULT;
static double beta = _STARPU_SCHED_BETA_DEFAULT;
static double _gamma = _STARPU_SCHED_GAMMA_DEFAULT;
static double idle_power = 0.0;
static const float alpha_minimum=0;
static const float alpha_maximum=10.0;
static const float beta_minimum=0;
static const float beta_maximum=10.0;
static const float gamma_minimum=0;
static const float gamma_maximum=10000.0;
static const float idle_power_minimum=0;
static const float idle_power_maximum=10000.0;
#endif /* !STARPU_USE_TOP */

static int count_non_ready_buffers(struct starpu_task *task, unsigned node)
{
	int cnt = 0;
	unsigned nbuffers = STARPU_TASK_GET_NBUFFERS(task);
	unsigned index;

	for (index = 0; index < nbuffers; index++)
	{
		starpu_data_handle_t handle;
		unsigned buffer_node = node;
		if (task->cl->specific_nodes)
			buffer_node = STARPU_CODELET_GET_NODE(task->cl, index);

		handle = STARPU_TASK_GET_HANDLE(task, index);

		int is_valid;
		starpu_data_query_status(handle, buffer_node, NULL, &is_valid, NULL);

		if (!is_valid)
			cnt++;
	}

	return cnt;
}

#ifdef STARPU_USE_TOP
static void param_modified(struct starpu_top_param* d)
{
#ifdef STARPU_DEVEL
#warning FIXME: get sched ctx to get alpha/beta/gamma/idle values
#endif
	/* Just to show parameter modification. */
	_STARPU_MSG("%s has been modified : "
		    "alpha=%f|beta=%f|gamma=%f|idle_power=%f !\n",
		    d->name, alpha,beta,_gamma, idle_power);
}
#endif /* !STARPU_USE_TOP */

static int _normalize_prio(int priority, int num_priorities, unsigned sched_ctx_id)
{
	int min = starpu_sched_ctx_get_min_priority(sched_ctx_id);
	int max = starpu_sched_ctx_get_max_priority(sched_ctx_id);
	return ((num_priorities-1)/(max-min)) * (priority - min);
}

static struct starpu_task *_starpu_fifo_pop_first_ready_task(struct _starpu_fifo_taskq *fifo_queue, unsigned node, int num_priorities)
{
	struct starpu_task *task = NULL, *current;

	if (fifo_queue->ntasks == 0)
		return NULL;

	if (fifo_queue->ntasks > 0)
	{
		fifo_queue->ntasks--;

		task = starpu_task_list_front(&fifo_queue->taskq);
		if (STARPU_UNLIKELY(!task))
			return NULL;

		int first_task_priority = task->priority;

		current = task;

		int non_ready_best = INT_MAX;

		while (current)
		{
			int priority = current->priority;

			if (priority >= first_task_priority)
			{
				int non_ready = count_non_ready_buffers(current, node);
				if (non_ready < non_ready_best)
				{
					non_ready_best = non_ready;
					task = current;

					if (non_ready == 0)
						break;
				}
			}

			current = current->next;
		}

		if(num_priorities != -1)
		{
			int i;
			int task_prio = _normalize_prio(task->priority, num_priorities, task->sched_ctx);
			for(i = 0; i <= task_prio; i++)
				fifo_queue->ntasks_per_priority[i]--;
		}

		starpu_task_list_erase(&fifo_queue->taskq, task);
	}

	return task;
}

static struct starpu_task *dmda_pop_ready_task(unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	struct starpu_task *task;

	unsigned workerid = starpu_worker_get_id_check();
	struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid];

	unsigned node = starpu_worker_get_memory_node(workerid);

	/* Take the opportunity to update start time */
	fifo->exp_start = STARPU_MAX(starpu_timing_now(), fifo->exp_start);
	fifo->exp_end = fifo->exp_start + fifo->exp_len;

	task = _starpu_fifo_pop_first_ready_task(fifo, node, dt->num_priorities);
	if (task)
	{
#ifdef STARPU_VERBOSE
		if (task->cl)
		{
			int non_ready = count_non_ready_buffers(task, node);
			if (non_ready == 0)
				dt->ready_task_cnt++;
		}

		dt->total_task_cnt++;
#endif
	}

	return task;
}

static struct starpu_task *dmda_pop_task(unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	struct starpu_task *task;

	unsigned workerid = starpu_worker_get_id_check();
	struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid];

	/* Take the opportunity to update start time */
	fifo->exp_start = STARPU_MAX(starpu_timing_now(), fifo->exp_start);
	fifo->exp_end = fifo->exp_start + fifo->exp_len;

	STARPU_ASSERT_MSG(fifo, "worker %u does not belong to ctx %u anymore.\n", workerid, sched_ctx_id);

	task = _starpu_fifo_pop_local_task(fifo);
	if (task)
	{
#ifdef STARPU_VERBOSE
		if (task->cl)
		{
			int non_ready = count_non_ready_buffers(task, starpu_worker_get_memory_node(workerid));
			if (non_ready == 0)
				dt->ready_task_cnt++;
		}
		
		dt->total_task_cnt++;
#endif
	}

	return task;
}

static struct starpu_task *dmda_pop_every_task(unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	struct starpu_task *new_list;

	unsigned workerid = starpu_worker_get_id_check();
	struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid];

	/* Take the opportunity to update start time */
	fifo->exp_start = STARPU_MAX(starpu_timing_now(), fifo->exp_start);
	fifo->exp_end = fifo->exp_start + fifo->exp_len;

	starpu_pthread_mutex_t *sched_mutex;
	starpu_pthread_cond_t *sched_cond;
	starpu_worker_get_sched_condition(workerid, &sched_mutex, &sched_cond);
	STARPU_PTHREAD_MUTEX_LOCK_SCHED(sched_mutex);
	new_list = _starpu_fifo_pop_every_task(fifo, workerid);
	STARPU_PTHREAD_MUTEX_UNLOCK_SCHED(sched_mutex);
	return new_list;
}

static int push_task_on_best_worker(struct starpu_task *task, int best_workerid,
				    double predicted, double predicted_transfer,
				    int prio, unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	/* make sure someone coule execute that task ! */
	STARPU_ASSERT(best_workerid != -1);
	unsigned child_sched_ctx = starpu_sched_ctx_worker_is_master_for_child_ctx(best_workerid, sched_ctx_id);
        if(child_sched_ctx != STARPU_NMAX_SCHED_CTXS)
        {
		starpu_sched_ctx_revert_task_counters(sched_ctx_id, task->flops);
                starpu_sched_ctx_move_task_to_ctx(task, child_sched_ctx);
                return 0;
        }

	struct _starpu_fifo_taskq *fifo = dt->queue_array[best_workerid];

	starpu_pthread_mutex_t *sched_mutex;
	starpu_pthread_cond_t *sched_cond;
	starpu_worker_get_sched_condition(best_workerid, &sched_mutex, &sched_cond);

#ifdef STARPU_USE_SC_HYPERVISOR
	starpu_sched_ctx_call_pushed_task_cb(best_workerid, sched_ctx_id);
#endif //STARPU_USE_SC_HYPERVISOR

	STARPU_PTHREAD_MUTEX_LOCK_SCHED(sched_mutex);

        /* Sometimes workers didn't take the tasks as early as we expected */
	fifo->exp_start = isnan(fifo->exp_start) ? starpu_timing_now() : STARPU_MAX(fifo->exp_start, starpu_timing_now());
	fifo->exp_end = fifo->exp_start + fifo->exp_len;
	fifo->exp_end = fifo->exp_start + fifo->exp_len;

	if ((starpu_timing_now() + predicted_transfer) < fifo->exp_end)
	{
		/* We may hope that the transfer will be finished by
		 * the start of the task. */
		predicted_transfer = 0.0;
	}
	else
	{
		/* The transfer will not be finished by then, take the
		 * remainder into account */
		predicted_transfer = (starpu_timing_now() + predicted_transfer) - fifo->exp_end;
	}

	if(!isnan(predicted_transfer)) 
	{
		fifo->exp_end += predicted_transfer;
		fifo->exp_len += predicted_transfer;
		if(dt->num_priorities != -1)
		{
			int i;
			int task_prio = _normalize_prio(task->priority, dt->num_priorities, task->sched_ctx);
			for(i = 0; i <= task_prio; i++)
				fifo->exp_len_per_priority[i] += predicted_transfer;
		}

	}

	if(!isnan(predicted))
	{
		fifo->exp_end += predicted;
		fifo->exp_len += predicted;
		if(dt->num_priorities != -1)
		{
			int i;
			int task_prio = _normalize_prio(task->priority, dt->num_priorities, task->sched_ctx);
			for(i = 0; i <= task_prio; i++)
				fifo->exp_len_per_priority[i] += predicted;
		}

	}

	STARPU_PTHREAD_MUTEX_UNLOCK_SCHED(sched_mutex);

	task->predicted = predicted;
	task->predicted_transfer = predicted_transfer;

#ifdef STARPU_USE_TOP
	starpu_top_task_prevision(task, best_workerid,
				  (unsigned long long)(fifo->exp_end-predicted)/1000,
				  (unsigned long long)fifo->exp_end/1000);
#endif /* !STARPU_USE_TOP */

	if (starpu_get_prefetch_flag())
	{
		unsigned memory_node = starpu_worker_get_memory_node(best_workerid);
		starpu_prefetch_task_input_on_node(task, memory_node);
	}

	STARPU_AYU_ADDTOTASKQUEUE(starpu_task_get_job_id(task), best_workerid);
	int ret = 0;
	if (prio)
	{
		STARPU_PTHREAD_MUTEX_LOCK_SCHED(sched_mutex);
		ret =_starpu_fifo_push_sorted_task(dt->queue_array[best_workerid], task);
		if(dt->num_priorities != -1)
		{
			int i;
			int task_prio = _normalize_prio(task->priority, dt->num_priorities, task->sched_ctx);
			for(i = 0; i <= task_prio; i++)
				dt->queue_array[best_workerid]->ntasks_per_priority[i]++;
		}


#if !defined(STARPU_NON_BLOCKING_DRIVERS) || defined(STARPU_SIMGRID)
		starpu_wakeup_worker_locked(best_workerid, sched_cond, sched_mutex);
#endif
		starpu_push_task_end(task);
		STARPU_PTHREAD_MUTEX_UNLOCK_SCHED(sched_mutex);
	}
	else
	{
		STARPU_PTHREAD_MUTEX_LOCK_SCHED(sched_mutex);
		starpu_task_list_push_back (&dt->queue_array[best_workerid]->taskq, task);
		dt->queue_array[best_workerid]->ntasks++;
		dt->queue_array[best_workerid]->nprocessed++;
#if !defined(STARPU_NON_BLOCKING_DRIVERS) || defined(STARPU_SIMGRID)
		starpu_wakeup_worker_locked(best_workerid, sched_cond, sched_mutex);
#endif
		starpu_push_task_end(task);
		STARPU_PTHREAD_MUTEX_UNLOCK_SCHED(sched_mutex);
	}

	return ret;
}

/* TODO: factorize with dmda!! */
static int _dm_push_task(struct starpu_task *task, unsigned prio, unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	unsigned worker, worker_ctx = 0;
	int best = -1;

	double best_exp_end = 0.0;
	double model_best = 0.0;
	double transfer_model_best = 0.0;

	int ntasks_best = -1;
	double ntasks_best_end = 0.0;
	int calibrating = 0;

	/* A priori, we know all estimations */
	int unknown = 0;

	unsigned best_impl = 0;
	unsigned nimpl;
	unsigned impl_mask;
	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);

	struct starpu_sched_ctx_iterator it;

	workers->init_iterator(workers, &it);
	while(workers->has_next_master(workers, &it))
	{
		worker = workers->get_next_master(workers, &it);
		struct _starpu_fifo_taskq *fifo  = dt->queue_array[worker];
		unsigned memory_node = starpu_worker_get_memory_node(worker);
		struct starpu_perfmodel_arch* perf_arch = starpu_worker_get_perf_archtype(worker, sched_ctx_id);

		/* Sometimes workers didn't take the tasks as early as we expected */
		double exp_start = isnan(fifo->exp_start) ? starpu_timing_now() : STARPU_MAX(fifo->exp_start, starpu_timing_now());

		if (!starpu_worker_can_execute_task_impl(worker, task, &impl_mask))
			continue;

		for (nimpl = 0; nimpl < STARPU_MAXIMPLEMENTATIONS; nimpl++)
		{
			if (!(impl_mask & (1U << nimpl)))
			{
				/* no one on that queue may execute this task */
				continue;
			}

			double exp_end;
			double local_length = starpu_task_expected_length(task, perf_arch, nimpl);
			double local_penalty = starpu_task_expected_data_transfer_time(memory_node, task);
			double ntasks_end = fifo->ntasks / starpu_worker_get_relative_speedup(perf_arch);

			//_STARPU_DEBUG("Scheduler dm: task length (%lf) worker (%u) kernel (%u) \n", local_length,worker,nimpl);

			/*
			 * This implements a default greedy scheduler for the
			 * case of tasks which have no performance model, or
			 * whose performance model is not calibrated yet.
			 *
			 * It simply uses the number of tasks already pushed to
			 * the workers, divided by the relative performance of
			 * a CPU and of a GPU.
			 *
			 * This is always computed, but the ntasks_best
			 * selection is only really used if the task indeed has
			 * no performance model, or is not calibrated yet.
			 */
			if (ntasks_best == -1
			
			    /* Always compute the greedy decision, at least for
			     * the tasks with no performance model. */
			    || (!calibrating && ntasks_end < ntasks_best_end)

			    /* The performance model of this task is not
			     * calibrated on this worker, try to run it there
			     * to calibrate it there. */
			    || (!calibrating && isnan(local_length))

			    /* the performance model of this task is not
			     * calibrated on this worker either, rather run it
			     * there if this one is low on scheduled tasks. */
			    || (calibrating && isnan(local_length) && ntasks_end < ntasks_best_end)
				)
			{
				ntasks_best_end = ntasks_end;
				ntasks_best = worker;
				best_impl = nimpl;
			}

			if (isnan(local_length))
			{
				/* we are calibrating, we want to speed-up calibration time
				 * so we privilege non-calibrated tasks (but still
				 * greedily distribute them to avoid dumb schedules) */
				static int warned;
				if (!warned)
				{
					warned = 1;
					_STARPU_DISP("Warning: performance model for %s not finished calibrating on worker %u, using a dumb scheduling heuristic for now\n", starpu_task_get_name(task), worker);
				}
				calibrating = 1;
			}

			if (isnan(local_length) || _STARPU_IS_ZERO(local_length))
				/* there is no prediction available for that task
				 * with that arch yet, so switch to a greedy strategy */
				unknown = 1;

			if (unknown)
				continue;

			exp_end = exp_start + fifo->exp_len + local_length;

			if (best == -1 || exp_end < best_exp_end)
			{
				/* a better solution was found */
				best_exp_end = exp_end;
				best = worker;
				model_best = local_length;
				transfer_model_best = local_penalty;
				best_impl = nimpl;
			}
		}
	}

	if (unknown)
	{
		best = ntasks_best;
		model_best = 0.0;
		transfer_model_best = 0.0;
#ifdef STARPU_VERBOSE
		dt->eager_task_cnt++;
#endif
	}

	//_STARPU_DEBUG("Scheduler dm: kernel (%u)\n", best_impl);

	starpu_task_set_implementation(task, best_impl);

	starpu_sched_task_break(task);
	/* we should now have the best worker in variable "best" */
	return push_task_on_best_worker(task, best,
					model_best, transfer_model_best, prio, sched_ctx_id);
}

/* TODO: factorise CPU computations, expensive with a lot of cores */
static void compute_all_performance_predictions(struct starpu_task *task,
						unsigned nworkers,
						double local_task_length[nworkers][STARPU_MAXIMPLEMENTATIONS],
						double exp_end[nworkers][STARPU_MAXIMPLEMENTATIONS],
						double *max_exp_endp,
						double *best_exp_endp,
						double local_data_penalty[nworkers][STARPU_MAXIMPLEMENTATIONS],
						double local_energy[nworkers][STARPU_MAXIMPLEMENTATIONS],
						int *forced_worker, int *forced_impl, unsigned sched_ctx_id, unsigned sorted_decision)
{
	int calibrating = 0;
	double max_exp_end = DBL_MIN;
	double best_exp_end = DBL_MAX;
	int ntasks_best = -1;
	int nimpl_best = 0;
	double ntasks_best_end = 0.0;

	/* A priori, we know all estimations */
	int unknown = 0;
	unsigned worker, worker_ctx = 0;

	unsigned nimpl;
	unsigned impl_mask;
	int task_prio = 0;

	starpu_task_bundle_t bundle = task->bundle;
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	
	if(sorted_decision && dt->num_priorities != -1)
		task_prio = _normalize_prio(task->priority, dt->num_priorities, sched_ctx_id);

	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);

	struct starpu_sched_ctx_iterator it;

	workers->init_iterator(workers, &it);
	while(workers->has_next_master(workers, &it))
	{
		worker = workers->get_next_master(workers, &it);

		struct _starpu_fifo_taskq *fifo = dt->queue_array[worker];
		struct starpu_perfmodel_arch* perf_arch = starpu_worker_get_perf_archtype(worker, sched_ctx_id);
		unsigned memory_node = starpu_worker_get_memory_node(worker);

		STARPU_ASSERT_MSG(fifo != NULL, "worker %u ctx %u\n", worker, sched_ctx_id);

		/* Sometimes workers didn't take the tasks as early as we expected */
		double exp_start = isnan(fifo->exp_start) ? starpu_timing_now() : STARPU_MAX(fifo->exp_start, starpu_timing_now());
		if (!starpu_worker_can_execute_task_impl(worker, task, &impl_mask))
			continue;

		for (nimpl  = 0; nimpl < STARPU_MAXIMPLEMENTATIONS; nimpl++)
		{
			if (!(impl_mask & (1U << nimpl)))
			{
				/* no one on that queue may execute this task */
				continue;
			}

			int fifo_ntasks = fifo->ntasks;
			double prev_exp_len = fifo->exp_len;
			/* consider the priority of the task when deciding on which worker to schedule, 
			   compute the expected_end of the task if it is inserted before other tasks already scheduled */
			if(sorted_decision)
			{
				if(dt->num_priorities != -1)
				{
					prev_exp_len = fifo->exp_len_per_priority[task_prio];
					fifo_ntasks = fifo->ntasks_per_priority[task_prio];
				}
				else
				{
					starpu_pthread_mutex_t *sched_mutex;
					starpu_pthread_cond_t *sched_cond;
					starpu_worker_get_sched_condition(worker, &sched_mutex, &sched_cond);
					STARPU_PTHREAD_MUTEX_LOCK_SCHED(sched_mutex);
					prev_exp_len = _starpu_fifo_get_exp_len_prev_task_list(fifo, task, worker, nimpl, &fifo_ntasks);
					STARPU_PTHREAD_MUTEX_UNLOCK_SCHED(sched_mutex);
				}
			}
				
			exp_end[worker_ctx][nimpl] = exp_start + prev_exp_len;
			if (exp_end[worker_ctx][nimpl] > max_exp_end)
				max_exp_end = exp_end[worker_ctx][nimpl];

			//_STARPU_DEBUG("Scheduler dmda: task length (%lf) worker (%u) kernel (%u) \n", local_task_length[worker][nimpl],worker,nimpl);

			if (bundle)
			{
				/* TODO : conversion time */
				local_task_length[worker_ctx][nimpl] = starpu_task_bundle_expected_length(bundle, perf_arch, nimpl);
				local_data_penalty[worker_ctx][nimpl] = starpu_task_bundle_expected_data_transfer_time(bundle, memory_node);
				local_energy[worker_ctx][nimpl] = starpu_task_bundle_expected_energy(bundle, perf_arch,nimpl);

			}
			else
			{
				local_task_length[worker_ctx][nimpl] = starpu_task_expected_length(task, perf_arch, nimpl);
				local_data_penalty[worker_ctx][nimpl] = starpu_task_expected_data_transfer_time(memory_node, task);
				local_energy[worker_ctx][nimpl] = starpu_task_expected_energy(task, perf_arch,nimpl);
				double conversion_time = starpu_task_expected_conversion_time(task, perf_arch, nimpl);
				if (conversion_time > 0.0)
					local_task_length[worker_ctx][nimpl] += conversion_time;
			}
			double ntasks_end = fifo_ntasks / starpu_worker_get_relative_speedup(perf_arch);

			/*
			 * This implements a default greedy scheduler for the
			 * case of tasks which have no performance model, or
			 * whose performance model is not calibrated yet.
			 *
			 * It simply uses the number of tasks already pushed to
			 * the workers, divided by the relative performance of
			 * a CPU and of a GPU.
			 *
			 * This is always computed, but the ntasks_best
			 * selection is only really used if the task indeed has
			 * no performance model, or is not calibrated yet.
			 */
			if (ntasks_best == -1

			    /* Always compute the greedy decision, at least for
			     * the tasks with no performance model. */
			    || (!calibrating && ntasks_end < ntasks_best_end)

			    /* The performance model of this task is not
			     * calibrated on this worker, try to run it there
			     * to calibrate it there. */
			    || (!calibrating && isnan(local_task_length[worker_ctx][nimpl]))

			    /* the performance model of this task is not
			     * calibrated on this worker either, rather run it
			     * there if this one is low on scheduled tasks. */
			    || (calibrating && isnan(local_task_length[worker_ctx][nimpl]) && ntasks_end < ntasks_best_end)
				)
			{
				ntasks_best_end = ntasks_end;
				ntasks_best = worker;
				nimpl_best = nimpl;
			}

			if (isnan(local_task_length[worker_ctx][nimpl]))
				/* we are calibrating, we want to speed-up calibration time
				 * so we privilege non-calibrated tasks (but still
				 * greedily distribute them to avoid dumb schedules) */
				calibrating = 1;

			if (isnan(local_task_length[worker_ctx][nimpl])
					|| _STARPU_IS_ZERO(local_task_length[worker_ctx][nimpl]))
				/* there is no prediction available for that task
				 * with that arch (yet or at all), so switch to a greedy strategy */
				unknown = 1;

			if (unknown)
				continue;

			exp_end[worker_ctx][nimpl] = exp_start + prev_exp_len + local_task_length[worker_ctx][nimpl];

			if (exp_end[worker_ctx][nimpl] < best_exp_end)
			{
				/* a better solution was found */
				best_exp_end = exp_end[worker_ctx][nimpl];
				nimpl_best = nimpl;
			}

			if (isnan(local_energy[worker_ctx][nimpl]))
				local_energy[worker_ctx][nimpl] = 0.;

		}
		worker_ctx++;
	}

	*forced_worker = unknown?ntasks_best:-1;
	*forced_impl = unknown?nimpl_best:-1;

#ifdef STARPU_VERBOSE
	if (unknown)
	{
		dt->eager_task_cnt++;
	}
#endif

	*best_exp_endp = best_exp_end;
	*max_exp_endp = max_exp_end;
}

static double _dmda_push_task(struct starpu_task *task, unsigned prio, unsigned sched_ctx_id, unsigned simulate, unsigned sorted_decision)
{
	/* find the queue */
	unsigned worker, worker_ctx = 0;
	int best = -1, best_in_ctx = -1;
	int selected_impl = 0;
	double model_best = 0.0;
	double transfer_model_best = 0.0;

	/* this flag is set if the corresponding worker is selected because
	   there is no performance prediction available yet */
	int forced_best = -1;
	int forced_impl = -1;

	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);
	unsigned nworkers_ctx = workers->nworkers;
	double local_task_length[nworkers_ctx][STARPU_MAXIMPLEMENTATIONS];
	double local_data_penalty[nworkers_ctx][STARPU_MAXIMPLEMENTATIONS];
	double local_energy[nworkers_ctx][STARPU_MAXIMPLEMENTATIONS];

	/* Expected end of this task on the workers */
	double exp_end[nworkers_ctx][STARPU_MAXIMPLEMENTATIONS];

	/* This is the minimum among the exp_end[] matrix */
	double best_exp_end;

	/* This is the maximum termination time of already-scheduled tasks over all workers */
	double max_exp_end = 0.0;

	double fitness[nworkers_ctx][STARPU_MAXIMPLEMENTATIONS];


	compute_all_performance_predictions(task,
					    nworkers_ctx,
					    local_task_length,
					    exp_end,
					    &max_exp_end,
					    &best_exp_end,
					    local_data_penalty,
					    local_energy,
					    &forced_best,
					    &forced_impl, sched_ctx_id, sorted_decision);
	
	
	double best_fitness = -1;

	unsigned nimpl;
	unsigned impl_mask;
	if (forced_best == -1)
	{
		struct starpu_sched_ctx_iterator it;

		workers->init_iterator(workers, &it);
		while(workers->has_next_master(workers, &it))
		{
			worker = workers->get_next_master(workers, &it);
			if (!starpu_worker_can_execute_task_impl(worker, task, &impl_mask))
				continue;
			for (nimpl = 0; nimpl < STARPU_MAXIMPLEMENTATIONS; nimpl++)
			{
				if (!(impl_mask & (1U << nimpl)))
				{
					/* no one on that queue may execute this task */
					continue;
				}
				fitness[worker_ctx][nimpl] = dt->alpha*(exp_end[worker_ctx][nimpl] - best_exp_end)
					+ dt->beta*(local_data_penalty[worker_ctx][nimpl])
					+ dt->_gamma*(local_energy[worker_ctx][nimpl]);

				if (exp_end[worker_ctx][nimpl] > max_exp_end)
				{
					/* This placement will make the computation
					 * longer, take into account the idle
					 * consumption of other cpus */
					fitness[worker_ctx][nimpl] += dt->_gamma * dt->idle_power * (exp_end[worker_ctx][nimpl] - max_exp_end) / 1000000.0;
				}

				if (best == -1 || fitness[worker_ctx][nimpl] < best_fitness)
				{
					/* we found a better solution */
					best_fitness = fitness[worker_ctx][nimpl];
					best = worker;
					best_in_ctx = worker_ctx;
					selected_impl = nimpl;

					//_STARPU_DEBUG("best fitness (worker %d) %e = alpha*(%e) + beta(%e) +gamma(%e)\n", worker, best_fitness, exp_end[worker][nimpl] - best_exp_end, local_data_penalty[worker][nimpl], local_energy[worker][nimpl]);

				}
			}
			worker_ctx++;
		}
	}
	STARPU_ASSERT(forced_best != -1 || best != -1);

	if (forced_best != -1)
	{
		/* there is no prediction available for that task
		 * with that arch we want to speed-up calibration time
		 * so we force this measurement */
		best = forced_best;
		selected_impl = forced_impl;
		model_best = 0.0;
		transfer_model_best = 0.0;
	}
	else if (task->bundle)
	{
		struct starpu_perfmodel_arch* perf_arch = starpu_worker_get_perf_archtype(best_in_ctx, sched_ctx_id);
		unsigned memory_node = starpu_worker_get_memory_node(best);
		model_best = starpu_task_expected_length(task, perf_arch, selected_impl);
		transfer_model_best = starpu_task_expected_data_transfer_time(memory_node, task);
	}
	else
	{
		model_best = local_task_length[best_in_ctx][selected_impl];
		transfer_model_best = local_data_penalty[best_in_ctx][selected_impl];
	}

	//_STARPU_DEBUG("Scheduler dmda: kernel (%u)\n", best_impl);
	starpu_task_set_implementation(task, selected_impl);

	starpu_sched_task_break(task);
	if(!simulate)
	{
		/* we should now have the best worker in variable "best" */
		return push_task_on_best_worker(task, best, model_best, transfer_model_best, prio, sched_ctx_id);
	}
	else
	{
//		double max_len = (max_exp_end - starpu_timing_now());
		/* printf("%d: dmda max_exp_end %lf best_exp_end %lf max_len %lf \n", sched_ctx_id, max_exp_end/1000000.0, best_exp_end/1000000.0, max_len/1000000.0);	 */
		return exp_end[best_in_ctx][selected_impl] ;
	}
}

static int dmda_push_sorted_decision_task(struct starpu_task *task)
{
	return _dmda_push_task(task, 1, task->sched_ctx, 0, 1);
}

static int dmda_push_sorted_task(struct starpu_task *task)
{
#ifdef STARPU_DEVEL
#warning TODO: after defining a scheduling window, use that instead of empty_ctx_tasks
#endif
	return _dmda_push_task(task, 1, task->sched_ctx, 0, 0);
}

static double get_task_heter_ratio(unsigned sched_ctx_id,struct starpu_task* task){
	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);
	struct starpu_sched_ctx_iterator it;
	struct starpu_sched_ctx_iterator it1;

	double max_execution_time = 0;
	double max_heter_tatio = 0;
	unsigned worker;
	unsigned impl_mask;
	unsigned nimpl;

	workers->init_iterator(workers, &it);
	while(workers->has_next_master(workers, &it))
	{
		worker = workers->get_next_master(workers, &it);
		unsigned memory_node = starpu_worker_get_memory_node(worker);
		struct starpu_perfmodel_arch* perf_arch = starpu_worker_get_perf_archtype(worker, sched_ctx_id);
		if (!starpu_worker_can_execute_task_impl(worker, task, &impl_mask))
			continue;

		for (nimpl = 0; nimpl < STARPU_MAXIMPLEMENTATIONS; nimpl++)
		{
			if (!(impl_mask & (1U << nimpl)))
			{
				/* no one on that queue may execute this task */
				continue;
			}
			double local_length = 1+starpu_task_expected_length(task, perf_arch, nimpl);
			printf("expected length is %lf\n", local_length);
			if (local_length>max_execution_time) max_execution_time = local_length;
		}
	}
	printf("the max_execution_time is %lf\n", max_execution_time);

	workers->init_iterator(workers, &it1);
	while(workers->has_next_master(workers, &it1))
	{
		worker = workers->get_next_master(workers, &it1);
		unsigned memory_node = starpu_worker_get_memory_node(worker);
		struct starpu_perfmodel_arch* perf_arch = starpu_worker_get_perf_archtype(worker, sched_ctx_id);
		if (!starpu_worker_can_execute_task_impl(worker, task, &impl_mask))
			continue;

		for (nimpl = 0; nimpl < STARPU_MAXIMPLEMENTATIONS; nimpl++)
		{
			if (!(impl_mask & (1U << nimpl)))
			{
				/* no one on that queue may execute this task */
				continue;
			}
			double local_length = 1+ starpu_task_expected_length(task, perf_arch, nimpl);
			double heter_ratio = max_execution_time/local_length;
			if(heter_ratio>max_heter_tatio)max_heter_tatio = heter_ratio;
		}
	}
	return max_heter_tatio;
}


static int dm_push_task(struct starpu_task *task)
{
	unsigned sched_ctx_id = task->sched_ctx;
	struct _starpu_dmda_data *data = (struct dummy_sched_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	STARPU_PTHREAD_MUTEX_LOCK(&data->policy_mutex);

    double max_heter_ratio = get_task_heter_ratio(sched_ctx_id, task);
	if(starpu_task_list_empty(&data->main_list)){
		starpu_task_list_push_back(&data->main_list, task);
	}
	else{
		struct starpu_task* current = starpu_task_list_begin(&data->main_list);
		if(max_heter_ratio>get_task_heter_ratio(sched_ctx_id, current))
		{
			starpu_task_list_push_front(&data->main_list, task);
		}
		else{
			current = current->next;
			while(current!= NULL){
				if(max_heter_ratio>get_task_heter_ratio(sched_ctx_id, current)){
					current->prev->next = task;
					task->next = current;
					task->prev = current->prev;
					current->prev = task;
				}
			}
		}
	}
	_dm_push_task(starpu_task_list_begin(&data->main_list), 0, starpu_task_list_begin(&data->main_list)->sched_ctx);
	starpu_push_task_end(task);
	STARPU_PTHREAD_MUTEX_UNLOCK(&data->policy_mutex);

	return 0;
}

static int dmda_push_task(struct starpu_task *task)
{
	STARPU_ASSERT(task);
	return _dmda_push_task(task, 0, task->sched_ctx, 0, 0);
}
static double dmda_simulate_push_task(struct starpu_task *task)
{
	STARPU_ASSERT(task);
	return _dmda_push_task(task, 0, task->sched_ctx, 1, 0);
}

static double dmda_simulate_push_sorted_task(struct starpu_task *task)
{
	STARPU_ASSERT(task);
	return _dmda_push_task(task, 1, task->sched_ctx, 1, 0);
}

static double dmda_simulate_push_sorted_decision_task(struct starpu_task *task)
{
	STARPU_ASSERT(task);
	return _dmda_push_task(task, 1, task->sched_ctx, 1, 1);
}

static void dmda_add_workers(unsigned sched_ctx_id, int *workerids, unsigned nworkers)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	unsigned i;
	for (i = 0; i < nworkers; i++)
	{
		struct _starpu_fifo_taskq *q;
		int workerid = workerids[i];
		/* if the worker has alreadry belonged to this context
		   the queue and the synchronization variables have been already initialized */
		q = dt->queue_array[workerid];
		if(q == NULL)
		{
			q = dt->queue_array[workerid] = _starpu_create_fifo();
			/* These are only stats, they can be read with races */
			STARPU_HG_DISABLE_CHECKING(q->exp_start);
			STARPU_HG_DISABLE_CHECKING(q->exp_len);
			STARPU_HG_DISABLE_CHECKING(q->exp_end);
		}

		if(dt->num_priorities != -1)
		{
			_STARPU_MALLOC(q->exp_len_per_priority, dt->num_priorities*sizeof(double));
			_STARPU_MALLOC(q->ntasks_per_priority, dt->num_priorities*sizeof(unsigned));
			int j;
			for(j = 0; j < dt->num_priorities; j++)
			{
				q->exp_len_per_priority[j] = 0.0;
				q->ntasks_per_priority[j] = 0;
			}
		}
	}
}

static void dmda_remove_workers(unsigned sched_ctx_id, int *workerids, unsigned nworkers)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	int workerid;
	unsigned i;
	for (i = 0; i < nworkers; i++)
	{
		workerid = workerids[i];
		if(dt->queue_array[workerid] != NULL)
		{
			if(dt->num_priorities != -1)
			{
				free(dt->queue_array[workerid]->exp_len_per_priority);
				free(dt->queue_array[workerid]->ntasks_per_priority);
			}

			_starpu_destroy_fifo(dt->queue_array[workerid]);
			dt->queue_array[workerid] = NULL;
		}
	}
}

static void initialize_dmda_policy(unsigned sched_ctx_id)
{
	starpu_sched_ctx_create_worker_collection(sched_ctx_id, STARPU_WORKER_LIST);

	struct _starpu_dmda_data *dt;
	_STARPU_CALLOC(dt, 1, sizeof(struct _starpu_dmda_data));

	starpu_sched_ctx_set_policy_data(sched_ctx_id, (void*)dt);

	_STARPU_MALLOC(dt->queue_array, STARPU_NMAXWORKERS*sizeof(struct _starpu_fifo_taskq*));

	int i;
	for(i = 0; i < STARPU_NMAXWORKERS; i++)
		dt->queue_array[i] = NULL;

	dt->alpha = starpu_get_env_float_default("STARPU_SCHED_ALPHA", _STARPU_SCHED_ALPHA_DEFAULT);
	dt->beta = starpu_get_env_float_default("STARPU_SCHED_BETA", _STARPU_SCHED_BETA_DEFAULT);
	dt->_gamma = starpu_get_env_float_default("STARPU_SCHED_GAMMA", _STARPU_SCHED_GAMMA_DEFAULT);
	dt->idle_power = starpu_get_env_float_default("STARPU_IDLE_POWER", 0.0);
	STARPU_PTHREAD_MUTEX_INIT(&dt->policy_mutex, NULL);
	starpu_task_list_init(&dt->main_list);
	if(starpu_sched_ctx_min_priority_is_set(sched_ctx_id) != 0 && starpu_sched_ctx_max_priority_is_set(sched_ctx_id) != 0)
		dt->num_priorities = starpu_sched_ctx_get_max_priority(sched_ctx_id) - starpu_sched_ctx_get_min_priority(sched_ctx_id) + 1;
	else 
		dt->num_priorities = -1;


#ifdef STARPU_USE_TOP
	/* FIXME: broken, needs to access context variable */
	starpu_top_register_parameter_float("DMDA_ALPHA", &alpha,
					    alpha_minimum, alpha_maximum, param_modified);
	starpu_top_register_parameter_float("DMDA_BETA", &beta,
					    beta_minimum, beta_maximum, param_modified);
	starpu_top_register_parameter_float("DMDA_GAMMA", &_gamma,
					    gamma_minimum, gamma_maximum, param_modified);
	starpu_top_register_parameter_float("DMDA_IDLE_POWER", &idle_power,
					    idle_power_minimum, idle_power_maximum, param_modified);
#endif /* !STARPU_USE_TOP */
}

static void initialize_dmda_sorted_policy(unsigned sched_ctx_id)
{
	initialize_dmda_policy(sched_ctx_id);

	/* The application may use any integer */
	if (starpu_sched_ctx_min_priority_is_set(sched_ctx_id) == 0)
		starpu_sched_ctx_set_min_priority(sched_ctx_id, INT_MIN);
	if (starpu_sched_ctx_max_priority_is_set(sched_ctx_id) == 0)
		starpu_sched_ctx_set_max_priority(sched_ctx_id, INT_MAX);
}

static void deinitialize_dmda_policy(unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
#ifdef STARPU_VERBOSE
	{
	struct _starpu_sched_ctx *sched_ctx = _starpu_get_sched_ctx_struct(sched_ctx_id);
	long int modelled_task_cnt = dt->total_task_cnt - dt->eager_task_cnt;
	_STARPU_DEBUG("%s sched policy (sched_ctx %u): total_task_cnt %ld ready_task_cnt %ld (%.1f%%), modelled_task_cnt = %ld (%.1f%%)%s\n",
		sched_ctx->sched_policy?sched_ctx->sched_policy->policy_name:"<none>",
		sched_ctx_id,
		dt->total_task_cnt,
		dt->ready_task_cnt,
		(100.0f*dt->ready_task_cnt)/dt->total_task_cnt,
		modelled_task_cnt,
		(100.0f*modelled_task_cnt)/dt->total_task_cnt,
		modelled_task_cnt==0?" *** Check if performance models are enabled and converging on a per-codelet basis, or use an non-modeling scheduling policy. ***":"");
	}
#endif

	free(dt->queue_array);
	free(dt);
	starpu_sched_ctx_delete_worker_collection(sched_ctx_id);
}

/* dmda_pre_exec_hook is called right after the data transfer is done and right
 * before the computation to begin, it is useful to update more precisely the
 * value of the expected start, end, length, etc... */
static void dmda_pre_exec_hook(struct starpu_task *task)
{
	unsigned sched_ctx_id = task->sched_ctx;
	unsigned workerid = starpu_worker_get_id_check();
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid];
	double model = task->predicted;
	double transfer_model = task->predicted_transfer;

	starpu_pthread_mutex_t *sched_mutex;
	starpu_pthread_cond_t *sched_cond;
	starpu_worker_get_sched_condition(workerid, &sched_mutex, &sched_cond);

	/* Once the task is executing, we can update the predicted amount
	 * of work. */
	STARPU_PTHREAD_MUTEX_LOCK_SCHED(sched_mutex);

	/* Take the opportunity to update start time */
	fifo->exp_start = STARPU_MAX(starpu_timing_now(), fifo->exp_start);
	fifo->exp_end = fifo->exp_start + fifo->exp_len;

	if(!isnan(transfer_model))
	{
		/* The transfer is over, get rid of it in the completion
		 * prediction */
		fifo->exp_len -= transfer_model;
		if(dt->num_priorities != -1)
		{
			int i;
			int task_prio = _normalize_prio(task->priority, dt->num_priorities, task->sched_ctx);
			for(i = 0; i <= task_prio; i++)
				fifo->exp_len_per_priority[i] -= transfer_model;
		}

	}

	if(!isnan(model))
	{
		/* We now start the computation, get rid of it in the completion
		 * prediction */
		fifo->exp_len -= model;
		fifo->exp_start += model;
		if(dt->num_priorities != -1)
		{
			int i;
			int task_prio = _normalize_prio(task->priority, dt->num_priorities, task->sched_ctx);
			for(i = 0; i <= task_prio; i++)
				fifo->exp_len_per_priority[i] -= model;
		}
	}

	fifo->exp_end = fifo->exp_start + fifo->exp_len;
	STARPU_PTHREAD_MUTEX_UNLOCK_SCHED(sched_mutex);
}

static void dmda_push_task_notify(struct starpu_task *task, int workerid, int perf_workerid, unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid];
	/* Compute the expected penality */
	struct starpu_perfmodel_arch *perf_arch = starpu_worker_get_perf_archtype(perf_workerid, sched_ctx_id);
	unsigned memory_node = starpu_worker_get_memory_node(workerid);

	double predicted = starpu_task_expected_length(task, perf_arch,
						       starpu_task_get_implementation(task));

	double predicted_transfer = starpu_task_expected_data_transfer_time(memory_node, task);
	starpu_pthread_mutex_t *sched_mutex;
	starpu_pthread_cond_t *sched_cond;
	starpu_worker_get_sched_condition(workerid, &sched_mutex, &sched_cond);


	/* Update the predictions */
	STARPU_PTHREAD_MUTEX_LOCK_SCHED(sched_mutex);
	/* Sometimes workers didn't take the tasks as early as we expected */
	fifo->exp_start = isnan(fifo->exp_start) ? starpu_timing_now() : STARPU_MAX(fifo->exp_start, starpu_timing_now());
	fifo->exp_end = fifo->exp_start + fifo->exp_len;

	/* If there is no prediction available, we consider the task has a null length */
	if (!isnan(predicted_transfer))
	{
		if (starpu_timing_now() + predicted_transfer < fifo->exp_end)
		{
			/* We may hope that the transfer will be finished by
			 * the start of the task. */
			predicted_transfer = 0;
		}
		else
		{
			/* The transfer will not be finished by then, take the
			 * remainder into account */
			predicted_transfer = (starpu_timing_now() + predicted_transfer) - fifo->exp_end;
		}
		task->predicted_transfer = predicted_transfer;
		fifo->exp_end += predicted_transfer;
		fifo->exp_len += predicted_transfer;
		if(dt->num_priorities != -1)
		{
			int i;
			int task_prio = _normalize_prio(task->priority, dt->num_priorities, task->sched_ctx);
			for(i = 0; i <= task_prio; i++)
				fifo->exp_len_per_priority[i] += predicted_transfer;
		}

	}

	/* If there is no prediction available, we consider the task has a null length */
	if (!isnan(predicted))
	{
		task->predicted = predicted;
		fifo->exp_end += predicted;
		fifo->exp_len += predicted;
		if(dt->num_priorities != -1)
		{
			int i;
			int task_prio = _normalize_prio(task->priority, dt->num_priorities, task->sched_ctx);
			for(i = 0; i <= task_prio; i++)
				fifo->exp_len_per_priority[i] += predicted;
		}

	}
	if(dt->num_priorities != -1)
	{
		int i;
		int task_prio = _normalize_prio(task->priority, dt->num_priorities, task->sched_ctx);
		for(i = 0; i <= task_prio; i++)
			fifo->ntasks_per_priority[i]++;
	}

	fifo->ntasks++;

	STARPU_PTHREAD_MUTEX_UNLOCK_SCHED(sched_mutex);
}

static void dmda_post_exec_hook(struct starpu_task * task)
{

	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(task->sched_ctx);
	unsigned workerid = starpu_worker_get_id_check();
	struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid];
	starpu_pthread_mutex_t *sched_mutex;
	starpu_pthread_cond_t *sched_cond;
	starpu_worker_get_sched_condition(workerid, &sched_mutex, &sched_cond);
	STARPU_PTHREAD_MUTEX_LOCK_SCHED(sched_mutex);
	fifo->exp_start = starpu_timing_now();
	fifo->exp_end = fifo->exp_start + fifo->exp_len;
	STARPU_PTHREAD_MUTEX_UNLOCK_SCHED(sched_mutex);
}

struct starpu_sched_policy _starpu_sched_dm_policy =
{
	.init_sched = initialize_dmda_policy,
	.deinit_sched = deinitialize_dmda_policy,
	.add_workers = dmda_add_workers ,
	.remove_workers = dmda_remove_workers,
	.push_task = dm_push_task,
	.simulate_push_task = NULL,
	.pop_task = dmda_pop_task,
	.pre_exec_hook = dmda_pre_exec_hook,
	.post_exec_hook = dmda_post_exec_hook,
	.pop_every_task = dmda_pop_every_task,
	.policy_name = "dm hudhoighjporjeopjowopgjpojgpjowgjopjgpo",
	.policy_description = "performance model"
};





/* default value */
static unsigned ntasks = 1024;

static unsigned long long nshot_per_task = 16*1024*1024ULL;

void cpu_kernel(void *descr[], void *cl_arg)
{
	unsigned *directions = (unsigned *)STARPU_VECTOR_GET_PTR(descr[0]);
	unsigned nx = nshot_per_task;

	TYPE *random_numbers = malloc(2*nx*sizeof(TYPE));
	sobolCPU(2*nx/n_dimensions, n_dimensions, directions, random_numbers);

	TYPE *random_numbers_x = &random_numbers[0];
	TYPE *random_numbers_y = &random_numbers[nx];

	unsigned current_cnt = 0;

	unsigned i;
	for (i = 0; i < nx; i++)
	{
		TYPE x = random_numbers_x[i];
		TYPE y = random_numbers_y[i];

		TYPE dist = (x*x + y*y);

		unsigned success = (dist <= 1.0);
		current_cnt += success;
	}

	unsigned *cnt = (unsigned *)STARPU_VECTOR_GET_PTR(descr[1]);
	*cnt = current_cnt;
	printf("%d\n", current_cnt);

	free(random_numbers);
}

/* The amount of work does not depend on the data size at all :) */
static size_t size_base(struct starpu_task *task, unsigned nimpl)
{
	return nshot_per_task;
}

static void parse_args(int argc, char **argv)
{
	int i;
	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-ntasks") == 0)
		{
			char *argptr;
			ntasks = strtol(argv[++i], &argptr, 10);
		}

		if (strcmp(argv[i], "-nshot") == 0)
		{
			char *argptr;
			nshot_per_task = strtol(argv[++i], &argptr, 10);
		}
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
		{
			fprintf(stderr,"Usage: %s [options...]\n", argv[0]);
			fprintf(stderr,"\n");
			fprintf(stderr,"Options:\n");
			fprintf(stderr,"-ntasks <n>		select the number of tasks\n");
			fprintf(stderr,"-nshot <n>		select the number of shot per task\n");
			exit(0);
		}
	}
}

static struct starpu_perfmodel model =
{
	.type = STARPU_HISTORY_BASED,
	.size_base = size_base,
	.symbol = "monte_carlo_pi"
};

static struct starpu_codelet pi_cl =
{
	.cpu_funcs = {cpu_kernel},
	.cpu_funcs_name = {"cpu_kernel"},
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {cuda_kernel},
#endif
	.nbuffers = 2,
	.modes = {STARPU_R, STARPU_W},
	.model = &model
};

int main(int argc, char **argv)
{
	unsigned i;
	int ret;
	struct starpu_conf conf;
	parse_args(argc, argv);

#ifdef STARPU_HAVE_UNSETENV
	unsetenv("STARPU_SCHED");
#endif

	starpu_conf_init(&conf);
	conf.sched_policy = &_starpu_sched_dm_policy,
	ret = starpu_init(&conf);
	if (ret == -ENODEV)
		return 77;
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_init");
	/* Initialize the random number generator */
	unsigned *sobol_qrng_directions = malloc(n_dimensions*n_directions*sizeof(unsigned));
	STARPU_ASSERT(sobol_qrng_directions);

	initSobolDirectionVectors(n_dimensions, sobol_qrng_directions);

	/* Any worker may use that array now */
	starpu_data_handle_t sobol_qrng_direction_handle;
	starpu_vector_data_register(&sobol_qrng_direction_handle, STARPU_MAIN_RAM,
		(uintptr_t)sobol_qrng_directions, n_dimensions*n_directions, sizeof(unsigned));

	unsigned *cnt_array = calloc(ntasks, sizeof(unsigned));
	STARPU_ASSERT(cnt_array);
	starpu_data_handle_t cnt_array_handle;
	starpu_vector_data_register(&cnt_array_handle, STARPU_MAIN_RAM, (uintptr_t)cnt_array, ntasks, sizeof(unsigned));

	/* Use a write-through policy : when the data is modified on an
	 * accelerator, we know that it will only be modified once and be
	 * accessed by the CPU later on */
	starpu_data_set_wt_mask(cnt_array_handle, (1<<0));

	struct starpu_data_filter f =
	{
		.filter_func = starpu_vector_filter_block,
		.nchildren = ntasks
	};

	starpu_data_partition(cnt_array_handle, &f);

	double start;
	double end;

	start = starpu_timing_now();

	for (i = 0; i < ntasks; i++)
	{
		struct starpu_task *task = starpu_task_create();

		task->cl = &pi_cl;

		STARPU_ASSERT(starpu_data_get_sub_data(cnt_array_handle, 1, i));

		task->handles[0] = sobol_qrng_direction_handle;
		task->handles[1] = starpu_data_get_sub_data(cnt_array_handle, 1, i);

		ret = starpu_task_submit(task);
		STARPU_ASSERT(!ret);
	}

	starpu_task_wait_for_all();

	/* Get the cnt_array back in main memory */
	starpu_data_unpartition(cnt_array_handle, STARPU_MAIN_RAM);
	starpu_data_unregister(cnt_array_handle);
	starpu_data_unregister(sobol_qrng_direction_handle);

	/* Count the total number of entries */
	unsigned long total_cnt = 0;
	for (i = 0; i < ntasks; i++)
		total_cnt += cnt_array[i];

	end = starpu_timing_now();

	double timing = end - start;

	unsigned long total_shot_cnt = ntasks * nshot_per_task;

	/* Total surface : Pi * r^ 2 = Pi*1^2, total square surface : 2^2 = 4, probability to impact the disk: pi/4 */
	FPRINTF(stderr, "Pi approximation : %f (%lu / %lu)\n", ((TYPE)total_cnt*4)/(total_shot_cnt), total_cnt, total_shot_cnt);
	FPRINTF(stderr, "Total time : %f ms\n", timing/1000.0);
	FPRINTF(stderr, "Speed : %f GShot/s\n", total_shot_cnt/(1e3*timing));

	if (!getenv("STARPU_SSILENT")) starpu_codelet_display_stats(&pi_cl);

	starpu_shutdown();

	return 0;
}
