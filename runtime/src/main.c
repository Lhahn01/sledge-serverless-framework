#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <runtime.h>
#include <sandbox.h>
#include <softint.h>
#include <util.h>
#include <sys/time.h>
#include <sys/resource.h>

i32 log_file_descriptor = -1;
u32 total_online_processors = 0;
u32 total_worker_processors = 0;
u32 first_worker_processor = 0;
int worker_threads_argument[SBOX_NCORES] = { 0 }; // This is always empty, as we don't pass an argument
pthread_t worker_threads[SBOX_NCORES];

static unsigned long long
get_time()
{
	struct timeval Tp;
	int            stat;
	stat = gettimeofday(&Tp, NULL);
	if (stat != 0) printf("Error return from gettimeofday: %d", stat);
	return (Tp.tv_sec * 1000000 + Tp.tv_usec);
}

static void
usage(char *cmd)
{
	printf("%s <modules_file>\n", cmd);
	debuglog("%s <modules_file>\n", cmd);
}

// Sets the process data segment (RLIMIT_DATA) and # file descriptors 
// (RLIMIT_NOFILE) soft limit to its hard limit (see man getrlimit)
void set_resource_limits_to_max(){
	struct rlimit resource_limit;
	if (getrlimit(RLIMIT_DATA, &resource_limit) < 0) {
		perror("getrlimit RLIMIT_DATA");
		exit(-1);
	}
	resource_limit.rlim_cur = resource_limit.rlim_max;
	if (setrlimit(RLIMIT_DATA, &resource_limit) < 0) {
		perror("setrlimit RLIMIT_DATA");
		exit(-1);
	}
	if (getrlimit(RLIMIT_NOFILE, &resource_limit) < 0) {
		perror("getrlimit RLIMIT_NOFILE");
		exit(-1);
	}
	resource_limit.rlim_cur = resource_limit.rlim_max;
	if (setrlimit(RLIMIT_NOFILE, &resource_limit) < 0) {
		perror("setrlimit RLIMIT_NOFILE");
		exit(-1);
	}
}

void allocate_available_cores(){
	// Find the number of processors currently online
	total_online_processors = sysconf(_SC_NPROCESSORS_ONLN);

	// If multicore, we'll pin one core as a listener and run sandbox threads on all others
	if (total_online_processors > 1) {
		first_worker_processor = 1;
		// SBOX_NCORES can be used as a cap on the number of cores to use
		// But if there are few cores that SBOX_NCORES, just use what is available
		u32 max_possible_workers = total_online_processors - 1;
		total_worker_processors = (max_possible_workers >= SBOX_NCORES) ? SBOX_NCORES : max_possible_workers;
	} else {
		// If single core, we'll do everything on CPUID 0
		first_worker_processor = 0;
		total_worker_processors = 1;
	}
	debuglog("Number of cores %u, sandboxing cores %u (start: %u) and module reqs %u\n", total_online_processors, total_worker_processors,
	         first_worker_processor, MOD_REQ_CORE);
}

// If NOSTIO is defined, close stdin, stdout, stderr, and write to logfile named awesome.log. Otherwise, log to STDOUT
// NOSTIO = No Standard Input/Output?
void process_nostio(){
#ifdef NOSTDIO
	fclose(stdout);
	fclose(stderr);
	fclose(stdin);
	log_file_descriptor = open(LOGFILE, O_CREAT | O_TRUNC | O_WRONLY, S_IRWXU | S_IRWXG);
	if (log_file_descriptor < 0) {
		perror("open");
		exit(-1);
	}
#else
	log_file_descriptor = 1;
#endif
}

void start_worker_threads(){
	for (int i = 0; i < total_worker_processors; i++) {
		int ret = pthread_create(&worker_threads[i], NULL, sandbox_run_func, (void *)&worker_threads_argument[i]);
		if (ret) {
			errno = ret;
			perror("pthread_create");
			exit(-1);
		}

		cpu_set_t cs;
		CPU_ZERO(&cs);
		CPU_SET(first_worker_processor + i, &cs);
		ret = pthread_setaffinity_np(worker_threads[i], sizeof(cs), &cs);
		assert(ret == 0);
	}
	debuglog("Sandboxing environment ready!\n");

	for (int i = 0; i < total_worker_processors; i++) {
		int ret = pthread_join(worker_threads[i], NULL);
		if (ret) {
			errno = ret;
			perror("pthread_join");
			exit(-1);
		}
	}

	printf("\nWorker Threads unexpectedly returned!!\n");
	exit(-1);
}

void execute_standalone(int argc, char **argv){
	arch_context_init(&base_context, 0, 0);
	uv_loop_init(&uvio);

	int   ac   = 1;
	char *args = argv[1];
	if (argc - 1 > 1) {
		ac        = argc - 1;
		char **av = argv + 1;
		args      = malloc(sizeof(char) * MOD_ARG_MAX_SZ * ac);
		memset(args, 0, sizeof(char) * MOD_ARG_MAX_SZ * ac);

		for (int i = 0; i < ac; i++) {
			char *a = args + (i * MOD_ARG_MAX_SZ * sizeof(char));
			strcpy(a, av[i]);
		}
	}

	/* in current dir! */
	struct module *m = module_alloc(args, args, ac, 0, 0, 0, 0, 0, 0);
	assert(m);

	// unsigned long long st = get_time(), en;
	struct sandbox *s = sandbox_alloc(m, args, 0, NULL);
	// en = get_time();
	// fprintf(stderr, "%llu\n", en - st);

	exit(0);
}

int
main(int argc, char **argv)
{
	printf("Starting Awsm\n");
#ifndef STANDALONE
	if (argc != 2) {
		usage(argv[0]);
		exit(-1);
	}

	memset(worker_threads, 0, sizeof(pthread_t) * SBOX_NCORES);

	set_resource_limits_to_max();
	allocate_available_cores();
	process_nostio();
	runtime_init();

	debuglog("Parsing modules file [%s]\n", argv[1]);
	if (util_parse_modules_file_json(argv[1])) {
		printf("failed to parse modules file[%s]\n", argv[1]);
		exit(-1);
	}

	runtime_thd_init();
	start_worker_threads();

#else /* STANDALONE */
	execute_standalone();
#endif
}
