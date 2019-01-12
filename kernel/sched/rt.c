/*
 * Real-Time Scheduling Class (mapped to the SCHED_FIFO and SCHED_RR
 * policies)
 */

#include "sched.h"

#include <linux/slab.h>
#include <linux/irq_work.h>

#include "walt.h"
#include <trace/events/sched.h>
#include "tune.h"

int sched_rr_timeslice = RR_TIMESLICE;

static int do_sched_rt_period_timer(struct rt_bandwidth *rt_b, int overrun);

struct rt_bandwidth def_rt_bandwidth;

unsigned int sched_switch_to_rt_load_ratio;
unsigned int sched_switch_to_fair_load_ratio;
unsigned int sched_rt_remove_ratio_for_freq;

static enum hrtimer_restart sched_rt_period_timer(struct hrtimer *timer)
{
	struct rt_bandwidth *rt_b =
		container_of(timer, struct rt_bandwidth, rt_period_timer);
	int idle = 0;
	int overrun;

	raw_spin_lock(&rt_b->rt_runtime_lock);
	for (;;) {
		overrun = hrtimer_forward_now(timer, rt_b->rt_period);
		if (!overrun)
			break;

		raw_spin_unlock(&rt_b->rt_runtime_lock);
		idle = do_sched_rt_period_timer(rt_b, overrun);
		raw_spin_lock(&rt_b->rt_runtime_lock);
	}
	if (idle)
		rt_b->rt_period_active = 0;
	raw_spin_unlock(&rt_b->rt_runtime_lock);

	return idle ? HRTIMER_NORESTART : HRTIMER_RESTART;
}

void init_rt_bandwidth(struct rt_bandwidth *rt_b, u64 period, u64 runtime)
{
	rt_b->rt_period = ns_to_ktime(period);
	rt_b->rt_runtime = runtime;

	raw_spin_lock_init(&rt_b->rt_runtime_lock);

	hrtimer_init(&rt_b->rt_period_timer,
			CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	rt_b->rt_period_timer.function = sched_rt_period_timer;
}

static void start_rt_bandwidth(struct rt_bandwidth *rt_b)
{
	if (!rt_bandwidth_enabled() || rt_b->rt_runtime == RUNTIME_INF)
		return;

	raw_spin_lock(&rt_b->rt_runtime_lock);
	if (!rt_b->rt_period_active) {
		rt_b->rt_period_active = 1;
		/*
		 * SCHED_DEADLINE updates the bandwidth, as a run away
		 * RT task with a DL task could hog a CPU. But DL does
		 * not reset the period. If a deadline task was running
		 * without an RT task running, it can cause RT tasks to
		 * throttle when they start up. Kick the timer right away
		 * to update the period.
		 */
		hrtimer_forward_now(&rt_b->rt_period_timer, ns_to_ktime(0));
		hrtimer_start_expires(&rt_b->rt_period_timer, HRTIMER_MODE_ABS_PINNED);
	}
	raw_spin_unlock(&rt_b->rt_runtime_lock);
}

void init_rt_rq(struct rt_rq *rt_rq)
{
	struct rt_prio_array *array;
	int i;

	array = &rt_rq->active;
	for (i = 0; i < MAX_RT_PRIO; i++) {
		INIT_LIST_HEAD(array->queue + i);
		__clear_bit(i, array->bitmap);
	}
	/* delimiter for bitsearch: */
	__set_bit(MAX_RT_PRIO, array->bitmap);

#if defined CONFIG_SMP
	rt_rq->highest_prio.curr = MAX_RT_PRIO;
	rt_rq->highest_prio.next = MAX_RT_PRIO;
	rt_rq->rt_nr_migratory = 0;
	rt_rq->overloaded = 0;
	plist_head_init(&rt_rq->pushable_tasks);
	atomic_long_set(&rt_rq->removed_util_avg, 0);
	atomic_long_set(&rt_rq->removed_load_avg, 0);

#endif /* CONFIG_SMP */
	/* We start is dequeued state, because no RT tasks are queued */
	rt_rq->rt_queued = 0;

	rt_rq->rt_time = 0;
	rt_rq->rt_throttled = 0;
	rt_rq->rt_runtime = 0;
	raw_spin_lock_init(&rt_rq->rt_runtime_lock);
}

#ifdef CONFIG_RT_GROUP_SCHED
static void destroy_rt_bandwidth(struct rt_bandwidth *rt_b)
{
	hrtimer_cancel(&rt_b->rt_period_timer);
}

static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return rt_rq->rq;
}

static inline struct rt_rq *rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	return rt_se->rt_rq;
}

static inline struct rq *rq_of_rt_se(struct sched_rt_entity *rt_se)
{
	struct rt_rq *rt_rq = rt_se->rt_rq;

	return rt_rq->rq;
}

void free_rt_sched_group(struct task_group *tg)
{
	int i;

	if (tg->rt_se)
		destroy_rt_bandwidth(&tg->rt_bandwidth);

	for_each_possible_cpu(i) {
		if (tg->rt_rq)
			kfree(tg->rt_rq[i]);
		if (tg->rt_se)
			kfree(tg->rt_se[i]);
	}

	kfree(tg->rt_rq);
	kfree(tg->rt_se);
}

void init_tg_rt_entry(struct task_group *tg, struct rt_rq *rt_rq,
		struct sched_rt_entity *rt_se, int cpu,
		struct sched_rt_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	rt_rq->highest_prio.curr = MAX_RT_PRIO;
	rt_rq->rt_nr_boosted = 0;
	rt_rq->rq = rq;
	rt_rq->tg = tg;

	tg->rt_rq[cpu] = rt_rq;
	tg->rt_se[cpu] = rt_se;

	if (!rt_se)
		return;

	if (!parent)
		rt_se->rt_rq = &rq->rt;
	else
		rt_se->rt_rq = parent->my_q;

	rt_se->my_q = rt_rq;
	rt_se->parent = parent;
	INIT_LIST_HEAD(&rt_se->run_list);
}

int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent)
{
	struct rt_rq *rt_rq;
	struct sched_rt_entity *rt_se;
	int i;

	tg->rt_rq = kzalloc(sizeof(rt_rq) * nr_cpu_ids, GFP_KERNEL);
	if (!tg->rt_rq)
		goto err;
	tg->rt_se = kzalloc(sizeof(rt_se) * nr_cpu_ids, GFP_KERNEL);
	if (!tg->rt_se)
		goto err;

	init_rt_bandwidth(&tg->rt_bandwidth,
			ktime_to_ns(def_rt_bandwidth.rt_period), 0);

	for_each_possible_cpu(i) {
		rt_rq = kzalloc_node(sizeof(struct rt_rq),
				     GFP_KERNEL, cpu_to_node(i));
		if (!rt_rq)
			goto err;

		rt_se = kzalloc_node(sizeof(struct sched_rt_entity),
				     GFP_KERNEL, cpu_to_node(i));
		if (!rt_se)
			goto err_free_rq;

		init_rt_rq(rt_rq);
		rt_rq->rt_runtime = tg->rt_bandwidth.rt_runtime;
		init_tg_rt_entry(tg, rt_rq, rt_se, i, parent->rt_se[i]);
	}

	return 1;

err_free_rq:
	kfree(rt_rq);
err:
	return 0;
}

#else /* CONFIG_RT_GROUP_SCHED */

#define rt_entity_is_task(rt_se) (1)

static inline struct task_struct *rt_task_of(struct sched_rt_entity *rt_se)
{
	return container_of(rt_se, struct task_struct, rt);
}

static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return container_of(rt_rq, struct rq, rt);
}

static inline struct rq *rq_of_rt_se(struct sched_rt_entity *rt_se)
{
	struct task_struct *p = rt_task_of(rt_se);

	return task_rq(p);
}

static inline struct rt_rq *rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	struct rq *rq = rq_of_rt_se(rt_se);

	return &rq->rt;
}

void free_rt_sched_group(struct task_group *tg) { }

int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}
#endif /* CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_SMP

#define entity_is_task(se)	(!se->my_q)
#define LOAD_AVG_MAX		47742
#define cap_scale(v, s) ((v)*(s) >> SCHED_CAPACITY_SHIFT)

u64 decay_load(u64 val, u64 n);
u32 accumulate_sum(u64 delta, int cpu, struct sched_avg *sa,
	       unsigned long weight, int running, struct cfs_rq *cfs_rq);

/*
 * We can represent the historical contribution to runnable average as the
 * coefficients of a geometric series.  To do this we sub-divide our runnable
 * history into segments of approximately 1ms (1024us); label the segment that
 * occurred N-ms ago p_N, with p_0 corresponding to the current period, e.g.
 *
 * [<- 1024us ->|<- 1024us ->|<- 1024us ->| ...
 *      p0            p1           p2
 *     (now)       (~1ms ago)  (~2ms ago)
 *
 * Let u_i denote the fraction of p_i that the entity was runnable.
 *
 * We then designate the fractions u_i as our co-efficients, yielding the
 * following representation of historical load:
 *   u_0 + u_1*y + u_2*y^2 + u_3*y^3 + ...
 *
 * We choose y based on the with of a reasonably scheduling period, fixing:
 *   y^32 = 0.5
 *
 * This means that the contribution to load ~32ms ago (u_32) will be weighted
 * approximately half as much as the contribution to load within the last ms
 * (u_0).
 *
 * When a period "rolls over" and we have new u_0`, multiplying the previous
 * sum again by y is sufficient to update:
 *   load_avg = u_0` + y*(u_0 + u_1*y + u_2*y^2 + ... )
 *            = u_0 + u_1*y + u_2*y^2 + ... [re-labeling u_i --> u_{i+1}]
 */
static __always_inline int
__update_load_avg(u64 now, int cpu, struct sched_avg *sa,
		  unsigned long weight, int running, struct cfs_rq *cfs_rq)
{
	u64 delta;

	delta = now - sa->last_update_time;
	/*
	 * This should only happen when time goes backwards, which it
	 * unfortunately does during sched clock init when we swap over to TSC.
	 */
	if ((s64)delta < 0) {
		sa->last_update_time = now;
		return 0;
	}

	/*
	 * Use 1024ns as the unit of measurement since it's a reasonable
	 * approximation of 1us and fast to compute.
	 */
	delta >>= 10;
	if (!delta)
		return 0;
	sa->last_update_time = now;

	/*
	 * Now we know we crossed measurement unit boundaries. The *_avg
	 * accrues by two steps:
	 *
	 * Step 1: accumulate *_sum since last_update_time. If we haven't
	 * crossed period boundaries, finish.
	 */
	if (!accumulate_sum(delta, cpu, sa, weight, running, cfs_rq))
		return 0;

/*
	 * Step 2: update *_avg.
	 */
	sa->load_avg = div_u64(sa->load_sum, LOAD_AVG_MAX);
	if (cfs_rq) {
		cfs_rq->runnable_load_avg =
			div_u64(cfs_rq->runnable_load_sum, LOAD_AVG_MAX);
	}

	sa->util_avg = sa->util_sum / LOAD_AVG_MAX;

	return 1;
}

void rt_rq_util_change(struct rt_rq *rt_rq)
{
	if (&this_rq()->rt == rt_rq)
		cpufreq_update_util(rt_rq->rq, SCHED_CPUFREQ_RT);
}

#ifdef CONFIG_RT_GROUP_SCHED
/* Take into account change of utilization of a child task group */
static inline void
update_tg_rt_util(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se)
{
	struct rt_rq *grt_rq = rt_se->my_q;
	long delta = grt_rq->avg.util_avg - rt_se->avg.util_avg;

	/* Nothing to update */
	if (!delta)
		return;

	/* Set new sched_rt_entity's utilization */
	rt_se->avg.util_avg = grt_rq->avg.util_avg;
	rt_se->avg.util_sum = rt_se->avg.util_avg * LOAD_AVG_MAX;

	/* Update parent rt_rq utilization */
	add_positive(&rt_rq->avg.util_avg, delta);
	rt_rq->avg.util_sum = rt_rq->avg.util_avg * LOAD_AVG_MAX;
}
static inline void
update_tg_rt_load(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se)
{
	struct rt_rq *grt_rq = rt_se->my_q;
	long delta = grt_rq->avg.load_avg - rt_se->avg.load_avg;

	/* Notthing to update */
	if (!delta)
		return;

	/* Set new sched_rt_entity's load */
	rt_se->avg.load_avg = grt_rq->avg.load_avg;
	rt_se->avg.load_sum = rt_se->avg.load_avg * LOAD_AVG_MAX;

	/* Update parent rt_rq load */
	add_positive(&rt_rq->avg.load_avg, delta);
	rt_rq->avg.load_sum = rt_rq->avg.load_avg * LOAD_AVG_MAX;
}

static inline int test_and_clear_tg_rt_propagate(struct sched_rt_entity *rt_se)
{
	struct rt_rq *grt_rq = rt_se->my_q;

	if (!grt_rq->propagate_avg)
		return 0;

	grt_rq->propagate_avg = 0;

	return 1;
}

static inline void propagate_rt_entity_load_avg(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	if (entity_is_task(rt_se))
		return;
	if (!test_and_clear_tg_rt_propagate(rt_se))
		return;

	rt_rq->propagate_avg = 1;

	update_tg_rt_util(rt_rq, rt_se);
	update_tg_rt_load(rt_rq, rt_se);
}
#else /* CONFIG_RT_GROUP_SCHED */
static inline void propagate_rt_entity_load_avg(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
}
#endif

int update_rt_rq_load_avg(u64 now, int cpu, struct rt_rq *rt_rq, bool update_freq)
{
	int decayed, removed_util = 0;
	struct sched_avg *sa = &rt_rq->avg;
	struct rq *rq = rt_rq->rq;

	if (atomic_long_read(&rt_rq->removed_util_avg)) {
		long r = atomic_long_xchg(&rt_rq->removed_util_avg, 0);
		sub_positive(&sa->util_avg, r);
		sub_positive(&sa->util_sum, r * LOAD_AVG_MAX);
		removed_util = 1;
#ifdef CONFIG_RT_GROUP_SCHED
		/* Set propagate_avg for task group load propagate */
		rt_rq->propagate_avg = 1;
#endif
	}

	if (atomic_long_read(&rt_rq->removed_load_avg)) {
		long r = atomic_long_xchg(&rt_rq->removed_load_avg, 0);
		sub_positive(&sa->load_avg, r);
		sub_positive(&sa->load_sum, r * LOAD_AVG_MAX);
#ifdef CONFIG_RT_GROUP_SCHED
		/* Set propagate_avg for task group load propagate */
		rt_rq->propagate_avg = 1;
#endif
	}

	decayed = __update_load_avg(now, cpu, sa, scale_load_down(NICE_0_LOAD),
			rt_rq->curr != NULL, NULL);

#ifndef CONFIG_64BIT
	smp_wmb();
	rt_rq->load_last_update_time_copy = sa->last_update_time;
#endif
	if (update_freq && (decayed || removed_util))
		rt_rq_util_change(rt_rq);

	if (rt_rq == &rq->rt)
		trace_sched_rt_load_avg_cpu(cpu_of(rq), rt_rq);

	return decayed;
}

void update_rt_load_avg(u64 now, struct sched_rt_entity *rt_se, struct rt_rq *rt_rq, int cpu)
{
	/*
	 * Track task load average for carrying it to new CPU after migrated.
	 */
	if (rt_se->avg.last_update_time)
		__update_load_avg(now, cpu, &rt_se->avg, scale_load_down(NICE_0_LOAD),
				rt_rq->curr == rt_se, NULL);

	update_rt_rq_load_avg(now, cpu, rt_rq, true);
	propagate_rt_entity_load_avg(rt_se, rt_rq);

	if (entity_is_task(rt_se))
		trace_sched_rt_load_avg_task(rt_task_of(rt_se), &rt_se->avg);
}

static void pull_rt_task(struct rq *this_rq);

static inline bool need_pull_rt_task(struct rq *rq, struct task_struct *prev)
{
	/* Try to pull RT tasks here if we lower this rq's prio */
	return rq->rt.highest_prio.curr > prev->prio;
}

static inline int rt_overloaded(struct rq *rq)
{
	return atomic_read(&rq->rd->rto_count);
}

static inline void rt_set_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	cpumask_set_cpu(rq->cpu, rq->rd->rto_mask);
	/*
	 * Make sure the mask is visible before we set
	 * the overload count. That is checked to determine
	 * if we should look at the mask. It would be a shame
	 * if we looked at the mask, but the mask was not
	 * updated yet.
	 *
	 * Matched by the barrier in pull_rt_task().
	 */
	smp_wmb();
	atomic_inc(&rq->rd->rto_count);
}

static inline void rt_clear_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	/* the order here really doesn't matter */
	atomic_dec(&rq->rd->rto_count);
	cpumask_clear_cpu(rq->cpu, rq->rd->rto_mask);
}

static void update_rt_migration(struct rt_rq *rt_rq)
{
	if (rt_rq->rt_nr_migratory && rt_rq->rt_nr_total > 1) {
		if (!rt_rq->overloaded) {
			rt_set_overload(rq_of_rt_rq(rt_rq));
			rt_rq->overloaded = 1;
		}
	} else if (rt_rq->overloaded) {
		rt_clear_overload(rq_of_rt_rq(rt_rq));
		rt_rq->overloaded = 0;
	}
}

static void inc_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	struct task_struct *p;

	if (!rt_entity_is_task(rt_se))
		return;

	p = rt_task_of(rt_se);
	rt_rq = &rq_of_rt_rq(rt_rq)->rt;

	rt_rq->rt_nr_total++;
	if (tsk_nr_cpus_allowed(p) > 1)
		rt_rq->rt_nr_migratory++;

	update_rt_migration(rt_rq);
}

static void dec_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	struct task_struct *p;

	if (!rt_entity_is_task(rt_se))
		return;

	p = rt_task_of(rt_se);
	rt_rq = &rq_of_rt_rq(rt_rq)->rt;

	rt_rq->rt_nr_total--;
	if (tsk_nr_cpus_allowed(p) > 1)
		rt_rq->rt_nr_migratory--;

	update_rt_migration(rt_rq);
}

static inline int has_pushable_tasks(struct rq *rq)
{
	return !plist_head_empty(&rq->rt.pushable_tasks);
}

static DEFINE_PER_CPU(struct callback_head, rt_push_head);
static DEFINE_PER_CPU(struct callback_head, rt_pull_head);

static void push_rt_tasks(struct rq *);
static void pull_rt_task(struct rq *);

static inline void queue_push_tasks(struct rq *rq)
{
	if (!has_pushable_tasks(rq))
		return;

	queue_balance_callback(rq, &per_cpu(rt_push_head, rq->cpu), push_rt_tasks);
}

static inline void queue_pull_task(struct rq *rq)
{
	queue_balance_callback(rq, &per_cpu(rt_pull_head, rq->cpu), pull_rt_task);
}

static void enqueue_pushable_task(struct rq *rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rq->rt.pushable_tasks);
	plist_node_init(&p->pushable_tasks, p->prio);
	plist_add(&p->pushable_tasks, &rq->rt.pushable_tasks);

	/* Update the highest prio pushable task */
	if (p->prio < rq->rt.highest_prio.next)
		rq->rt.highest_prio.next = p->prio;
}

static void dequeue_pushable_task(struct rq *rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rq->rt.pushable_tasks);

	/* Update the new highest prio pushable task */
	if (has_pushable_tasks(rq)) {
		p = plist_first_entry(&rq->rt.pushable_tasks,
				      struct task_struct, pushable_tasks);
		rq->rt.highest_prio.next = p->prio;
	} else
		rq->rt.highest_prio.next = MAX_RT_PRIO;
}

#else
static inline void rt_rq_util_change(struct rt_rq *rt_rq)
{
}

static inline
int update_rt_rq_load_avg(u64 now, int cpu, struct rt_rq *rt_rq, bool update_freq)
{
	return 0;
}
static inline
void update_rt_load_avg(u64 now, struct sched_rt_entity *rt_se, struct rt_rq *rt_rq, int cpu)
{
}

static inline void enqueue_pushable_task(struct rq *rq, struct task_struct *p)
{
}

static inline void dequeue_pushable_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void inc_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
}

static inline
void dec_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
}

static inline bool need_pull_rt_task(struct rq *rq, struct task_struct *prev)
{
	return false;
}

static inline void pull_rt_task(struct rq *this_rq)
{
}

static inline void queue_push_tasks(struct rq *rq)
{
}
#endif /* CONFIG_SMP */

static void enqueue_top_rt_rq(struct rt_rq *rt_rq);
static void dequeue_top_rt_rq(struct rt_rq *rt_rq);

static inline int on_rt_rq(struct sched_rt_entity *rt_se)
{
	return rt_se->on_rq;
}

#ifdef CONFIG_RT_GROUP_SCHED

static inline u64 sched_rt_runtime(struct rt_rq *rt_rq)
{
	if (!rt_rq->tg)
		return RUNTIME_INF;

	return rt_rq->rt_runtime;
}

static inline u64 sched_rt_period(struct rt_rq *rt_rq)
{
	return ktime_to_ns(rt_rq->tg->rt_bandwidth.rt_period);
}

typedef struct task_group *rt_rq_iter_t;

static inline struct task_group *next_task_group(struct task_group *tg)
{
	do {
		tg = list_entry_rcu(tg->list.next,
			typeof(struct task_group), list);
	} while (&tg->list != &task_groups && task_group_is_autogroup(tg));

	if (&tg->list == &task_groups)
		tg = NULL;

	return tg;
}

#define for_each_rt_rq(rt_rq, iter, rq)					\
	for (iter = container_of(&task_groups, typeof(*iter), list);	\
		(iter = next_task_group(iter)) &&			\
		(rt_rq = iter->rt_rq[cpu_of(rq)]);)

#define for_each_sched_rt_entity(rt_se) \
	for (; rt_se; rt_se = rt_se->parent)

static inline struct rt_rq *group_rt_rq(struct sched_rt_entity *rt_se)
{
	return rt_se->my_q;
}

static void enqueue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags);
static void dequeue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags);

static void sched_rt_rq_enqueue(struct rt_rq *rt_rq)
{
	struct task_struct *curr = rq_of_rt_rq(rt_rq)->curr;
	struct rq *rq = rq_of_rt_rq(rt_rq);
	struct sched_rt_entity *rt_se;

	int cpu = cpu_of(rq);

	rt_se = rt_rq->tg->rt_se[cpu];

	if (rt_rq->rt_nr_running) {
		if (!rt_se)
			enqueue_top_rt_rq(rt_rq);
		else if (!on_rt_rq(rt_se))
			enqueue_rt_entity(rt_se, 0);

		if (rt_rq->highest_prio.curr < curr->prio)
			resched_curr(rq);
	}
}

static void sched_rt_rq_dequeue(struct rt_rq *rt_rq)
{
	struct sched_rt_entity *rt_se;
	int cpu = cpu_of(rq_of_rt_rq(rt_rq));

	rt_se = rt_rq->tg->rt_se[cpu];

	if (!rt_se)
		dequeue_top_rt_rq(rt_rq);
	else if (on_rt_rq(rt_se))
		dequeue_rt_entity(rt_se, 0);
}

static inline int rt_rq_throttled(struct rt_rq *rt_rq)
{
	return rt_rq->rt_throttled && !rt_rq->rt_nr_boosted;
}

static int rt_se_boosted(struct sched_rt_entity *rt_se)
{
	struct rt_rq *rt_rq = group_rt_rq(rt_se);
	struct task_struct *p;

	if (rt_rq)
		return !!rt_rq->rt_nr_boosted;

	p = rt_task_of(rt_se);
	return p->prio != p->normal_prio;
}

#ifdef CONFIG_SMP
static inline const struct cpumask *sched_rt_period_mask(void)
{
	return this_rq()->rd->span;
}
#else
static inline const struct cpumask *sched_rt_period_mask(void)
{
	return cpu_online_mask;
}
#endif

static inline
struct rt_rq *sched_rt_period_rt_rq(struct rt_bandwidth *rt_b, int cpu)
{
	return container_of(rt_b, struct task_group, rt_bandwidth)->rt_rq[cpu];
}

static inline struct rt_bandwidth *sched_rt_bandwidth(struct rt_rq *rt_rq)
{
	return &rt_rq->tg->rt_bandwidth;
}

#else /* !CONFIG_RT_GROUP_SCHED */

static inline u64 sched_rt_runtime(struct rt_rq *rt_rq)
{
	return rt_rq->rt_runtime;
}

static inline u64 sched_rt_period(struct rt_rq *rt_rq)
{
	return ktime_to_ns(def_rt_bandwidth.rt_period);
}

typedef struct rt_rq *rt_rq_iter_t;

#define for_each_rt_rq(rt_rq, iter, rq) \
	for ((void) iter, rt_rq = &rq->rt; rt_rq; rt_rq = NULL)

#define for_each_sched_rt_entity(rt_se) \
	for (; rt_se; rt_se = NULL)

static inline struct rt_rq *group_rt_rq(struct sched_rt_entity *rt_se)
{
	return NULL;
}

static inline void sched_rt_rq_enqueue(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (!rt_rq->rt_nr_running)
		return;

	enqueue_top_rt_rq(rt_rq);
	resched_curr(rq);
}

static inline void sched_rt_rq_dequeue(struct rt_rq *rt_rq)
{
	dequeue_top_rt_rq(rt_rq);
}

static inline int rt_rq_throttled(struct rt_rq *rt_rq)
{
	return rt_rq->rt_throttled;
}

static inline const struct cpumask *sched_rt_period_mask(void)
{
	return cpu_online_mask;
}

static inline
struct rt_rq *sched_rt_period_rt_rq(struct rt_bandwidth *rt_b, int cpu)
{
	return &cpu_rq(cpu)->rt;
}

static inline struct rt_bandwidth *sched_rt_bandwidth(struct rt_rq *rt_rq)
{
	return &def_rt_bandwidth;
}

#endif /* CONFIG_RT_GROUP_SCHED */

bool sched_rt_bandwidth_account(struct rt_rq *rt_rq)
{
	struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);

	return (hrtimer_active(&rt_b->rt_period_timer) ||
		rt_rq->rt_time < rt_b->rt_runtime);
}

#ifdef CONFIG_SMP
/*
 * We ran out of runtime, see if we can borrow some from our neighbours.
 */
static void do_balance_runtime(struct rt_rq *rt_rq)
{
	struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
	struct root_domain *rd = rq_of_rt_rq(rt_rq)->rd;
	int i, weight;
	u64 rt_period;

	weight = cpumask_weight(rd->span);

	raw_spin_lock(&rt_b->rt_runtime_lock);
	rt_period = ktime_to_ns(rt_b->rt_period);
	for_each_cpu(i, rd->span) {
		struct rt_rq *iter = sched_rt_period_rt_rq(rt_b, i);
		s64 diff;

		if (iter == rt_rq)
			continue;

		raw_spin_lock(&iter->rt_runtime_lock);
		/*
		 * Either all rqs have inf runtime and there's nothing to steal
		 * or __disable_runtime() below sets a specific rq to inf to
		 * indicate its been disabled and disalow stealing.
		 */
		if (iter->rt_runtime == RUNTIME_INF)
			goto next;

		/*
		 * From runqueues with spare time, take 1/n part of their
		 * spare time, but no more than our period.
		 */
		diff = iter->rt_runtime - iter->rt_time;
		if (diff > 0) {
			diff = div_u64((u64)diff, weight);
			if (rt_rq->rt_runtime + diff > rt_period)
				diff = rt_period - rt_rq->rt_runtime;
			iter->rt_runtime -= diff;
			rt_rq->rt_runtime += diff;
			if (rt_rq->rt_runtime == rt_period) {
				raw_spin_unlock(&iter->rt_runtime_lock);
				break;
			}
		}
next:
		raw_spin_unlock(&iter->rt_runtime_lock);
	}
	raw_spin_unlock(&rt_b->rt_runtime_lock);
}

/*
 * Ensure this RQ takes back all the runtime it lend to its neighbours.
 */
static void __disable_runtime(struct rq *rq)
{
	struct root_domain *rd = rq->rd;
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	if (unlikely(!scheduler_running))
		return;

	for_each_rt_rq(rt_rq, iter, rq) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
		s64 want;
		int i;

		raw_spin_lock(&rt_b->rt_runtime_lock);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * Either we're all inf and nobody needs to borrow, or we're
		 * already disabled and thus have nothing to do, or we have
		 * exactly the right amount of runtime to take out.
		 */
		if (rt_rq->rt_runtime == RUNTIME_INF ||
				rt_rq->rt_runtime == rt_b->rt_runtime)
			goto balanced;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);

		/*
		 * Calculate the difference between what we started out with
		 * and what we current have, that's the amount of runtime
		 * we lend and now have to reclaim.
		 */
		want = rt_b->rt_runtime - rt_rq->rt_runtime;

		/*
		 * Greedy reclaim, take back as much as we can.
		 */
		for_each_cpu(i, rd->span) {
			struct rt_rq *iter = sched_rt_period_rt_rq(rt_b, i);
			s64 diff;

			/*
			 * Can't reclaim from ourselves or disabled runqueues.
			 */
			if (iter == rt_rq || iter->rt_runtime == RUNTIME_INF)
				continue;

			raw_spin_lock(&iter->rt_runtime_lock);
			if (want > 0) {
				diff = min_t(s64, iter->rt_runtime, want);
				iter->rt_runtime -= diff;
				want -= diff;
			} else {
				iter->rt_runtime -= want;
				want -= want;
			}
			raw_spin_unlock(&iter->rt_runtime_lock);

			if (!want)
				break;
		}

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * We cannot be left wanting - that would mean some runtime
		 * leaked out of the system.
		 */
		BUG_ON(want);
balanced:
		/*
		 * Disable all the borrow logic by pretending we have inf
		 * runtime - in which case borrowing doesn't make sense.
		 */
		rt_rq->rt_runtime = RUNTIME_INF;
		rt_rq->rt_throttled = 0;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		raw_spin_unlock(&rt_b->rt_runtime_lock);

		/* Make rt_rq available for pick_next_task() */
		sched_rt_rq_enqueue(rt_rq);
	}
}

static void __enable_runtime(struct rq *rq)
{
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	if (unlikely(!scheduler_running))
		return;

	/*
	 * Reset each runqueue's bandwidth settings
	 */
	for_each_rt_rq(rt_rq, iter, rq) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);

		raw_spin_lock(&rt_b->rt_runtime_lock);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		rt_rq->rt_runtime = rt_b->rt_runtime;
		rt_rq->rt_time = 0;
		rt_rq->rt_throttled = 0;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		raw_spin_unlock(&rt_b->rt_runtime_lock);
	}
}

static void balance_runtime(struct rt_rq *rt_rq)
{
	if (!sched_feat(RT_RUNTIME_SHARE))
		return;

	if (rt_rq->rt_time > rt_rq->rt_runtime) {
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		do_balance_runtime(rt_rq);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
	}
}
#else /* !CONFIG_SMP */
static inline void balance_runtime(struct rt_rq *rt_rq) {}
#endif /* CONFIG_SMP */

static int do_sched_rt_period_timer(struct rt_bandwidth *rt_b, int overrun)
{
	int i, idle = 1, throttled = 0;
	const struct cpumask *span;

	span = sched_rt_period_mask();
#ifdef CONFIG_RT_GROUP_SCHED
	/*
	 * FIXME: isolated CPUs should really leave the root task group,
	 * whether they are isolcpus or were isolated via cpusets, lest
	 * the timer run on a CPU which does not service all runqueues,
	 * potentially leaving other CPUs indefinitely throttled.  If
	 * isolation is really required, the user will turn the throttle
	 * off to kill the perturbations it causes anyway.  Meanwhile,
	 * this maintains functionality for boot and/or troubleshooting.
	 */
	if (rt_b == &root_task_group.rt_bandwidth)
		span = cpu_online_mask;
#endif
	for_each_cpu(i, span) {
		int enqueue = 0;
		struct rt_rq *rt_rq = sched_rt_period_rt_rq(rt_b, i);
		struct rq *rq = rq_of_rt_rq(rt_rq);

		raw_spin_lock(&rq->lock);
		update_rq_clock(rq);

		if (rt_rq->rt_time) {
			u64 runtime;

			raw_spin_lock(&rt_rq->rt_runtime_lock);
			if (rt_rq->rt_throttled)
				balance_runtime(rt_rq);
			runtime = rt_rq->rt_runtime;
			rt_rq->rt_time -= min(rt_rq->rt_time, overrun*runtime);
			if (rt_rq->rt_throttled && rt_rq->rt_time < runtime) {
				rt_rq->rt_throttled = 0;
				enqueue = 1;

				/*
				 * When we're idle and a woken (rt) task is
				 * throttled check_preempt_curr() will set
				 * skip_update and the time between the wakeup
				 * and this unthrottle will get accounted as
				 * 'runtime'.
				 */
				if (rt_rq->rt_nr_running && rq->curr == rq->idle)
					rq_clock_skip_update(rq, false);
			}
			if (rt_rq->rt_time || rt_rq->rt_nr_running)
				idle = 0;
			raw_spin_unlock(&rt_rq->rt_runtime_lock);
		} else if (rt_rq->rt_nr_running) {
			idle = 0;
			if (!rt_rq_throttled(rt_rq))
				enqueue = 1;
		}
		if (rt_rq->rt_throttled)
			throttled = 1;

		if (enqueue)
			sched_rt_rq_enqueue(rt_rq);
		raw_spin_unlock(&rq->lock);
	}

	if (!throttled && (!rt_bandwidth_enabled() || rt_b->rt_runtime == RUNTIME_INF))
		return 1;

	return idle;
}

static inline int rt_se_prio(struct sched_rt_entity *rt_se)
{
#ifdef CONFIG_RT_GROUP_SCHED
	struct rt_rq *rt_rq = group_rt_rq(rt_se);

	if (rt_rq)
		return rt_rq->highest_prio.curr;
#endif

	return rt_task_of(rt_se)->prio;
}

static void dump_throttled_rt_tasks(struct rt_rq *rt_rq)
{
	struct rt_prio_array *array = &rt_rq->active;
	struct sched_rt_entity *rt_se;
	char buf[500];
	char *pos = buf;
	char *end = buf + sizeof(buf);
	int idx;

	pos += snprintf(pos, sizeof(buf),
		"sched: RT throttling activated for rt_rq %p (cpu %d)\n",
		rt_rq, cpu_of(rq_of_rt_rq(rt_rq)));

	if (bitmap_empty(array->bitmap, MAX_RT_PRIO))
		goto out;

	pos += snprintf(pos, end - pos, "potential CPU hogs:\n");
	idx = sched_find_first_bit(array->bitmap);
	while (idx < MAX_RT_PRIO) {
		list_for_each_entry(rt_se, array->queue + idx, run_list) {
			struct task_struct *p;

			if (!rt_entity_is_task(rt_se))
				continue;

			p = rt_task_of(rt_se);
			if (pos < end)
				pos += snprintf(pos, end - pos, "\t%s (%d)\n",
					p->comm, p->pid);
		}
		idx = find_next_bit(array->bitmap, MAX_RT_PRIO, idx + 1);
	}
out:
#ifdef CONFIG_PANIC_ON_RT_THROTTLING
	/*
	 * Use pr_err() in the BUG() case since printk_sched() will
	 * not get flushed and deadlock is not a concern.
	 */
	pr_err("%s", buf);
	BUG();
#else
	printk_deferred("%s", buf);
#endif
}

static int sched_rt_runtime_exceeded(struct rt_rq *rt_rq)
{
	u64 runtime = sched_rt_runtime(rt_rq);

	if (rt_rq->rt_throttled)
		return rt_rq_throttled(rt_rq);

	if (runtime >= sched_rt_period(rt_rq))
		return 0;

	balance_runtime(rt_rq);
	runtime = sched_rt_runtime(rt_rq);
	if (runtime == RUNTIME_INF)
		return 0;

	if (rt_rq->rt_time > runtime) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);

		/*
		 * Don't actually throttle groups that have no runtime assigned
		 * but accrue some time due to boosting.
		 */
		if (likely(rt_b->rt_runtime)) {
			static bool once = false;

			rt_rq->rt_throttled = 1;

			if (!once) {
				once = true;
				dump_throttled_rt_tasks(rt_rq);
			}
		} else {
			/*
			 * In case we did anyway, make it go away,
			 * replenishment is a joke, since it will replenish us
			 * with exactly 0 ns.
			 */
			rt_rq->rt_time = 0;
		}

		if (rt_rq_throttled(rt_rq)) {
			sched_rt_rq_dequeue(rt_rq);
			return 1;
		}
	}

	return 0;
}

/*
 * Update the current task's runtime statistics. Skip current tasks that
 * are not in our scheduling class.
 */
static void update_curr_rt(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct sched_rt_entity *rt_se = &curr->rt;
	u64 delta_exec;

	if (curr->sched_class != &rt_sched_class)
		return;

	delta_exec = rq_clock_task(rq) - curr->se.exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;

	/* Kick cpufreq (see the comment in kernel/sched/sched.h). */
	cpufreq_update_this_cpu(rq, SCHED_CPUFREQ_RT);

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = rq_clock_task(rq);
	cpuacct_charge(curr, delta_exec);

	sched_rt_avg_update(rq, delta_exec);

	if (!rt_bandwidth_enabled())
		return;

	for_each_sched_rt_entity(rt_se) {
		struct rt_rq *rt_rq = rt_rq_of_se(rt_se);

		if (sched_rt_runtime(rt_rq) != RUNTIME_INF) {
			raw_spin_lock(&rt_rq->rt_runtime_lock);
			rt_rq->rt_time += delta_exec;
			if (sched_rt_runtime_exceeded(rt_rq))
				resched_curr(rq);
			raw_spin_unlock(&rt_rq->rt_runtime_lock);
		}
	}
}

static void
dequeue_top_rt_rq(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	BUG_ON(&rq->rt != rt_rq);

	if (!rt_rq->rt_queued)
		return;

	BUG_ON(!rq->nr_running);

	sub_nr_running(rq, rt_rq->rt_nr_running);
	rt_rq->rt_queued = 0;
}

static void
enqueue_top_rt_rq(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	BUG_ON(&rq->rt != rt_rq);

	if (rt_rq->rt_queued)
		return;
	if (rt_rq_throttled(rt_rq) || !rt_rq->rt_nr_running)
		return;

	add_nr_running(rq, rt_rq->rt_nr_running);
	rt_rq->rt_queued = 1;
}

#if defined CONFIG_SMP

static void
inc_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

#ifdef CONFIG_RT_GROUP_SCHED
	/*
	 * Change rq's cpupri only if rt_rq is the top queue.
	 */
	if (&rq->rt != rt_rq)
		return;
#endif
	if (rq->online && prio < prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, prio);
}

static void
dec_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

#ifdef CONFIG_RT_GROUP_SCHED
	/*
	 * Change rq's cpupri only if rt_rq is the top queue.
	 */
	if (&rq->rt != rt_rq)
		return;
#endif
	if (rq->online && rt_rq->highest_prio.curr != prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, rt_rq->highest_prio.curr);
}

#else /* CONFIG_SMP */

static inline
void inc_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio) {}
static inline
void dec_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio) {}

#endif /* CONFIG_SMP */

#if defined CONFIG_SMP || defined CONFIG_RT_GROUP_SCHED
static void
inc_rt_prio(struct rt_rq *rt_rq, int prio)
{
	int prev_prio = rt_rq->highest_prio.curr;

	if (prio < prev_prio)
		rt_rq->highest_prio.curr = prio;

	inc_rt_prio_smp(rt_rq, prio, prev_prio);
}

static void
dec_rt_prio(struct rt_rq *rt_rq, int prio)
{
	int prev_prio = rt_rq->highest_prio.curr;

	if (rt_rq->rt_nr_running) {

		WARN_ON(prio < prev_prio);

		/*
		 * This may have been our highest task, and therefore
		 * we may have some recomputation to do
		 */
		if (prio == prev_prio) {
			struct rt_prio_array *array = &rt_rq->active;

			rt_rq->highest_prio.curr =
				sched_find_first_bit(array->bitmap);
		}

	} else
		rt_rq->highest_prio.curr = MAX_RT_PRIO;

	dec_rt_prio_smp(rt_rq, prio, prev_prio);
}

#else

static inline void inc_rt_prio(struct rt_rq *rt_rq, int prio) {}
static inline void dec_rt_prio(struct rt_rq *rt_rq, int prio) {}

#endif /* CONFIG_SMP || CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_RT_GROUP_SCHED

static void
inc_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	if (rt_se_boosted(rt_se))
		rt_rq->rt_nr_boosted++;

	if (rt_rq->tg)
		start_rt_bandwidth(&rt_rq->tg->rt_bandwidth);
}

static void
dec_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	if (rt_se_boosted(rt_se))
		rt_rq->rt_nr_boosted--;

	WARN_ON(!rt_rq->rt_nr_running && rt_rq->rt_nr_boosted);
}

#else /* CONFIG_RT_GROUP_SCHED */

static void
inc_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	start_rt_bandwidth(&def_rt_bandwidth);
}

static inline
void dec_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq) {}

#endif /* CONFIG_RT_GROUP_SCHED */

static inline
unsigned int rt_se_nr_running(struct sched_rt_entity *rt_se)
{
	struct rt_rq *group_rq = group_rt_rq(rt_se);

	if (group_rq)
		return group_rq->rt_nr_running;
	else
		return 1;
}

static inline
unsigned int rt_se_rr_nr_running(struct sched_rt_entity *rt_se)
{
	struct rt_rq *group_rq = group_rt_rq(rt_se);
	struct task_struct *tsk;

	if (group_rq)
		return group_rq->rr_nr_running;

	tsk = rt_task_of(rt_se);

	return (tsk->policy == SCHED_RR) ? 1 : 0;
}

static inline
void inc_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	int prio = rt_se_prio(rt_se);

	WARN_ON(!rt_prio(prio));
	rt_rq->rt_nr_running += rt_se_nr_running(rt_se);
	rt_rq->rr_nr_running += rt_se_rr_nr_running(rt_se);

	inc_rt_prio(rt_rq, prio);
	inc_rt_migration(rt_se, rt_rq);
	inc_rt_group(rt_se, rt_rq);
}

static inline
void dec_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	WARN_ON(!rt_prio(rt_se_prio(rt_se)));
	WARN_ON(!rt_rq->rt_nr_running);
	rt_rq->rt_nr_running -= rt_se_nr_running(rt_se);
	rt_rq->rr_nr_running -= rt_se_rr_nr_running(rt_se);

	dec_rt_prio(rt_rq, rt_se_prio(rt_se));
	dec_rt_migration(rt_se, rt_rq);
	dec_rt_group(rt_se, rt_rq);
}
#ifdef CONFIG_SMP
/**
 * attach_rt_entity_load_avg - attach this entity to its rt_rq load avg
 * @rt_rq: rt_rq to attach to
 * @rt_se: sched_rt_entity to attach
 *
 * Must call update_rt_rq_load_avg() before this, since we rely on
 * rt_rq->avg.last_update_time being current.
 *
 * load_{avg,sum} are not used by RT
 */
static void
attach_entity_load_avg(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se)
{
	rt_se->avg.last_update_time = rt_rq->avg.last_update_time;
	rt_rq->avg.util_avg += rt_se->avg.util_avg;
	rt_rq->avg.util_sum += rt_se->avg.util_sum;
	rt_rq->avg.load_avg += rt_se->avg.load_avg;
	rt_rq->avg.load_sum += rt_se->avg.load_sum;
#ifdef CONFIG_RT_GROUP_SCHED
	/* Set propagate_avg for task group load propagate */
	rt_rq->propagate_avg = 1;
#endif
	rt_rq_util_change(rt_rq);
}

/*
 * detach_entity_load_avg - detach this entity from its rt_rq load avg
 * @rt_rq: rt_rq to detach from
 * @rt_se: sched_rt_entity to detach
 *
 * Must call update_rt_rq_load_avg() before this, since we rely on
 * rt_rq->avg.last_update_time being current.
 *
 * load_{avg,sum} are not used by RT
 */
static void detach_entity_load_avg(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se)
{
	sub_positive(&rt_rq->avg.util_avg, rt_se->avg.util_avg);
	sub_positive(&rt_rq->avg.util_sum, rt_se->avg.util_sum);
	sub_positive(&rt_rq->avg.load_avg, rt_se->avg.load_avg);
	sub_positive(&rt_rq->avg.load_sum, rt_se->avg.load_sum);
#ifdef CONFIG_RT_GROUP_SCHED
	/* Set propagate_avg for task group load propagate */
	rt_rq->propagate_avg = 1;
#endif
	rt_rq_util_change(rt_rq);
}
#else
static inline void
attach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se) {}
static inline void
detach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se) {}
#endif
/*
 * Change rt_se->run_list location unless SAVE && !MOVE
 *
 * assumes ENQUEUE/DEQUEUE flags match
 */
static inline bool move_entity(unsigned int flags)
{
	if ((flags & (DEQUEUE_SAVE | DEQUEUE_MOVE)) == DEQUEUE_SAVE)
		return false;

	return true;
}

static void __delist_rt_entity(struct sched_rt_entity *rt_se, struct rt_prio_array *array)
{
	list_del_init(&rt_se->run_list);

	if (list_empty(array->queue + rt_se_prio(rt_se)))
		__clear_bit(rt_se_prio(rt_se), array->bitmap);

	rt_se->on_list = 0;
}

static void __enqueue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;
	struct rt_rq *group_rq = group_rt_rq(rt_se);
	struct list_head *queue = array->queue + rt_se_prio(rt_se);
	u64 now = rq_clock_task(rq_of_rt_rq(rt_rq));

	/*
	 * Don't enqueue the group if its throttled, or when empty.
	 * The latter is a consequence of the former when a child group
	 * get throttled and the current group doesn't have any other
	 * active members.
	 */
	if (group_rq && (rt_rq_throttled(group_rq) || !group_rq->rt_nr_running)) {
		if (rt_se->on_list)
			__delist_rt_entity(rt_se, array);
		return;
	}

	if (move_entity(flags)) {
		WARN_ON_ONCE(rt_se->on_list);
		if (flags & ENQUEUE_HEAD)
			list_add(&rt_se->run_list, queue);
		else
			list_add_tail(&rt_se->run_list, queue);

		__set_bit(rt_se_prio(rt_se), array->bitmap);
		rt_se->on_list = 1;
	}
	rt_se->on_rq = 1;

	update_rt_load_avg(now, rt_se, rt_rq, cpu_of(rq_of_rt_rq(rt_rq)));

	if (rt_entity_is_task(rt_se) && !rt_se->avg.last_update_time)
		attach_entity_load_avg(&rq_of_rt_se(rt_se)->rt, rt_se);

	inc_rt_tasks(rt_se, rt_rq);
}

static void __dequeue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;
	u64 now = rq_clock_task(rq_of_rt_rq(rt_rq));

	if (move_entity(flags)) {
		WARN_ON_ONCE(!rt_se->on_list);
		__delist_rt_entity(rt_se, array);
	}
	rt_se->on_rq = 0;

	update_rt_load_avg(now, rt_se, rt_rq, cpu_of(rq_of_rt_rq(rt_rq)));

	dec_rt_tasks(rt_se, rt_rq);
}

/*
 * Because the prio of an upper entry depends on the lower
 * entries, we must remove entries top - down.
 */
static void dequeue_rt_stack(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct sched_rt_entity *back = NULL;

	for_each_sched_rt_entity(rt_se) {
		rt_se->back = back;
		back = rt_se;
	}

	dequeue_top_rt_rq(rt_rq_of_se(back));

	for (rt_se = back; rt_se; rt_se = rt_se->back) {
		if (on_rt_rq(rt_se))
			__dequeue_rt_entity(rt_se, flags);
	}
}

static void enqueue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rq *rq = rq_of_rt_se(rt_se);

	dequeue_rt_stack(rt_se, flags);
	for_each_sched_rt_entity(rt_se)
		__enqueue_rt_entity(rt_se, flags);
	enqueue_top_rt_rq(&rq->rt);
}

static void dequeue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rq *rq = rq_of_rt_se(rt_se);

	dequeue_rt_stack(rt_se, flags);

	for_each_sched_rt_entity(rt_se) {
		struct rt_rq *rt_rq = group_rt_rq(rt_se);

		if (rt_rq && rt_rq->rt_nr_running)
			__enqueue_rt_entity(rt_se, flags);
	}
	enqueue_top_rt_rq(&rq->rt);
}

/*
 * Adding/removing a task to/from a priority array:
 */
static void
enqueue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;

	if (flags & ENQUEUE_WAKEUP)
		rt_se->timeout = 0;

	update_rt_rq_load_avg(rq_clock_task(rq), cpu_of(rq), &rq->rt, 0);

	enqueue_rt_entity(rt_se, flags);
	walt_inc_cumulative_runnable_avg(rq, p);

	if (!task_current(rq, p) && tsk_nr_cpus_allowed(p) > 1)
		enqueue_pushable_task(rq, p);
}

static void dequeue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;

	update_curr_rt(rq);
	dequeue_rt_entity(rt_se, flags);
	walt_dec_cumulative_runnable_avg(rq, p);

	dequeue_pushable_task(rq, p);
}

/*
 * Put task to the head or the end of the run list without the overhead of
 * dequeue followed by enqueue.
 */
static void
requeue_rt_entity(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se, int head)
{
	if (on_rt_rq(rt_se)) {
		struct rt_prio_array *array = &rt_rq->active;
		struct list_head *queue = array->queue + rt_se_prio(rt_se);

		if (head)
			list_move(&rt_se->run_list, queue);
		else
			list_move_tail(&rt_se->run_list, queue);
	}
}

static void requeue_task_rt(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq;

	for_each_sched_rt_entity(rt_se) {
		rt_rq = rt_rq_of_se(rt_se);
		requeue_rt_entity(rt_rq, rt_se, head);
	}
}

static void yield_task_rt(struct rq *rq)
{
	requeue_task_rt(rq, rq->curr, 0);
}

#ifdef CONFIG_SMP
static int find_lowest_rq(struct task_struct *task);

#ifdef CONFIG_SCHED_USE_FLUID_RT
static int
select_task_rq_rt_fluid(struct task_struct *p, int cpu, int sd_flag, int flags)
{
	int target;

	rcu_read_lock();
	target = find_lowest_rq(p);
	if (target != -1)
		cpu = target;
	rcu_read_unlock();

	return cpu;
}

static inline void set_victim_flag(struct task_struct *p)
{
	p->victim_flag = 1;
}

static inline void clear_victim_flag(struct task_struct *p)
{
	p->victim_flag = 0;
}

static inline bool test_victim_flag(struct task_struct *p)
{
	if (p->victim_flag)
		return true;
	else
		return false;
}
#else
static int
select_task_rq_rt(struct task_struct *p, int cpu, int sd_flag, int flags)
{
	struct task_struct *curr;
	struct rq *rq;

	/* For anything but wake ups, just return the task_cpu */
	if (sd_flag != SD_BALANCE_WAKE && sd_flag != SD_BALANCE_FORK)
		goto out;

	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = READ_ONCE(rq->curr); /* unlocked access */

	/*
	 * If the current task on @p's runqueue is an RT task, then
	 * try to see if we can wake this RT task up on another
	 * runqueue. Otherwise simply start this RT task
	 * on its current runqueue.
	 *
	 * We want to avoid overloading runqueues. If the woken
	 * task is a higher priority, then it will stay on this CPU
	 * and the lower prio task should be moved to another CPU.
	 * Even though this will probably make the lower prio task
	 * lose its cache, we do not want to bounce a higher task
	 * around just because it gave up its CPU, perhaps for a
	 * lock?
	 *
	 * For equal prio tasks, we just let the scheduler sort it out.
	 *
	 * Otherwise, just let it ride on the affined RQ and the
	 * post-schedule router will push the preempted task away
	 *
	 * This test is optimistic, if we get it wrong the load-balancer
	 * will have to sort it out.
	 */
	if (curr && unlikely(rt_task(curr)) &&
	    (tsk_nr_cpus_allowed(curr) < 2 ||
	     curr->prio <= p->prio)) {
		int target = find_lowest_rq(p);

		/*
		 * Don't bother moving it if the destination CPU is
		 * not running a lower priority task.
		 */
		if (target != -1 &&
		    p->prio < cpu_rq(target)->rt.highest_prio.curr)
			cpu = target;
	}
	rcu_read_unlock();

out:
	return cpu;
}
static inline bool test_victim_flag(struct task_struct *p)
{
	return false;
}
static inline void clear_victim_flag(struct task_struct *p)
{
}
#endif

#ifdef CONFIG_RT_GROUP_SCHED
/*
 * Called within set_task_rq() right before setting a task's cpu. The
 * caller only guarantees p->pi_lock is held; no other assumptions,
 * including the state of rq->lock, should be made.
 */
void set_task_rq_rt(struct sched_rt_entity *rt_se,
				    struct rt_rq *prev, struct rt_rq *next)
{
	if (!sched_feat(ATTACH_AGE_LOAD))
		return;
	/*
	 * We are supposed to update the task to "current" time, then its up to
	 * date and ready to go to new CPU/cfs_rq. But we have difficulty in
	 * getting what current time is, so simply throw away the out-of-date
	 * time. This will result in the wakee task is less decayed, but giving
	 * the wakee more load sounds not bad.
	 */
	if (rt_se->avg.last_update_time && prev) {
		u64 p_last_update_time;
		u64 n_last_update_time;

#ifndef CONFIG_64BIT
		u64 p_last_update_time_copy;
		u64 n_last_update_time_copy;

		do {
			p_last_update_time_copy = prev->load_last_update_time_copy;
			n_last_update_time_copy = next->load_last_update_time_copy;

			smp_rmb();

			p_last_update_time = prev->avg.last_update_time;
			n_last_update_time = next->avg.last_update_time;

		} while (p_last_update_time != p_last_update_time_copy ||
			 n_last_update_time != n_last_update_time_copy);
#else
		p_last_update_time = prev->avg.last_update_time;
		n_last_update_time = next->avg.last_update_time;
#endif
		__update_load_avg(p_last_update_time, cpu_of(rq_of_rt_rq(prev)),
				&rt_se->avg, 0, 0, NULL);

		rt_se->avg.last_update_time = n_last_update_time;
	}
}
#endif /* CONFIG_RT_GROUP_SCHED */

#ifndef CONFIG_64BIT
static inline u64 rt_rq_last_update_time(struct rt_rq *rt_rq)
{
	u64 last_update_time_copy;
	u64 last_update_time;

	do {
		last_update_time_copy = rt_rq->load_last_update_time_copy;
		smp_rmb();
		last_update_time = rt_rq->avg.last_update_time;
	} while (last_update_time != last_update_time_copy);

	return last_update_time;
}
#else
static inline u64 rt_rq_last_update_time(struct rt_rq *rt_rq)
{
	return rt_rq->avg.last_update_time;
}
#endif

/*
 * Synchronize entity load avg of dequeued entity without locking
 * the previous rq.
 */
static void sync_entity_load_avg(struct sched_rt_entity *rt_se)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	u64 last_update_time;

	last_update_time = rt_rq_last_update_time(rt_rq);
	__update_load_avg(last_update_time, cpu_of(rq_of_rt_rq(rt_rq)),
				&rt_se->avg, 0, 0, NULL);
}

/*
 * Task first catches up with rt_rq, and then subtract
 * itself from the rt_rq (task must be off the queue now).
 */
static void remove_entity_load_avg(struct sched_rt_entity *rt_se)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);

	/*
	 * tasks cannot exit without having gone through wake_up_new_task() ->
	 * post_init_entity_util_avg() which will have added things to the
	 * rt_rq, so we can remove unconditionally.
	 *
	 * Similarly for groups, they will have passed through
	 * post_init_entity_util_avg() before unregister_sched_fair_group()
	 * calls this.
	 */

	sync_entity_load_avg(rt_se);
	atomic_long_add(rt_se->avg.load_avg, &rt_rq->removed_load_avg);
	atomic_long_add(rt_se->avg.util_avg, &rt_rq->removed_util_avg);
}

static void attach_task_rt_rq(struct task_struct *p)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	u64 now = rq_clock_task(rq_of_rt_rq(rt_rq));

	update_rt_load_avg(now, rt_se, rt_rq, cpu_of(rq_of_rt_rq(rt_rq)));
	attach_entity_load_avg(rt_rq, rt_se);
}

static void detach_task_rt_rq(struct task_struct *p)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	u64 now = rq_clock_task(rq_of_rt_rq(rt_rq));

	update_rt_load_avg(now, rt_se, rt_rq, cpu_of(rq_of_rt_rq(rt_rq)));
	detach_entity_load_avg(rt_rq, rt_se);
}

static void migrate_task_rq_rt(struct task_struct *p)
{
	/*
	 * As for fair, we are supposed to update the task to "current" time,
	 * then its up to date and ready to go to new CPU/rt_rq. But we have
	 * difficulty in getting what current time is, so simply throw away the
	 * out-of-date time. This will result in the wakee task is less
	 * decayed, but giving the wakee more load sounds not bad.
	 */
	remove_entity_load_avg(&p->rt);

	/* Tell new CPU we are migrated */
	p->rt.avg.last_update_time = 0;

	/* We have migrated, no longer consider this task hot */
	p->se.exec_start = 0;
}

static void task_dead_rt(struct task_struct *p)
{
	remove_entity_load_avg(&p->rt);
}

#ifdef CONFIG_RT_GROUP_SCHED
static void task_set_group_rt(struct task_struct *p)
{
	set_task_rq(p, task_cpu(p));
}

static void task_move_group_rt(struct task_struct *p)
{
	detach_task_rt_rq(p);
	set_task_rq(p, task_cpu(p));

#ifdef CONFIG_SMP
	/* Tell se's rt_rq has been changed -- migrated */
	p->rt.avg.last_update_time = 0;
#endif
	attach_task_rt_rq(p);
}

static void task_change_group_rt(struct task_struct *p, int type)
{
	switch (type) {
	case TASK_SET_GROUP:
		task_set_group_rt(p);
		break;
	case TASK_MOVE_GROUP:
		task_move_group_rt(p);
		break;
	}
}
#endif

static void check_preempt_equal_prio(struct rq *rq, struct task_struct *p)
{
	/*
	 * Current can't be migrated, useless to reschedule,
	 * let's hope p can move out.
	 */
	if (tsk_nr_cpus_allowed(rq->curr) == 1 ||
	    !cpupri_find(&rq->rd->cpupri, rq->curr, NULL))
		return;

	/*
	 * p is migratable, so let's not schedule it and
	 * see if it is pushed or pulled somewhere else.
	 */
	if (tsk_nr_cpus_allowed(p) != 1
	    && cpupri_find(&rq->rd->cpupri, p, NULL))
		return;

	/*
	 * There appears to be other cpus that can accept
	 * current and none to run 'p', so lets reschedule
	 * to try and push current away:
	 */
	requeue_task_rt(rq, p, 1);
	resched_curr(rq);
}

#endif /* CONFIG_SMP */

/*
 * Preempt the current task with a newly woken task if needed:
 */
static void check_preempt_curr_rt(struct rq *rq, struct task_struct *p, int flags)
{
	if (p->prio < rq->curr->prio) {
		resched_curr(rq);
		return;
	} else if (test_victim_flag(p)) {
		requeue_task_rt(rq, p, 1);
		resched_curr(rq);
		return;
	}

#ifdef CONFIG_SMP
	/*
	 * If:
	 *
	 * - the newly woken task is of equal priority to the current task
	 * - the newly woken task is non-migratable while current is migratable
	 * - current will be preempted on the next reschedule
	 *
	 * we should check to see if current can readily move to a different
	 * cpu.  If so, we will reschedule to allow the push logic to try
	 * to move current somewhere else, making room for our non-migratable
	 * task.
	 */
	if (p->prio == rq->curr->prio && !test_tsk_need_resched(rq->curr))
		check_preempt_equal_prio(rq, p);
#endif
}

#ifdef CONFIG_SMP
static void sched_rt_update_capacity_req(struct rq *rq)
{
	if (!sched_freq())
		return;

	set_rt_cpu_capacity(rq->cpu, 1, rq->rt.avg.util_avg);
}
#else
static inline void sched_rt_update_capacity_req(struct rq *rq)
{ }

#endif

static struct sched_rt_entity *pick_next_rt_entity(struct rq *rq,
						   struct rt_rq *rt_rq)
{
	struct rt_prio_array *array = &rt_rq->active;
	struct sched_rt_entity *next = NULL;
	struct list_head *queue;
	int idx;

	idx = sched_find_first_bit(array->bitmap);
	BUG_ON(idx >= MAX_RT_PRIO);

	queue = array->queue + idx;
	next = list_entry(queue->next, struct sched_rt_entity, run_list);

	return next;
}

#ifdef CONFIG_SMP
void init_rt_entity_runnable_average(struct sched_rt_entity *rt_se)
{
	struct sched_avg *sa = &rt_se->avg;

	sa->last_update_time = 0;
	/*
	 * sched_avg's period_contrib should be strictly less then 1024, so
	 * we give it 1023 to make sure it is almost a period (1024us), and
	 * will definitely be update (after enqueue).
	 */
	sa->period_contrib = 1023;
	/*
	 * Tasks are intialized with zero load.
	 * Load is not actually used by RT.
	 */
	sa->load_avg = 0;
	sa->load_sum = 0;
	/*
	 * At this point, util_avg won't be used in select_task_rq_fair anyway
	 */
	sa->util_avg = 0;
	sa->util_sum = 0;
	/* when this task enqueue'ed, it will contribute to its rt_rq's load_avg */
}
#else /* !CONFIG_SMP */
void init_rt_entity_runnable_average(struct sched_rt_entity *rt_se) { }
#endif

static struct task_struct *_pick_next_task_rt(struct rq *rq)
{
	struct sched_rt_entity *rt_se;
	struct task_struct *p;
	struct rt_rq *rt_rq  = &rq->rt;
	u64 now = rq_clock_task(rq_of_rt_rq(rt_rq));

	do {
		rt_se = pick_next_rt_entity(rq, rt_rq);
		BUG_ON(!rt_se);
		update_rt_load_avg(now, rt_se, rt_rq, cpu_of(rq_of_rt_rq(rt_rq)));
		rt_rq->curr = rt_se;
		rt_rq = group_rt_rq(rt_se);
	} while (rt_rq);

	p = rt_task_of(rt_se);
	p->se.exec_start = rq_clock_task(rq);

	return p;
}

static struct task_struct *
pick_next_task_rt(struct rq *rq, struct task_struct *prev, struct pin_cookie cookie)
{
	struct task_struct *p;
	struct rt_rq *rt_rq = &rq->rt;

	if (need_pull_rt_task(rq, prev)) {
		/*
		 * This is OK, because current is on_cpu, which avoids it being
		 * picked for load-balance and preemption/IRQs are still
		 * disabled avoiding further scheduler activity on it and we're
		 * being very careful to re-start the picking loop.
		 */
		lockdep_unpin_lock(&rq->lock, cookie);
		pull_rt_task(rq);
		lockdep_repin_lock(&rq->lock, cookie);
		/*
		 * pull_rt_task() can drop (and re-acquire) rq->lock; this
		 * means a dl or stop task can slip in, in which case we need
		 * to re-start task selection.
		 */
		if (unlikely((rq->stop && task_on_rq_queued(rq->stop)) ||
			     rq->dl.dl_nr_running))
			return RETRY_TASK;
	}

	/*
	 * We may dequeue prev's rt_rq in put_prev_task().
	 * So, we update time before rt_nr_running check.
	 */
	if (prev->sched_class == &rt_sched_class)
		update_curr_rt(rq);

	if (!rt_rq->rt_queued) {
		/*
		 * The next task to be picked on this rq will have a lower
		 * priority than rt tasks so we can spend some time to update
		 * the capacity used by rt tasks based on the last activity.
		 * This value will be the used as an estimation of the next
		 * activity.
		 */
		sched_rt_update_capacity_req(rq);
		return NULL;
	}

	put_prev_task(rq, prev);

	p = _pick_next_task_rt(rq);

	/* The running task is never eligible for pushing */
	dequeue_pushable_task(rq, p);

	queue_push_tasks(rq);

	update_rt_rq_load_avg(rq_clock_task(rq), cpu_of(rq), rt_rq,
			rq->curr->sched_class == &rt_sched_class);

	clear_victim_flag(p);

	return p;
}

static void put_prev_task_rt(struct rq *rq, struct task_struct *p)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	u64 now = rq_clock_task(rq_of_rt_rq(rt_rq));

	update_curr_rt(rq);

	/*
	 * The previous task needs to be made eligible for pushing
	 * if it is still active
	 */
	if (on_rt_rq(&p->rt) && tsk_nr_cpus_allowed(p) > 1)
		enqueue_pushable_task(rq, p);

	for_each_sched_rt_entity(rt_se) {
		struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
		if (rt_se->on_rq)
			update_rt_load_avg(now, rt_se, rt_rq, cpu_of(rq_of_rt_rq(rt_rq)));

		rt_rq->curr = NULL;
	}
}

#ifdef CONFIG_SMP

/* Only try algorithms three times */
#define RT_MAX_TRIES 3

static int pick_rt_task(struct rq *rq, struct task_struct *p, int cpu)
{
	if (!task_running(rq, p) &&
	    cpumask_test_cpu(cpu, tsk_cpus_allowed(p)))
		return 1;
	return 0;
}

/*
 * Return the highest pushable rq's task, which is suitable to be executed
 * on the cpu, NULL otherwise
 */
static struct task_struct *pick_highest_pushable_task(struct rq *rq, int cpu)
{
	struct plist_head *head = &rq->rt.pushable_tasks;
	struct task_struct *p;

	if (!has_pushable_tasks(rq))
		return NULL;

	plist_for_each_entry(p, head, pushable_tasks) {
		if (pick_rt_task(rq, p, cpu))
			return p;
	}

	return NULL;
}

static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask);

#ifdef CONFIG_SCHED_USE_FLUID_RT
static inline int weight_from_rtprio(int prio)
{
	int idx = (prio >> 1);

	if (!rt_prio(prio))
		return sched_prio_to_weight[prio - MAX_RT_PRIO];

	if ((idx << 1) == prio)
		return rtprio_to_weight[idx];
	else
		return ((rtprio_to_weight[idx] + rtprio_to_weight[idx+1]) >> 1);
}

extern int exynos_select_cpu_rt(struct sched_domain *sd, struct task_struct *p, bool boost);
extern unsigned long cpu_util_wake(int cpu, struct task_struct *p);
extern unsigned long task_util(struct task_struct *p);

unsigned int frt_boost_threshold;

#define cpu_selected(cpu)	(cpu >= 0)

static int find_victim_rt_rq(struct task_struct *task, struct sched_group *sg, int *best_cpu)
{
	struct cpumask *sg_cpus = sched_group_cpus(sg);
	int i;
	unsigned long victim_rtweight, target_rtweight, min_rtweight;
	unsigned int victim_cpu_cap, min_cpu_cap = arch_scale_cpu_capacity(NULL, task_cpu(task));
	bool victim_rt = true;

	if (!rt_task(task))
		return *best_cpu;

	target_rtweight = task->rt.avg.util_avg * weight_from_rtprio(task->prio);
	min_rtweight = target_rtweight;

	for_each_cpu_and(i, sg_cpus, tsk_cpus_allowed(task)) {
		struct task_struct *victim = cpu_rq(i)->curr;

		if (victim->nr_cpus_allowed < 2)
			continue;

		if (rt_task(victim)) {
			victim_cpu_cap = arch_scale_cpu_capacity(NULL, i);
			victim_rtweight = victim->rt.avg.util_avg * weight_from_rtprio(victim->prio);

			if (min_cpu_cap == victim_cpu_cap) {
				if (victim_rtweight < min_rtweight) {
					min_rtweight = victim_rtweight;
					*best_cpu = i;
					min_cpu_cap = victim_cpu_cap;
				}
			} else {
				/*
				 * It's necessary to un-cap the cpu capacity when comparing
				 * utilization of each CPU. This is why the Fluid RT tries to give
				 * the green light on big CPU to the long-run RT task
				 * in accordance with the priority.
				 */
				if (victim_rtweight * min_cpu_cap < min_rtweight * victim_cpu_cap) {
					min_rtweight = victim_rtweight;
					*best_cpu = i;
					min_cpu_cap = victim_cpu_cap;
				}
			}
		} else {
			/* If Non-RT CPU is exist, select it first. */
			*best_cpu = i;
			victim_rt = false;
			trace_sched_fluid_select_cpu(task, victim, *best_cpu, cpu_util(*best_cpu),
					task_util(task), "Victim Normal Task");
			break;
		}
	}

	if (*best_cpu >= 0 && victim_rt) {
		set_victim_flag(cpu_rq(*best_cpu)->curr);
		trace_sched_fluid_select_cpu(task, cpu_rq(*best_cpu)->curr, *best_cpu,
				cpu_util(*best_cpu), task_util(task), "Victim RT Task");
	}

	return *best_cpu;
}

static int find_lowest_rq_fluid(struct task_struct *task)
{
	struct cpumask *lowest_mask = this_cpu_cpumask_var_ptr(local_cpu_mask);
	struct sched_domain *sd;
	struct sched_group *sg;
	int best_cpu = -1;
	int prev_cpu = task_cpu(task);
	u64 cpu_load = ULLONG_MAX, min_load = ULLONG_MAX;
	u64 rt_cpu_load = ULLONG_MAX, rt_min_load = ULLONG_MAX;
	int min_norm_cpu = -1, min_rt_cpu = -1;
	int i;
	bool boost = false;

	/* Make sure the mask is initialized first */
	if (unlikely(!lowest_mask))
		return best_cpu;

	if (task->nr_cpus_allowed == 1)
		return best_cpu; /* No other targets possible */

	/* find target cpu */
	cpupri_find(&task_rq(task)->rd->cpupri, task, lowest_mask);

	rcu_read_lock();

	sd = rcu_dereference(per_cpu(sd_ea, 0));
	if (!sd)
		goto unlock;

	if (task_util(task) >= frt_boost_threshold)
		boost = true;

	best_cpu = exynos_select_cpu_rt(sd, task, boost);
	if (cpu_selected(best_cpu)) {
		trace_sched_fluid_select_cpu(task, cpu_rq(best_cpu)->curr, best_cpu,
				cpu_util(best_cpu), task_util(task), "Idle CPU Selection");
		goto unlock;
	}

	sg = sd->groups;

	do {
		for_each_cpu_and(i, sched_group_cpus(sg), lowest_mask) {

			cpu_load = cpu_util_wake(i, task) + task_util(task);

			if (rt_task(cpu_rq(i)->curr)) {
				rt_cpu_load = cpu_util_wake(i, task) + task_util(task);

				if (rt_cpu_load < rt_min_load ||
					(rt_cpu_load == rt_min_load && i == prev_cpu)) {
					rt_min_load = rt_cpu_load;
					min_rt_cpu = i;
				}
				continue;
			}

			if (cpu_load < min_load ||
				(cpu_load == min_load && i == prev_cpu)) {
				min_load = cpu_load;
				min_norm_cpu = i;
			}

		}

		/* best min-load non-rt cpu is exist? */
		if (cpu_selected(min_norm_cpu) && (capacity_orig_of(min_norm_cpu) >= min_load)) {
			best_cpu = min_norm_cpu;
			trace_sched_fluid_select_cpu(task, cpu_rq(best_cpu)->curr,
					best_cpu, cpu_util(best_cpu), task_util(task),
					"Non-RT(min_load) CPU Selection");
			goto unlock;
		}

		/* best min-load rt cpu is exist? */
		if (cpu_selected(min_rt_cpu) && (capacity_orig_of(min_rt_cpu) >= min_load)) {
			best_cpu = min_rt_cpu;
			trace_sched_fluid_select_cpu(task, cpu_rq(best_cpu)->curr,
					best_cpu, cpu_util(best_cpu), task_util(task),
					"RT(min_load) CPU Selection");
			goto unlock;
		}

		if (!boost)
			break;
	} while (sg = sg->next, sg != sd->groups);

	sg = sd->groups;

	do {
		if (find_victim_rt_rq(task, sg, &best_cpu) != -1)
			break;
		if (!boost)
			break;
	} while (sg = sg->next, sg != sd->groups);
unlock:
	rcu_read_unlock();

	return best_cpu;
}

static ssize_t show_frt_boost_threshold(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%u\n", frt_boost_threshold);
}

static ssize_t store_frt_boost_threshold(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	input = input < 0 ? 0 : input;
	input = input > 1024 ? 1024 : input;

	frt_boost_threshold = input;

	return count;
}

static struct kobj_attribute frt_boost_threshold_attr =
__ATTR(boost_frt_threshold, 0644, show_frt_boost_threshold,
		store_frt_boost_threshold);
#endif

static ssize_t show_switch_rt_load_ratio(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%u\n", sched_switch_to_rt_load_ratio);
}

static ssize_t store_switch_rt_load_ratio(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	input = input < 0 ? 0 : input;

	sched_switch_to_rt_load_ratio = input;

	return count;
}

static ssize_t show_switch_fair_load_ratio(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%u\n", sched_switch_to_fair_load_ratio);
}

static ssize_t store_switch_fair_load_ratio(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	input = input < 0 ? 0 : input;

	sched_switch_to_fair_load_ratio = input;

	return count;
}

static ssize_t show_rt_ratio_for_freq(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%u\n", 100 - sched_rt_remove_ratio_for_freq);
}

static ssize_t store_rt_ratio_for_freq(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	input = input < 0 ? 0 : input;
	input = input > 100 ? 100 : input;

	sched_rt_remove_ratio_for_freq = 100 - input;

	return count;
}

static struct kobj_attribute switch_fair_load_ratio_attr =
__ATTR(switch_fair_load_ratio, 0644, show_switch_fair_load_ratio,
		store_switch_fair_load_ratio);

static struct kobj_attribute switch_rt_load_ratio_attr =
__ATTR(switch_rt_load_ratio, 0644, show_switch_rt_load_ratio,
		store_switch_rt_load_ratio);

static struct kobj_attribute rt_ratio_for_freq_attr =
__ATTR(rt_ratio_for_freq, 0644, show_rt_ratio_for_freq,
		store_rt_ratio_for_freq);

static int find_lowest_rq(struct task_struct *task)
{
	struct sched_domain *sd;
	struct cpumask *lowest_mask = this_cpu_cpumask_var_ptr(local_cpu_mask);
	int this_cpu = smp_processor_id();
	int cpu      = task_cpu(task);

#ifdef CONFIG_SCHED_USE_FLUID_RT
	return find_lowest_rq_fluid(task);
#endif

	/* Make sure the mask is initialized first */
	if (unlikely(!lowest_mask))
		return -1;

	if (tsk_nr_cpus_allowed(task) == 1)
		return -1; /* No other targets possible */

	if (!cpupri_find(&task_rq(task)->rd->cpupri, task, lowest_mask))
		return -1; /* No targets found */

	/*
	 * At this point we have built a mask of cpus representing the
	 * lowest priority tasks in the system.  Now we want to elect
	 * the best one based on our affinity and topology.
	 *
	 * We prioritize the last cpu that the task executed on since
	 * it is most likely cache-hot in that location.
	 */
	if (cpumask_test_cpu(cpu, lowest_mask))
		return cpu;

	/*
	 * Otherwise, we consult the sched_domains span maps to figure
	 * out which cpu is logically closest to our hot cache data.
	 */
	if (!cpumask_test_cpu(this_cpu, lowest_mask))
		this_cpu = -1; /* Skip this_cpu opt if not among lowest */

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		if (sd->flags & SD_WAKE_AFFINE) {
			int best_cpu;

			/*
			 * "this_cpu" is cheaper to preempt than a
			 * remote processor.
			 */
			if (this_cpu != -1 &&
			    cpumask_test_cpu(this_cpu, sched_domain_span(sd))) {
				rcu_read_unlock();
				return this_cpu;
			}

			best_cpu = cpumask_first_and(lowest_mask,
						     sched_domain_span(sd));
			if (best_cpu < nr_cpu_ids) {
				rcu_read_unlock();
				return best_cpu;
			}
		}
	}
	rcu_read_unlock();

	/*
	 * And finally, if there were no matches within the domains
	 * just give the caller *something* to work with from the compatible
	 * locations.
	 */
	if (this_cpu != -1)
		return this_cpu;

	cpu = cpumask_any(lowest_mask);
	if (cpu < nr_cpu_ids)
		return cpu;
	return -1;
}

/* Will lock the rq it finds */
static struct rq *find_lock_lowest_rq(struct task_struct *task, struct rq *rq)
{
	struct rq *lowest_rq = NULL;
	int tries;
	int cpu;

	for (tries = 0; tries < RT_MAX_TRIES; tries++) {
		cpu = find_lowest_rq(task);

		if ((cpu == -1) || (cpu == rq->cpu))
			break;

		lowest_rq = cpu_rq(cpu);

		if (lowest_rq->rt.highest_prio.curr <= task->prio) {
			/*
			 * Target rq has tasks of equal or higher priority,
			 * retrying does not release any lock and is unlikely
			 * to yield a different result.
			 */
			lowest_rq = NULL;
			break;
		}

		/* if the prio of this runqueue changed, try again */
		if (double_lock_balance(rq, lowest_rq)) {
			/*
			 * We had to unlock the run queue. In
			 * the mean time, task could have
			 * migrated already or had its affinity changed.
			 * Also make sure that it wasn't scheduled on its rq.
			 */
			if (unlikely(task_rq(task) != rq ||
				     !cpumask_test_cpu(lowest_rq->cpu,
						       tsk_cpus_allowed(task)) ||
				     task_running(rq, task) ||
				     !rt_task(task) ||
				     !task_on_rq_queued(task))) {

				double_unlock_balance(rq, lowest_rq);
				lowest_rq = NULL;
				break;
			}
		}

		/* If this rq is still suitable use it. */
		if (lowest_rq->rt.highest_prio.curr > task->prio)
			break;

		/* try again */
		double_unlock_balance(rq, lowest_rq);
		lowest_rq = NULL;
	}

	return lowest_rq;
}

static struct task_struct *pick_next_pushable_task(struct rq *rq)
{
	struct task_struct *p;

	if (!has_pushable_tasks(rq))
		return NULL;

	p = plist_first_entry(&rq->rt.pushable_tasks,
			      struct task_struct, pushable_tasks);

	BUG_ON(rq->cpu != task_cpu(p));
	BUG_ON(task_current(rq, p));
	BUG_ON(tsk_nr_cpus_allowed(p) <= 1);

	BUG_ON(!task_on_rq_queued(p));
	BUG_ON(!rt_task(p));

	return p;
}

/*
 * If the current CPU has more than one RT task, see if the non
 * running task can migrate over to a CPU that is running a task
 * of lesser priority.
 */
static int push_rt_task(struct rq *rq)
{
	struct task_struct *next_task;
	struct rq *lowest_rq;
	int ret = 0;

	if (!rq->rt.overloaded)
		return 0;

	next_task = pick_next_pushable_task(rq);
	if (!next_task)
		return 0;

retry:
	if (unlikely(next_task == rq->curr)) {
		WARN_ON(1);
		return 0;
	}

	/*
	 * It's possible that the next_task slipped in of
	 * higher priority than current. If that's the case
	 * just reschedule current.
	 */
	if (unlikely(next_task->prio < rq->curr->prio)) {
		resched_curr(rq);
		return 0;
	}

	/* We might release rq lock */
	get_task_struct(next_task);

	/* find_lock_lowest_rq locks the rq if found */
	lowest_rq = find_lock_lowest_rq(next_task, rq);
	if (!lowest_rq) {
		struct task_struct *task;
		/*
		 * find_lock_lowest_rq releases rq->lock
		 * so it is possible that next_task has migrated.
		 *
		 * We need to make sure that the task is still on the same
		 * run-queue and is also still the next task eligible for
		 * pushing.
		 */
		task = pick_next_pushable_task(rq);
		if (task_cpu(next_task) == rq->cpu && task == next_task) {
			/*
			 * The task hasn't migrated, and is still the next
			 * eligible task, but we failed to find a run-queue
			 * to push it to.  Do not retry in this case, since
			 * other cpus will pull from us when ready.
			 */
			goto out;
		}

		if (!task)
			/* No more tasks, just exit */
			goto out;

		/*
		 * Something has shifted, try again.
		 */
		put_task_struct(next_task);
		next_task = task;
		goto retry;
	}

	deactivate_task(rq, next_task, 0);
	set_task_cpu(next_task, lowest_rq->cpu);
	activate_task(lowest_rq, next_task, 0);
	ret = 1;

	resched_curr(lowest_rq);

	double_unlock_balance(rq, lowest_rq);

out:
	put_task_struct(next_task);

	return ret;
}

static void push_rt_tasks(struct rq *rq)
{
	/* push_rt_task will return true if it moved an RT */
	while (push_rt_task(rq))
		;
}

#ifdef HAVE_RT_PUSH_IPI

/*
 * When a high priority task schedules out from a CPU and a lower priority
 * task is scheduled in, a check is made to see if there's any RT tasks
 * on other CPUs that are waiting to run because a higher priority RT task
 * is currently running on its CPU. In this case, the CPU with multiple RT
 * tasks queued on it (overloaded) needs to be notified that a CPU has opened
 * up that may be able to run one of its non-running queued RT tasks.
 *
 * All CPUs with overloaded RT tasks need to be notified as there is currently
 * no way to know which of these CPUs have the highest priority task waiting
 * to run. Instead of trying to take a spinlock on each of these CPUs,
 * which has shown to cause large latency when done on machines with many
 * CPUs, sending an IPI to the CPUs to have them push off the overloaded
 * RT tasks waiting to run.
 *
 * Just sending an IPI to each of the CPUs is also an issue, as on large
 * count CPU machines, this can cause an IPI storm on a CPU, especially
 * if its the only CPU with multiple RT tasks queued, and a large number
 * of CPUs scheduling a lower priority task at the same time.
 *
 * Each root domain has its own irq work function that can iterate over
 * all CPUs with RT overloaded tasks. Since all CPUs with overloaded RT
 * tassk must be checked if there's one or many CPUs that are lowering
 * their priority, there's a single irq work iterator that will try to
 * push off RT tasks that are waiting to run.
 *
 * When a CPU schedules a lower priority task, it will kick off the
 * irq work iterator that will jump to each CPU with overloaded RT tasks.
 * As it only takes the first CPU that schedules a lower priority task
 * to start the process, the rto_start variable is incremented and if
 * the atomic result is one, then that CPU will try to take the rto_lock.
 * This prevents high contention on the lock as the process handles all
 * CPUs scheduling lower priority tasks.
 *
 * All CPUs that are scheduling a lower priority task will increment the
 * rt_loop_next variable. This will make sure that the irq work iterator
 * checks all RT overloaded CPUs whenever a CPU schedules a new lower
 * priority task, even if the iterator is in the middle of a scan. Incrementing
 * the rt_loop_next will cause the iterator to perform another scan.
 *
 */
static int rto_next_cpu(struct root_domain *rd)
{
	int next;
	int cpu;

	/*
	 * When starting the IPI RT pushing, the rto_cpu is set to -1,
	 * rt_next_cpu() will simply return the first CPU found in
	 * the rto_mask.
	 *
	 * If rto_next_cpu() is called with rto_cpu is a valid cpu, it
	 * will return the next CPU found in the rto_mask.
	 *
	 * If there are no more CPUs left in the rto_mask, then a check is made
	 * against rto_loop and rto_loop_next. rto_loop is only updated with
	 * the rto_lock held, but any CPU may increment the rto_loop_next
	 * without any locking.
	 */
	for (;;) {

		/* When rto_cpu is -1 this acts like cpumask_first() */
		cpu = cpumask_next(rd->rto_cpu, rd->rto_mask);

		rd->rto_cpu = cpu;

		if (cpu < nr_cpu_ids)
			return cpu;

		rd->rto_cpu = -1;

		/*
		 * ACQUIRE ensures we see the @rto_mask changes
		 * made prior to the @next value observed.
		 *
		 * Matches WMB in rt_set_overload().
		 */
		next = atomic_read_acquire(&rd->rto_loop_next);

		if (rd->rto_loop == next)
			break;

		rd->rto_loop = next;
	}

	return -1;
}

static inline bool rto_start_trylock(atomic_t *v)
{
	return !atomic_cmpxchg_acquire(v, 0, 1);
}

static inline void rto_start_unlock(atomic_t *v)
{
	atomic_set_release(v, 0);
}

static void tell_cpu_to_push(struct rq *rq)
{
	int cpu = -1;

	/* Keep the loop going if the IPI is currently active */
	atomic_inc(&rq->rd->rto_loop_next);

	/* Only one CPU can initiate a loop at a time */
	if (!rto_start_trylock(&rq->rd->rto_loop_start))
		return;

	raw_spin_lock(&rq->rd->rto_lock);

	/*
	 * The rto_cpu is updated under the lock, if it has a valid cpu
	 * then the IPI is still running and will continue due to the
	 * update to loop_next, and nothing needs to be done here.
	 * Otherwise it is finishing up and an ipi needs to be sent.
	 */
	if (rq->rd->rto_cpu < 0)
		cpu = rto_next_cpu(rq->rd);

	raw_spin_unlock(&rq->rd->rto_lock);

	rto_start_unlock(&rq->rd->rto_loop_start);

	if (cpu >= 0) {
		/* Make sure the rd does not get freed while pushing */
		sched_get_rd(rq->rd);
		irq_work_queue_on(&rq->rd->rto_push_work, cpu);
	}
}

/* Called from hardirq context */
void rto_push_irq_work_func(struct irq_work *work)
{
	struct root_domain *rd =
		container_of(work, struct root_domain, rto_push_work);
	struct rq *rq;
	int cpu;

	rq = this_rq();

	/*
	 * We do not need to grab the lock to check for has_pushable_tasks.
	 * When it gets updated, a check is made if a push is possible.
	 */
	if (has_pushable_tasks(rq)) {
		raw_spin_lock(&rq->lock);
		push_rt_tasks(rq);
		raw_spin_unlock(&rq->lock);
	}

	raw_spin_lock(&rd->rto_lock);

	/* Pass the IPI to the next rt overloaded queue */
	cpu = rto_next_cpu(rd);

	raw_spin_unlock(&rd->rto_lock);

	if (cpu < 0) {
		sched_put_rd(rd);
		return;
	}

	/* Try the next RT overloaded CPU */
	irq_work_queue_on(&rd->rto_push_work, cpu);
}
#endif /* HAVE_RT_PUSH_IPI */

static void pull_rt_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, cpu;
	bool resched = false;
	struct task_struct *p;
	struct rq *src_rq;
	int rt_overload_count = rt_overloaded(this_rq);

	if (likely(!rt_overload_count))
		return;

	/*
	 * Match the barrier from rt_set_overloaded; this guarantees that if we
	 * see overloaded we must also see the rto_mask bit.
	 */
	smp_rmb();

	/* If we are the only overloaded CPU do nothing */
	if (rt_overload_count == 1 &&
	    cpumask_test_cpu(this_rq->cpu, this_rq->rd->rto_mask))
		return;

#ifdef HAVE_RT_PUSH_IPI
	if (sched_feat(RT_PUSH_IPI)) {
		tell_cpu_to_push(this_rq);
		return;
	}
#endif

	for_each_cpu(cpu, this_rq->rd->rto_mask) {
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);

		/*
		 * Don't bother taking the src_rq->lock if the next highest
		 * task is known to be lower-priority than our current task.
		 * This may look racy, but if this value is about to go
		 * logically higher, the src_rq will push this task away.
		 * And if its going logically lower, we do not care
		 */
		if (src_rq->rt.highest_prio.next >=
		    this_rq->rt.highest_prio.curr)
			continue;

		/*
		 * We can potentially drop this_rq's lock in
		 * double_lock_balance, and another CPU could
		 * alter this_rq
		 */
		double_lock_balance(this_rq, src_rq);

		/*
		 * We can pull only a task, which is pushable
		 * on its rq, and no others.
		 */
		p = pick_highest_pushable_task(src_rq, this_cpu);

		/*
		 * Do we have an RT task that preempts
		 * the to-be-scheduled task?
		 */
		if (p && (p->prio < this_rq->rt.highest_prio.curr)) {
			WARN_ON(p == src_rq->curr);
			WARN_ON(!task_on_rq_queued(p));

			/*
			 * There's a chance that p is higher in priority
			 * than what's currently running on its cpu.
			 * This is just that p is wakeing up and hasn't
			 * had a chance to schedule. We only pull
			 * p if it is lower in priority than the
			 * current task on the run queue
			 */
			if (p->prio < src_rq->curr->prio)
				goto skip;

			resched = true;

			deactivate_task(src_rq, p, 0);
			set_task_cpu(p, this_cpu);
			activate_task(this_rq, p, 0);
			/*
			 * We continue with the search, just in
			 * case there's an even higher prio task
			 * in another runqueue. (low likelihood
			 * but possible)
			 */
		}
skip:
		double_unlock_balance(this_rq, src_rq);
	}

	if (resched)
		resched_curr(this_rq);
}

/*
 * If we are not running and we are not going to reschedule soon, we should
 * try to push tasks away now
 */
static void task_woken_rt(struct rq *rq, struct task_struct *p)
{
	if (!task_running(rq, p) &&
	    !test_tsk_need_resched(rq->curr) &&
	    tsk_nr_cpus_allowed(p) > 1 &&
	    (dl_task(rq->curr) || rt_task(rq->curr)) &&
	    (tsk_nr_cpus_allowed(rq->curr) < 2 ||
	     rq->curr->prio <= p->prio))
		push_rt_tasks(rq);
}

/* Assumes rq->lock is held */
static void rq_online_rt(struct rq *rq)
{
	if (rq->rt.overloaded)
		rt_set_overload(rq);

	__enable_runtime(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, rq->rt.highest_prio.curr);
}

/* Assumes rq->lock is held */
static void rq_offline_rt(struct rq *rq)
{
	if (rq->rt.overloaded)
		rt_clear_overload(rq);

	__disable_runtime(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, CPUPRI_INVALID);
}

/*
 * When switch from the rt queue, we bring ourselves to a position
 * that we might want to pull RT tasks from other runqueues.
 */
static void switched_from_rt(struct rq *rq, struct task_struct *p)
{
	detach_task_rt_rq(p);
	/*
	 * If there are other RT tasks then we will reschedule
	 * and the scheduling of the other RT tasks will handle
	 * the balancing. But if we are the last RT task
	 * we may need to handle the pulling of RT tasks
	 * now.
	 */
	if (!task_on_rq_queued(p) || rq->rt.rt_nr_running)
		return;

	queue_pull_task(rq);
}

void __init init_sched_rt_class(void)
{
	unsigned int i;

	for_each_possible_cpu(i) {
		zalloc_cpumask_var_node(&per_cpu(local_cpu_mask, i),
					GFP_KERNEL, cpu_to_node(i));
	}

#ifdef CONFIG_SCHED_USE_FLUID_RT
    frt_boost_threshold = 0;
#endif
    sched_switch_to_rt_load_ratio = 0;
    sched_switch_to_fair_load_ratio = 0;
    sched_rt_remove_ratio_for_freq = 0;
}
#endif /* CONFIG_SMP */

extern
void copy_sched_avg(struct sched_avg *from, struct sched_avg *to, unsigned int ratio);

/*
 * When switching a task to RT, we may overload the runqueue
 * with RT tasks. In this case we try to push them off to
 * other runqueues.
 */
static void switched_to_rt(struct rq *rq, struct task_struct *p)
{
	copy_sched_avg(&p->se.avg, &p->rt.avg, sched_switch_to_rt_load_ratio);
	/*
	 * If we are already running, then there's nothing
	 * that needs to be done. But if we are not running
	 * we may need to preempt the current running task.
	 * If that current running task is also an RT task
	 * then see if we can move to another run queue.
	 */
	if (task_on_rq_queued(p) && rq->curr != p) {
#ifdef CONFIG_SMP
		if (tsk_nr_cpus_allowed(p) > 1 && rq->rt.overloaded)
			queue_push_tasks(rq);
#endif /* CONFIG_SMP */
		if (p->prio < rq->curr->prio && cpu_online(cpu_of(rq)))
			resched_curr(rq);
	}
}

/*
 * Priority of the task has changed. This may cause
 * us to initiate a push or pull.
 */
static void
prio_changed_rt(struct rq *rq, struct task_struct *p, int oldprio)
{
	if (!task_on_rq_queued(p))
		return;

	if (rq->curr == p) {
#ifdef CONFIG_SMP
		/*
		 * If our priority decreases while running, we
		 * may need to pull tasks to this runqueue.
		 */
		if (oldprio < p->prio)
			queue_pull_task(rq);

		/*
		 * If there's a higher priority task waiting to run
		 * then reschedule.
		 */
		if (p->prio > rq->rt.highest_prio.curr)
			resched_curr(rq);
#else
		/* For UP simply resched on drop of prio */
		if (oldprio < p->prio)
			resched_curr(rq);
#endif /* CONFIG_SMP */
	} else {
		/*
		 * This task is not running, but if it is
		 * greater than the current running task
		 * then reschedule.
		 */
		if (p->prio < rq->curr->prio)
			resched_curr(rq);
	}
}

static void watchdog(struct rq *rq, struct task_struct *p)
{
	unsigned long soft, hard;

	/* max may change after cur was read, this will be fixed next tick */
	soft = task_rlimit(p, RLIMIT_RTTIME);
	hard = task_rlimit_max(p, RLIMIT_RTTIME);

	if (soft != RLIM_INFINITY) {
		unsigned long next;

		if (p->rt.watchdog_stamp != jiffies) {
			p->rt.timeout++;
			p->rt.watchdog_stamp = jiffies;
		}

		next = DIV_ROUND_UP(min(soft, hard), USEC_PER_SEC/HZ);
		if (p->rt.timeout > next)
			p->cputime_expires.sched_exp = p->se.sum_exec_runtime;
	}
}

static void task_tick_rt(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_rt_entity *rt_se = &p->rt;
	u64 now = rq_clock_task(rq);

	update_curr_rt(rq);

	for_each_sched_rt_entity(rt_se) {
		struct rt_rq *rt_rq = rt_rq_of_se(rt_se);

		update_rt_load_avg(now, rt_se, rt_rq, cpu_of(rq_of_rt_rq(rt_rq)));
	}

	if (rq->rt.rt_nr_running)
		sched_rt_update_capacity_req(rq);

	watchdog(rq, p);

	/*
	 * RR tasks need a special form of timeslice management.
	 * FIFO tasks have no timeslices.
	 */
	if (p->policy != SCHED_RR)
		return;

	if (--p->rt.time_slice)
		return;

	p->rt.time_slice = sched_rr_timeslice;

	/*
	 * Requeue to the end of queue if we (and all of our ancestors) are not
	 * the only element on the queue
	 */
	for_each_sched_rt_entity(rt_se) {
		if (rt_se->run_list.prev != rt_se->run_list.next) {
			requeue_task_rt(rq, p, 0);
			resched_curr(rq);
			return;
		}
	}
}

static void set_curr_task_rt(struct rq *rq)
{
	struct task_struct *p = rq->curr;
	struct sched_rt_entity *rt_se = &p->rt;

	p->se.exec_start = rq_clock_task(rq);

	for_each_sched_rt_entity(rt_se) {
		struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
		rt_rq->curr = rt_se;
	}

	/* The running task is never eligible for pushing */
	dequeue_pushable_task(rq, p);
}

static unsigned int get_rr_interval_rt(struct rq *rq, struct task_struct *task)
{
	/*
	 * Time slice is 0 for SCHED_FIFO tasks
	 */
	if (task->policy == SCHED_RR)
		return sched_rr_timeslice;
	else
		return 0;
}

const struct sched_class rt_sched_class = {
	.next			= &fair_sched_class,
	.enqueue_task		= enqueue_task_rt,
	.dequeue_task		= dequeue_task_rt,
	.yield_task		= yield_task_rt,

	.check_preempt_curr	= check_preempt_curr_rt,

	.pick_next_task		= pick_next_task_rt,
	.put_prev_task		= put_prev_task_rt,

#ifdef CONFIG_SMP
#ifdef CONFIG_SCHED_USE_FLUID_RT
	.select_task_rq		= select_task_rq_rt_fluid,
#else
	.select_task_rq		= select_task_rq_rt,
#endif
	.migrate_task_rq	= migrate_task_rq_rt,

	.set_cpus_allowed       = set_cpus_allowed_common,
	.rq_online              = rq_online_rt,
	.rq_offline             = rq_offline_rt,
	.task_woken		= task_woken_rt,
	.switched_from		= switched_from_rt,
	.task_dead		= task_dead_rt,
#endif

	.set_curr_task          = set_curr_task_rt,
	.task_tick		= task_tick_rt,

	.get_rr_interval	= get_rr_interval_rt,

	.prio_changed		= prio_changed_rt,
	.switched_to		= switched_to_rt,

	.update_curr		= update_curr_rt,
#ifdef CONFIG_RT_GROUP_SCHED
	.task_change_group	= task_change_group_rt,
#endif
};

#ifdef CONFIG_SCHED_DEBUG
extern void print_rt_rq(struct seq_file *m, int cpu, struct rt_rq *rt_rq);

void print_rt_stats(struct seq_file *m, int cpu)
{
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	rcu_read_lock();
	for_each_rt_rq(rt_rq, iter, cpu_rq(cpu))
		print_rt_rq(m, cpu, rt_rq);
	rcu_read_unlock();
}
#endif /* CONFIG_SCHED_DEBUG */

/**********************************************************************
 * Sysfs                                                              *
 **********************************************************************/
static struct attribute *ert_attrs[] = {
#ifdef CONFIG_SCHED_USE_FLUID_RT
	&frt_boost_threshold_attr.attr,
#endif
	&switch_fair_load_ratio_attr.attr,
	&switch_rt_load_ratio_attr.attr,
	&rt_ratio_for_freq_attr.attr,
	NULL,
};

static const struct attribute_group ert_group = {
	.attrs = ert_attrs,
};

static struct kobject *ert_kobj;

static int init_sysfs(void)
{
	int ret;

	ert_kobj = kobject_create_and_add("ert", kernel_kobj);
	ret = sysfs_create_group(ert_kobj, &ert_group);

	return 0;
}
late_initcall(init_sysfs);
