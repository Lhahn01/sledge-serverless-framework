#include "current_sandbox.h"
#include "sandbox_functions.h"
#include "sandbox_receive_request.h"
#include "sandbox_send_response.h"
#include "sandbox_set_as_error.h"
#include "sandbox_set_as_returned.h"
#include "scheduler.h"
#include "software_interrupt.h"

__thread struct sandbox *worker_thread_current_sandbox = NULL;

__thread struct sandbox_context_cache local_sandbox_context_cache = {
	.wasi_context = NULL,
	.memory = {
		.start = NULL,
		.size = 0,
		.max = 0,
	},
	.module_indirect_table = NULL,
};

// TODO: Propagate arguments from *.json spec file
const int   dummy_argc   = 4;
const char *dummy_argv[] = { "Test", "One", "Two", "Three" };

static inline void
current_sandbox_enable_preemption(struct sandbox *sandbox)
{
#ifdef LOG_PREEMPTION
	debuglog("Sandbox %lu - enabling preemption - Missed %d SIGALRM\n", sandbox->id,
	         software_interrupt_deferred_sigalrm);
	fflush(stderr);
#endif
	if (__sync_bool_compare_and_swap(&sandbox->ctxt.preemptable, 0, 1) == false) {
		panic("Recursive call to current_sandbox_enable_preemption\n");
	}

	if (software_interrupt_deferred_sigalrm > 0) {
		/* Update Max */
		if (software_interrupt_deferred_sigalrm > software_interrupt_deferred_sigalrm_max[worker_thread_idx]) {
			software_interrupt_deferred_sigalrm_max[worker_thread_idx] =
			  software_interrupt_deferred_sigalrm;
		}

		software_interrupt_deferred_sigalrm = 0;
		// TODO: Replay. Does the replay need to be before or after enabling preemption?
	}
}

static inline void
current_sandbox_disable_preemption(struct sandbox *sandbox)
{
#ifdef LOG_PREEMPTION
	debuglog("Sandbox %lu - disabling preemption\n", sandbox->id);
	fflush(stderr);
#endif
	if (__sync_bool_compare_and_swap(&sandbox->ctxt.preemptable, 1, 0) == false) {
		panic("Recursive call to current_sandbox_disable_preemption\n");
	}
}

/**
 * Sandbox execution logic
 * Handles setup, request parsing, WebAssembly initialization, function execution, response building and
 * sending, and cleanup
 */
void
current_sandbox_start(void)
{
	struct sandbox *sandbox = current_sandbox_get();
	assert(sandbox != NULL);
	assert(sandbox->state == SANDBOX_RUNNING);

	char *error_message = "";

	sandbox_open_http(sandbox);

	if (sandbox_receive_request(sandbox) < 0) {
		error_message = "Unable to receive or parse client request\n";
		goto err;
	};

	/* Initialize sandbox memory */
	struct module *current_module = sandbox_get_module(sandbox);
	/* TODO: Why is the initialize_globals symbol not present here? */
	// module_initialize_globals(current_module);
	module_initialize_memory(current_module);

	/* Initialize WASI */
	wasi_options_t options;
	wasi_options_init(&options);
	options.argc                             = dummy_argc;
	options.argv                             = dummy_argv;
	sandbox->wasi_context                    = wasi_context_init(&options);
	local_sandbox_context_cache.wasi_context = sandbox->wasi_context;
	assert(sandbox->wasi_context != NULL);

	/* Executing the function */
	current_sandbox_enable_preemption(sandbox);
	sandbox->return_value = module_entrypoint(current_module);
	current_sandbox_disable_preemption(sandbox);
	sandbox->timestamp_of.completion = __getcycles();

	/* Retrieve the result, construct the HTTP response, and send to client */
	if (sandbox_send_response(sandbox) < 0) {
		error_message = "Unable to build and send client response\n";
		goto err;
	};

	http_total_increment_2xx();

	sandbox->timestamp_of.response = __getcycles();

	assert(sandbox->state == SANDBOX_RUNNING);
	sandbox_close_http(sandbox);
	sandbox_set_as_returned(sandbox, SANDBOX_RUNNING);

done:
	/* Cleanup connection and exit sandbox */
	generic_thread_dump_lock_overhead();
	scheduler_yield();

	/* This assert prevents a segfault discussed in
	 * https://github.com/phanikishoreg/awsm-Serverless-Framework/issues/66
	 */
	assert(0);
err:
	debuglog("%s", error_message);
	assert(sandbox->state == SANDBOX_RUNNING);

	/* Send a 400 error back to the client */
	client_socket_send(sandbox->client_socket_descriptor, 400);

	sandbox_close_http(sandbox);
	sandbox_set_as_error(sandbox, SANDBOX_RUNNING);
	goto done;
}
