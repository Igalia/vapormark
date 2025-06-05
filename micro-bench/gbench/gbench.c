/*
 * gbench.c
 *
 * Copyright (C) 2023 Igalia
 * Changwoo Min <changwoo@igalia.com>
 *
 * GPLv2, portions copied from schbench (and potentially from kernel and fio)
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/epoll.h>
#include <unistd.h>

enum {
	IPC_FUTEX,
	IPC_PIPE_EPOLL,
	IPC_SOCK_SELECT,

	IPC_MAX,
};

enum {
	USEC_PER_SEC			= 1000000L,
};

static const char *ipc_str[3] = {
	"futex", "pipe", "sock",
};

struct opt;

union pipe_fds {
	int			fds[2];
	struct {
		int		rfd;
		int		wfd;
	};
};

struct pipe_pair {
	union pipe_fds		rx;
	union pipe_fds		tx;
};

#define MAX_EPOLL_EVENTS	64

struct epoll_ipc {
	int			fd;
	int			nfds;
	struct epoll_event	events[MAX_EPOLL_EVENTS];
};

union ipc {
	int			futex;
	struct pipe_pair	pipe;
	struct epoll_ipc	epoll;
};

#define MAIN_ID			(-1)

struct task_stat {
	__u64			cnt;
	__u64			avg_run_time;
	__u64			frq_run_time;
	__u64			avg_wait_time;
	__u64			frq_wait_time;
};
	
struct task_data {
	struct opt *		opt;
	pthread_t		tid;
	pid_t			pid;
	int			id;
	union ipc		ipc;
	__u64			run_time;
	__u64			wait_time;
	__u64 *			data;
	struct task_stat	stat;
};

struct opt {
	int			ipc_type;
	struct task_data	main;
	int			nr_workers;
	struct task_data *	workers;
	__u64			cache_footprint_kb; /* def: 256kb */
	int			benchmark_time_sec; /* def: 60sec */
};

#ifdef DEBUG
#define debug(fmt, ...)  printf(fmt, __VA_ARGS__)
#else
#define debug(fmt, ...)
#endif

/* the message threads flip this to true when they decide runtime is up */
static volatile unsigned long stopping = 0;

static struct timeval base_time;
static __thread __u64 stick;

/* we're so fancy we make our own futex wrappers */
#define FUTEX_BLOCKED 0
#define FUTEX_RUNNING 1

static int futex(int *uaddr, int futex_op, int val,
		 const struct timespec *timeout, int *uaddr2, int val3)
{
	return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

/*
 * wakeup a process waiting on a futex, making sure they are really waiting
 * first
 */
static void fpost(int *futexp)
{
	int s;

	if (__sync_bool_compare_and_swap(futexp, FUTEX_BLOCKED,
					 FUTEX_RUNNING)) {
		s = futex(futexp, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
		if (s  == -1) {
			perror("FUTEX_WAKE");
			exit(1);
		}
	}
}

/*
 * wait on a futex, with an optional timeout.  Make sure to set
 * the futex to FUTEX_BLOCKED beforehand.
 *
 * This will return zero if all went well, or return -ETIMEDOUT if you
 * hit the timeout without getting posted
 */
static int fwait(int *futexp, struct timespec *timeout)
{
	int s;
	while (1) {
		/* Is the futex available? */
		if (__sync_bool_compare_and_swap(futexp, FUTEX_RUNNING,
						 FUTEX_BLOCKED)) {
			break;      /* Yes */
		}
		/* Futex is not available; wait */
		s = futex(futexp, FUTEX_WAIT_PRIVATE, FUTEX_BLOCKED, timeout, NULL, 0);
		if (s == -1 && errno != EAGAIN) {
			if (errno == ETIMEDOUT)
				return -ETIMEDOUT;
			perror("futex-FUTEX_WAIT");
			exit(1);
		}
	}
	return 0;
}

static __u64 get_time_usec(void)
{
	struct timeval now;
	signed long sec, usec;

	gettimeofday(&now, NULL);

	sec = now.tv_sec - base_time.tv_sec;	
	usec = now.tv_usec - base_time.tv_usec;	

	return sec*1000000 + usec;
}

static __u64 start_tick(void)
{
	__u64 cur_tick = get_time_usec();
	__u64 diff_tick = cur_tick - stick;
	stick = cur_tick;
	return diff_tick;
}

static __u64 get_cur_tick(void)
{
	return get_time_usec() - stick;
}

static __u64 get_matrix_size(struct opt *opt)
{
	return sqrt(opt->cache_footprint_kb * 1024 / 3 / sizeof(__u64));
}

/*
 * multiply two matrices in a naive way to emulate some cache footprint
 */
static void do_some_math(struct task_data *t)
{
	__u64 matrix_size = get_matrix_size(t->opt);
	__u64 i, j, k;
	__u64 *m1, *m2, *m3;

	m1 = &t->data[0];
	m2 = &t->data[matrix_size * matrix_size];
	m3 = &t->data[2 * matrix_size * matrix_size];

	for (i = 0; i < matrix_size; i++) {
		for (j = 0; j < matrix_size; j++) {
			m3[i * matrix_size + j] = 0;

			for (k = 0; k < matrix_size; k++)
				m3[i * matrix_size + j] +=
					m1[i * matrix_size + k] *
					m2[k * matrix_size + j];
		}
	}
}

static __u64 calc_avg(__u64 old_val, __u64 new_val)
{
	/* EWMA = (0.75 * old) + (0.25 * new) */
	return (old_val - (old_val >> 2)) + (new_val >> 2);
}

static __u64 calc_avg_freq(__u64 old_freq, __u64 interval)
{
	__u64 new_freq, ewma_freq;

	new_freq = USEC_PER_SEC / interval;
	ewma_freq = calc_avg(old_freq, new_freq);

	return ewma_freq;
}

static void update_stat(struct task_data *t, __u64 wait_int, __u64 run_dur)
{
	struct task_stat *s = &t->stat;

	s->avg_run_time = calc_avg(s->avg_run_time, run_dur);
	s->frq_run_time = calc_avg_freq(s->frq_run_time, run_dur+wait_int);

	s->avg_wait_time = calc_avg(s->avg_wait_time, wait_int);
	s->frq_wait_time = calc_avg_freq(s->frq_wait_time, run_dur+wait_int);
}

static void do_work(struct task_data *t)
{
	__u64 wait_interval, run_duration;

	/* do some computation */
	wait_interval = start_tick();
	do {
		do_some_math(t);
	} while (t->run_time >= (run_duration = get_cur_tick()));

	/* update statistics */
	update_stat(t, wait_interval, run_duration);
}

static void worker_create_ipc(struct task_data *w)
{
	int ret = 0;

	switch(w->opt->ipc_type) {
	case IPC_FUTEX:
		/* do nothing */
		break;
	case IPC_PIPE_EPOLL:
		/* create a pair of pipe -- rx and tx */
		ret = pipe(w->ipc.pipe.rx.fds);
		if (ret == -1) {
			perror("failed to create an rx pipe\n");
			exit(1);
		}
		ret = pipe(w->ipc.pipe.tx.fds);
		if (ret == -1) {
			perror("failed to create a tx pipe\n");
			exit(1);
		}
		break;
	case IPC_SOCK_SELECT:
		break;
	default:
		fprintf(stderr, "incorrect ipc type: %d\n", w->opt->ipc_type);
		exit(1);
		break;
	}
}

static void worker_ping_pong_futex(struct task_data *w, struct task_data *m)
{
	/* set myself to blocked */
	w->ipc.futex = FUTEX_BLOCKED;

	/* let the main know */
	fpost(&m->ipc.futex);

	/*
	 * don't wait if the main threads are shutting down,
	 * they will never kick us fpost has a full barrier, so as long
	 * as the message thread walks his list after setting stopping,
	 * we shouldn't miss the wakeup
	 */
	if (!stopping) {
		/* if he hasn't already woken us up, wait */
		fwait(&w->ipc.futex, NULL);
	}
}

static void worker_ping_pong_pipe(struct task_data *w, struct task_data *m)
{
	int wr_id = w->id;

	/* let the main know */
	ssize_t w_ret = write(w->ipc.pipe.tx.wfd, &wr_id, sizeof(wr_id));
	if (w_ret != sizeof(wr_id)) {
		perror("worker write failed");
		exit(1);
	}

	/*
	 * don't wait if the main threads are shutting down,
	 * they will never kick us fpost has a full barrier, so as long
	 * as the message thread walks his list after setting stopping,
	 * we shouldn't miss the wakeup
	 */
	if (!stopping) {
		/* if he hasn't already woken us up, wait */
		ssize_t r = read(w->ipc.pipe.rx.rfd, &wr_id, sizeof(wr_id));
		if (r != sizeof(wr_id)) {
			perror("worker read failed");
			exit(1);
		}
	}
}

static int worker_ping_pong(struct task_data *w, struct task_data *m)
{
	/* full memory barrier */
	__sync_synchronize();

	if (stopping)
		return 1;

	switch(w->opt->ipc_type) {
	case IPC_FUTEX:
		worker_ping_pong_futex(w, m);
		break;
	case IPC_PIPE_EPOLL:
		worker_ping_pong_pipe(w, m);
		break;
	case IPC_SOCK_SELECT:
		break;
	default:
		fprintf(stderr, "incorrect ipc type: %d\n", w->opt->ipc_type);
		exit(1);
		break;
	}

	return 0;
}

static void *worker_thr(void *arg)
{
	struct task_data *w = arg;
	struct task_data *m = &w->opt->main;

	w->pid = gettid();

	for (w->stat.cnt = 0; 1; w->stat.cnt++) {
		debug("work[%lx] = %llu\n", w->tid, w->stat.cnt);
		
		/* exchange a heartbeat signal */
		if (worker_ping_pong(w, m))
			break;

		/* do some computation */
		do_work(w);

		/* sleep for a while */
		usleep(w->wait_time);
	}

	return NULL;
}

static 
int main_ping_pong_pipe(struct task_data *m, struct task_data *w, int nr_w)
{
	struct epoll_ipc *epoll = &m->ipc.epoll;
	int wr_id;

	/* unblock workers */
	for (int i = 0; i < epoll->nfds; i++) {
		/* read a ping message from a worker */
		ssize_t r = read(epoll->events[i].data.fd, &wr_id, sizeof(wr_id));
		if (r != sizeof(wr_id)) {
			perror("read failed");
			exit(1);
		}

		/* send a pong message back to the worker */
		ssize_t w_ret = write(w[wr_id].ipc.pipe.rx.wfd, &wr_id, sizeof(wr_id));
		if (w_ret != sizeof(wr_id)) {
			perror("write failed");
			exit(1);
		}
	}

	if (stopping) {
		for (int i = 0; i < nr_w; i++) {
			wr_id = i;
			ssize_t w_ret = write(w[i].ipc.pipe.rx.wfd, &wr_id, sizeof(wr_id));
			if (w_ret != sizeof(wr_id)) {
				perror("write failed (stopping)");
				exit(1);
			}
		}
		return 1;
	}

	/* wait for response from a worker */
	while (!stopping) {
		m->ipc.epoll.nfds = epoll_wait(m->ipc.epoll.fd,
				m->ipc.epoll.events,
				MAX_EPOLL_EVENTS,
				100);
		switch (m->ipc.epoll.nfds) {
		case 0: /* time out then retry */
			__sync_synchronize();
			break;
		case -1: /* error */
			perror("failed to epoll_wait\n");
			exit(1);
			return 1;
		default: /* got some messages */
			return 0;
		}
	}

	return 0;
}

static 
int main_ping_pong_futex(struct task_data *m, struct task_data *w, int nr_w)
{
	m->ipc.futex = FUTEX_BLOCKED;

	/* unblock workers */
	for (int i = 0; i < nr_w; i++)
		fpost(&w[i].ipc.futex);

	if (stopping) {
		for (int i = 0; i < nr_w; i++)
			fpost(&w[i].ipc.futex);
		return 1;
	}

	/* wait for response from a worker */
	fwait(&m->ipc.futex, NULL);

	return 0;
}

static int main_ping_pong(struct task_data *m, struct task_data *w, int nr_w)
{
	/* full memory barrier */
	__sync_synchronize();


	switch(m->opt->ipc_type) {
	case IPC_FUTEX:
		return main_ping_pong_futex(m, w, nr_w);
	case IPC_PIPE_EPOLL:
		return main_ping_pong_pipe(m, w, nr_w);
	case IPC_SOCK_SELECT:
		break;
	default:
		fprintf(stderr, "incorrect ipc type: %d\n", m->opt->ipc_type);
		exit(1);
		break;
	}

	return 0;
}

static void main_create_ipc(struct task_data *m)
{
	int ret = 0;

	switch(m->opt->ipc_type) {
	case IPC_FUTEX:
		/* do nothing */
		break;
	case IPC_PIPE_EPOLL:
		/* create an epoll instance */
		m->ipc.epoll.fd = epoll_create1(0);
		if (m->ipc.epoll.fd == -1) {
			perror("failed to create an epoll\n");
			exit(1);
		}

		/* add workers's tx.rfd to epollfd */
		for (int i = 0; i < m->opt->nr_workers; ++i) {
			struct task_data *w = &m->opt->workers[i];
			struct epoll_event ev;

			ev.events = EPOLLIN;
			ev.data.fd = w->ipc.pipe.tx.rfd;
			ret = epoll_ctl(m->ipc.epoll.fd,
					EPOLL_CTL_ADD,
					w->ipc.pipe.tx.rfd,
					&ev);
			if (ret == -1) {
				perror("failed to add epoll_ctl\n");
				exit(1);
			}
		}
		break;
	case IPC_SOCK_SELECT:
		break;
	default:
		fprintf(stderr, "incorrect ipc type: %d\n", m->opt->ipc_type);
		exit(1);
		break;
	}
}

static void *main_thr(void *arg)
{
	struct opt *opt = arg;
	struct task_data *m = &opt->main;
	struct task_data *w = opt->workers;
	int nr_w = opt->nr_workers;
	int i;
	
	/* init main id */
	m->pid = gettid();
	m->id = MAIN_ID;

	/* launch workers */
	for (i = 0; i < opt->nr_workers; i++) {
		int ret;

		/* Create a worker thread. */
		w[i].id = i;
		worker_create_ipc(&w[i]);
		ret = pthread_create(&w[i].tid, NULL, worker_thr, &w[i]);
		if (ret) {
			fprintf(stderr, "error %d from pthread_create\n", ret);
			exit(1);
		}
	}

	/* init ipc for the main thread */
	main_create_ipc(m);

	/* do its work */
	for (m->stat.cnt = 0; 1; m->stat.cnt++) {
		debug("main[%lx] = %llu\n", m->tid, m->stat.cnt);

		/* exchange a heartbeat signal */
		if (main_ping_pong(m, w, nr_w))
			break;

		/* do some computation */
		do_work(m);

		/* sleep for a while */
		usleep(m->wait_time);
	}

	/* now, it's time to finish. wait for workers. */
	for (i = 0; i < opt->nr_workers; i++)
		pthread_join(w[i].tid, NULL);

	return NULL;
}

static void launch_main_thr(struct opt *opt)
{
	struct task_data *m = &opt->main;
	int ret;

	/* Create a main thread. Workers will be created by the main thread. */
	ret = pthread_create(&m->tid, NULL, main_thr, opt);
	if (ret) {
		fprintf(stderr, "error %d from pthread_create\n", ret);
		exit(1);
	}
}

static void print_usage(void)
{
	fprintf(stderr, "gbench usage:\n"
		"\t-i (--ipc): ipc type: futex, pipe, socket (def: futex)\n"
		"\t-s (--star): workers communicates only through a main threads\n"
		"\t  specify 'r1:w1,r2:w2' where r1 and r2 are run time;\n"
		"\t  w1 and w2 are wait time for each worker thread.\n"
		"\t  time units are all usec.\n"
		"\t-t (--time): benchmark time in seconds (def: 60)\n"
		"\t-F (--cache_footprint): cache footprint (kb, def: 256)\n"
	       );
	exit(1);
}

static int get_nr_toks(char *s)
{
	int nr = 0;

	for (; *s != '\0'; s++) {
		if (*s == ',')
			nr++;
	}
	return nr + 1;
}

static __u64 *alloc_data(struct task_data *t)
{
	int matrix_size = get_matrix_size(t->opt);

	return malloc(3 * sizeof(__u64) * matrix_size * matrix_size);
}

static int parse_subopt_s(struct opt *opt, char *s)
{
	struct task_data *m = &opt->main;
	struct task_data *w;
	int num = get_nr_toks(s);
	char *t;
	int i;

	/* parse run time and wait time for main */
	t = strtok(s, ",:");
	m->run_time = atol(t);
	t = strtok(NULL, ",:");
	m->wait_time = atol(t);
	m->opt = opt;

	/* alloc workers array */
	opt->nr_workers = num - 1;
	opt->workers = w = calloc(num - 1, sizeof(struct task_data));
	if (!opt->workers)
		return -ENOMEM;
	for (i = 0; i < opt->nr_workers; i++) {
		w[i].opt = opt;
		w[i].data = alloc_data(&w[i]);
		if (!w[i].data)
			return -ENOMEM;
	}

	/* parse run time and wait time for each worker */
	for (i = 0; i < opt->nr_workers && t; i++) {
		t = strtok(NULL, ",:");
		w[i].run_time = atol(t);
		t = strtok(NULL, ",:");
		w[i].wait_time = atol(t);
	}

	return 0;
}

static int parse_ipc_type(char *s)
{
	for (int i = 0; i < IPC_MAX; i++) {
		if (strcmp(ipc_str[i], s) == 0)
			return i;
	}

	return -EINVAL;
}

static int parse_options(struct opt *opt, int argc, char **argv)
{
	char *option_string = "i:s:t:F:h";
	static struct option long_options[] = {
		{"ipc", required_argument, 0, 'i'},
		{"time", required_argument, 0, 't'},
		{"star", required_argument, 0, 's'},
		{"cache_footprint", required_argument, 0, 'F'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};
	int c, ret;;

	/* init opt to default values */
	memset(opt, 0, sizeof(*opt));
	opt->ipc_type = IPC_FUTEX;
	opt->cache_footprint_kb = 256;
	opt->benchmark_time_sec = 60;
	
	/* parse options */
	while (1) {
		int option_index = 0;

		c = getopt_long(argc, argv, option_string,
				long_options, &option_index);
		if (c == -1)
			break;

		switch(c) {
		case 'i':
			ret = parse_ipc_type(optarg);
			if (ret < 0 )
				return ret;
			opt->ipc_type = ret;
			break;
		case 't':
			opt->benchmark_time_sec = atoi(optarg);
			break;
		case 's':
			ret = parse_subopt_s(opt, optarg);
			if (ret)
				return ret;
			break;
		case 'F':
			opt->cache_footprint_kb = atoi(optarg);
			break;
		default:
			print_usage();
			break;
		}
	}

	/* further initialize the main */
	opt->main.data = alloc_data(&opt->main);
	if (!opt->main.data)
		return -ENOMEM;

	/* sanity check */
	if (opt->nr_workers < 1)
		print_usage();

	return 0;
}

static void stop_benchmark(struct opt *opt)
{
	struct task_data *m = &opt->main;

	/* full memory barrier */
	__sync_synchronize();

	/* then update it atomically */
	__sync_bool_compare_and_swap(&stopping, 0, 1);

	/* finally waiting for the termination of the main */
	fpost(&m->ipc.futex);
	pthread_join(m->tid, NULL);
}

static void show_results(struct opt *opt)
{
	struct task_data *m = &opt->main;
	struct task_data *w = opt->workers;
	int nr_w = opt->nr_workers;

	printf("# thread\t %10s  %10s  %10s  %10s  %10s  %10s  %10s\n",
		"run_t", "run_a", "run_f", "wait_t", "wait_a", "wait_f", "cnt");
	printf("main-thr[%d]\t  %10lld  %10lld  %10lld  "
		"%10lld  %10lld  %10lld  %10lld\n",
		m->pid,
		m->run_time, m->stat.avg_run_time, m->stat.frq_run_time,
		m->wait_time, m->stat.avg_wait_time, m->stat.frq_wait_time,
		m->stat.cnt);

	for (int i = 0; i < nr_w; i++) {
		printf("worker[%d]-%d\t  %10lld  %10lld  %10lld  "
			"%10lld  %10lld  %10lld  %10lld\n",
			w[i].pid, i, w[i].run_time, w[i].stat.avg_run_time,
			w[i].stat.frq_run_time, w[i].wait_time,
			w[i].stat.avg_wait_time, w[i].stat.frq_wait_time,
			w[i].stat.cnt);
	}
}

static void init(void)
{
	/* init base time for overflow-free time calculation */
	gettimeofday(&base_time, NULL);
}

int main(int argc, char **argv)
{
	struct opt opt;

	init();
	parse_options(&opt, argc, argv);
	launch_main_thr(&opt);
	sleep(opt.benchmark_time_sec);
	stop_benchmark(&opt);
	show_results(&opt);

	return 0;
}
