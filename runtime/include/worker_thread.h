#pragma once

#include "generic_thread.h"
#include "runtime.h"

extern __thread struct arch_context worker_thread_base_context;
extern __thread int                 worker_thread_epoll_file_descriptor;
extern __thread int                 worker_thread_idx;

void *worker_thread_main(void *return_code);
