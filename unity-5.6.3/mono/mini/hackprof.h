#pragma once
#include "mini.h"

void hackprof_init_after_agent();
void hackprof_init_new_thread(MonoThread *thread);
void hackprof_init_new_thread_locked(MonoThread *thread);
