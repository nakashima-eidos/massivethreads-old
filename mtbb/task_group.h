#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <functional>

#if !defined(TASK_GROUP_INIT_SZ)
#define TASK_GROUP_INIT_SZ 10
#endif

#if !defined(TASK_GROUP_NULL_CREATE)
#define TASK_GROUP_NULL_CREATE 0
#endif

struct task {
  pthread_t tid;
  std::function<void ()> f;
};

struct task_list_node {
  task_list_node * next;
  int capacity;
  int n;
  task a[TASK_GROUP_INIT_SZ];
};

void * invoke_task(void * arg_) {
  task * arg = (task *)arg_;
  std::function<void()> f = arg->f;
  f();
  return arg_;
}

struct task_group {
  task_list_node first_chunk_[1];
  task_list_node * head;
  task_list_node * tail;
  task_group() {
    head = first_chunk_;
    tail = first_chunk_;
    head->next = NULL;
    head->capacity = TASK_GROUP_INIT_SZ;
    head->n = 0;
  }
  ~task_group() {
    task_list_node * q = NULL;
    for (task_list_node * p = head; p; p = q) {
      q = p->next;
      if (p != first_chunk_) delete p;
    }
  }

  void extend() {
    task_list_node * new_node = new task_list_node();
    new_node->next = NULL;
    new_node->n = 0;
    new_node->capacity = TASK_GROUP_INIT_SZ;
    tail->next = new_node;
    tail = new_node;
  }
  void run(std::function<void ()> f) {
    if (tail->n == tail->capacity) {
      if (tail->next == NULL) extend();
      else tail = tail->next;
      assert(tail->n == 0);
    }
    task * t = &tail->a[tail->n];
    t->f = f;
    if (TASK_GROUP_NULL_CREATE) {
      invoke_task((void *)t);
    } else {
      tail->n++;
      pthread_create(&t->tid, NULL, invoke_task, (void*)t);
    }
  }
  void wait() {
    for (task_list_node * p = head; p && p->n; p = p->next) {
      for (int i = 0; i < p->n; i++) {
	void * ret;
	pthread_join(p->a[i].tid, &ret);
      }
      p->n = 0;
    }
  }
};
