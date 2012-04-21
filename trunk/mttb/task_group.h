#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <functional>

struct task {
  pthread_t tid;
  std::function<void ()> f;
};

void * invoke_task(void * arg_) {
  task * arg = (task *)arg_;
  std::function<void()> f = arg->f;
  f();
  return arg_;
}

#if !defined(TASK_GROUP_INIT_SZ)
#define TASK_GROUP_INIT_SZ 10
#endif

#if !defined(TASK_GROUP_NULL_CREATE)
#define TASK_GROUP_NULL_CREATE 0
#endif

struct task_group {
  int n;
  int capacity;
  task tasks_[TASK_GROUP_INIT_SZ];
  task * tasks;
  task_group() {
    n = 0;
    capacity = TASK_GROUP_INIT_SZ;
    tasks = tasks_;
  }
  void extend() {
    int new_capacity = capacity + capacity + 1;
    task * new_tasks = (task *)malloc(sizeof(task) * new_capacity);
    assert(new_tasks);
    memcpy(new_tasks, tasks, sizeof(task) * capacity);
    if (tasks == tasks_) {
      memset(tasks, 0, sizeof(task) * capacity);
    } else {
      free(tasks);
    }
    tasks = new_tasks;
    capacity = new_capacity;
  }
  void run(std::function<void ()> f) {
    if (n == capacity) extend();
    task * t = &tasks[n];
    t->f = f;
    if (TASK_GROUP_NULL_CREATE) {
      invoke_task((void *)t);
    } else {
      n++;
      pthread_create(&t->tid, NULL, invoke_task, (void*)t);
    }
  }
  void wait() {
    int i;
    if (n > 0) {
      for (i = 0; i < n; i++) {
	void * ret;
	pthread_join(tasks[i].tid, &ret);
      }
      n = 0;
    }
  }
};
