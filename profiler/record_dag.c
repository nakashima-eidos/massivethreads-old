/* 
 * dag_recorder.c
 */

#include <errno.h>
#include <string.h>

#include "dag_recorder_impl.h"

dr_global_state GS = {
  0,				/* initialized */
  0,				/* num_workers */
  0,				/* root */
  0,				/* start_clock */
  0,				/* thread_specific */
  0,				/* ts */
};

/* --------------------- dr_get_worker ------------------- */

dr_get_worker_key_struct dr_gwks = { 0, 0, 0 };

/* --------------- depth first traverse a graph ------------ */

/* 
 stack of (dag nodes | string); infrastructure for
 printing hierarchical dag structure
 without recursion.
*/

typedef struct dr_dag_node_stack_cell {
  /* next cell in the stack or a free list */
  struct dr_dag_node_stack_cell * next;
  /* one and only one of these two fields are non-null */
  dr_dag_node * node;		/* this is a node */
} dr_dag_node_stack_cell;

/* TODO:
   probably we should make it larger, so that
   we don't call malloc too many times.
   to do so, we explictly need to maintain
   the set of addresses obtained from malloc.
   currently there are no track of them.
   in the end we simply call free for individual
   cells (see dr_dag_node_stack_fini), and
   it is wrong when dr_dag_node_stack_cell_sz > 1
 */
enum { dr_dag_node_stack_cell_sz = 1 };

typedef struct dr_dag_node_stack {
  /* free list (recycle popped cells) */
  dr_dag_node_stack_cell * freelist;
  /* the stack (linear list) */
  dr_dag_node_stack_cell * top;
} dr_dag_node_stack;

/* initialize the stack to be empty */
static void 
dr_dag_node_stack_init(dr_dag_node_stack * s) {
  s->freelist = 0;
  s->top = 0;
}

static void 
dr_dag_node_stack_fini(dr_dag_node_stack * s) {
  (void)dr_check(!s->top);
  dr_dag_node_stack_cell * cell;
  dr_dag_node_stack_cell * next;
  for (cell = s->freelist; cell; cell = next) {
    next = cell->next;
    dr_free(cell, sizeof(dr_dag_node_stack_cell) * dr_dag_node_stack_cell_sz);
  }
}

/* ensure the free list is not empty.
   get memory via malloc and fill the free list.
   return a pointer to a cell */
static dr_dag_node_stack_cell * 
ensure_freelist(dr_dag_node_stack * s) {
  dr_dag_node_stack_cell * f = s->freelist;
  if (!f) {
    int n = dr_dag_node_stack_cell_sz;
    f = (dr_dag_node_stack_cell *)
      dr_malloc(sizeof(dr_dag_node_stack_cell) * n);
    int i;
    for (i = 0; i < n - 1; i++) {
      f[i].next = &f[i + 1];
    }
    f[n - 1].next = 0;
    s->freelist = f;
  }
  return f;
}

/* push a dag node to the stack */
static void 
dr_dag_node_stack_push(dr_dag_node_stack * s, 
		       dr_dag_node * node) {
  dr_dag_node_stack_cell * f = ensure_freelist(s);
  f->node = node;
  s->freelist = f->next;
  f->next = s->top;
  s->top = f;
}

/* pop an element from the stack s.
   it is either a string or a dag node.
   the result is returned to one of 
   *xp (when it is a node) and *strp
   (when it is a string). the other one
   is filled with a null.
*/
static dr_dag_node * 
dr_dag_node_stack_pop(dr_dag_node_stack * s) {
  dr_dag_node_stack_cell * top = s->top;
  (void)dr_check(top);
  s->top = top->next;
  top->next = s->freelist;
  s->freelist = top;
  return top->node;
}

/* push the children of g in the reverse order,
   so that we later handle the children in the
   right order */
static void
dr_dag_node_stack_push_children(dr_dag_node_stack * s, 
				dr_dag_node * g) {
  if (g->info.kind < dr_dag_node_kind_section) {
    if (g->info.kind == dr_dag_node_kind_create_task
	&& g->child) {
      dr_dag_node_stack_push(s, g->child);
    }
  } else {
    dr_dag_node * head = g->subgraphs->head;
    dr_dag_node * tail = g->subgraphs->tail;
    dr_dag_node * ch;
    /* a bit complicated to reverse the children */

    /* 1. count the number of children */
    int n_children = 0;
    for (ch = head; ch; ch = ch->next) {
      n_children++;
    }
    /* 2. make the array of the right size and
       fill the array with children */
    dr_dag_node ** children
      = (dr_dag_node **)dr_malloc(sizeof(dr_dag_node *) * n_children);
    int idx = 0;
    dr_dag_node_kind_t K = g->info.kind;
    for (ch = head; ch; ch = ch->next) {
      dr_dag_node_kind_t k = ch->info.kind;
      if (DAG_RECORDER_CHK_LEVEL>=1) {
	if (K == dr_dag_node_kind_section) {
	  (void)dr_check(k == dr_dag_node_kind_create_task 
			 || k == dr_dag_node_kind_section
			 || k == dr_dag_node_kind_wait_tasks);
	  if (k == dr_dag_node_kind_wait_tasks) {
	    (void)dr_check(ch == tail);
	  }
	} else {
	  (void)dr_check(K == dr_dag_node_kind_task);
	  (void)dr_check(k == dr_dag_node_kind_section
			 || k == dr_dag_node_kind_end_task);
	  if (k == dr_dag_node_kind_end_task) {
	    (void)dr_check(ch == tail);
	  }
	}
      }
      children[idx++] = ch;
    }
    assert(idx == n_children);
    /* 3. finally push them in the reverse order */
    for (idx = n_children - 1; idx >= 0; idx--) {
      dr_dag_node_stack_push(s, children[idx]);
    }
    dr_free(children, sizeof(dr_dag_node *) * n_children);
  }
}

/* --- free all descendants of g (and optionally g also) --- */

static void 
dr_dag_node_free(dr_dag_node * n, 
		 dr_dag_node_freelist * fl) {
#if DAG_RECORDER_VALGRIND_MEM_DBG
  return dr_free(n, sizeof(dr_dag_node));
#else
  n->next = fl->head;
  if (!fl->head)
    fl->tail = n;
  fl->head = n;
#endif
}

static void 
dr_dag_node_list_free(dr_dag_node_list * l, 
		      dr_dag_node_freelist * fl) 
  __attribute__ ((unused));

/* free all nodes in l */
static void 
dr_dag_node_list_free(dr_dag_node_list * l, 
		      dr_dag_node_freelist * fl) {
#if DAG_RECORDER_VALGRIND_MEM_DBG
  dr_dag_node * ch;
  dr_dag_node * next;
  for (ch = l->head; ch; ch = next) {
    next = ch->next;
    dr_dag_node_free(ch, fl);
  }
  dr_dag_node_list_init(l);
#else
  if (l->head) {
    (void)dr_check(l->tail);
    (void)dr_check(!l->tail->next);
    l->tail->next = fl->head;
    if (!fl->head)
      fl->tail = l->tail;
    fl->head = l->head;
    dr_dag_node_list_init(l);
  } else {
    (void)dr_check(!l->tail);
  }
#endif
  (void)dr_check(!l->head);
  (void)dr_check(!l->tail);
}

void 
dr_free_dag(dr_dag_node * g, int free_root,
	    dr_dag_node_freelist * fl) {
  dr_dag_node_stack s[1];
  dr_dag_node_stack_init(s);
  dr_dag_node_stack_push(s, g);
  while (s->top) {
    dr_dag_node * x = dr_dag_node_stack_pop(s);
    if (x->info.kind == dr_dag_node_kind_create_task) {
      if (x->child) {
	dr_dag_node_stack_push(s, x->child);
      }
    } else if (x->info.kind >= dr_dag_node_kind_section) {
      dr_dag_node_stack_push_children(s, x);
    }
    if (x != g) dr_dag_node_free(x, fl);
  }
  if (free_root) {
    dr_dag_node_free(g, fl);
  } else {
    dr_dag_node_list_init(g->subgraphs);
  }
  dr_dag_node_stack_fini(s);
}

/* --- make a position-independent copy of a graph --- */

typedef struct dr_string_table_cell {
  struct dr_string_table_cell * next;
  const char * s;
} dr_string_table_cell;

typedef struct {
  dr_string_table_cell * head;
  dr_string_table_cell * tail;
  long n;
} dr_string_table;

static void 
dr_string_table_init(dr_string_table * t) {
  t->n = 0;
  t->head = t->tail = 0;
}

static void 
dr_string_table_destroy(dr_string_table * t) {
  dr_string_table_cell * c;
  dr_string_table_cell * next;
  for (c = t->head; c; c = next) {
    next = c->next;
    dr_free(c, sizeof(dr_string_table_cell));
  }
}

static long 
dr_string_table_find(dr_string_table * t, const char * s) {
  dr_string_table_cell * c;
  long i = 0;
  for (c = t->head; c; c = c->next) {
    if (strcmp(c->s, s) == 0) return i;
    i++;
  }
  (void)dr_check(i == t->n);
  return i;
}

static void
dr_string_table_append(dr_string_table * t, const char * s) {
  dr_string_table_cell * c 
    = (dr_string_table_cell *)dr_malloc(sizeof(dr_string_table_cell));
  c->s = s;
  c->next = 0;
  if (t->head) {
    (void)dr_check(t->tail);
    t->tail->next = c;
  } else {
    (void)dr_check(!t->tail);
    t->head = c;
  }
  t->tail = c;
  t->n++;
}

/* find s in the string table t.
   if not found, return a new index */
static long 
dr_string_table_intern(dr_string_table * t, const char * s) {
  long idx = dr_string_table_find(t, s);
  if (idx == t->n) {
    dr_string_table_append(t, s);
  }
  return idx;
}

/* given linked-list based string table t,
   flatten it.
   flattened table consists of
   a flat char array contigously
   storing all strings and an array
   of indexes into that string array;
   these two arrays are also stored
   contiguously in memory.
   there is also a header pointing to them.
   
   before:
   |abc|-->|defg|-->|hi|-->|

   after:
 +-0
 | 4 -----+            Index Array
 | 9 -----+-----+
 |        |     |
 +-> abc\0defg\0hi\0   Char Array
     0... .5... .10

*/
static dr_pi_string_table *
dr_string_table_flatten(dr_string_table * t) {
  long str_bytes = 0;		/* string length */
  int n = 0;
  dr_string_table_cell * c;
  for (c = t->head; c; c = c->next) {
    n++;
    str_bytes += strlen(c->s) + 1;
  }  
  {
    long header_bytes = sizeof(dr_pi_string_table);
    long table_bytes = n * sizeof(const char *);
    long total_bytes = header_bytes + table_bytes + str_bytes;
    void * a = dr_malloc(total_bytes); /* leaking */
    dr_pi_string_table * h = a;
    /* index array */
    long * I = a + header_bytes;
    /* char array */
    char * C = a + header_bytes + table_bytes;
    char * p = C;
    long i = 0;
    h->n = n;
    h->sz = total_bytes;
    h->I = I;
    h->C = C;
    for (c = t->head; c; c = c->next) {
      strcpy(p, c->s);
      I[i] = p - C;
      p += strlen(c->s) + 1;
      i++;
    }  
    (void)dr_check(i == n);
    (void)dr_check(p == C + str_bytes);
    return h;
  }
}

/* copy g into p */
static void
dr_pi_dag_copy_1(dr_dag_node * g, 
		 dr_pi_dag_node * p, dr_pi_dag_node * lim,
		 dr_string_table * st) {
  assert(p < lim);
  p->info = g->info;
  p->info.start.pos.file = 0;
  p->info.start.pos.file_idx
    = dr_string_table_intern(st, g->info.start.pos.file);
  p->info.end.pos.file = 0;
  p->info.end.pos.file_idx
    = dr_string_table_intern(st, g->info.end.pos.file);
  g->forward = p;		/* record g was copied to p */
}

/* g has been copied, but its children have not been.
   copy g's children into q,
   and set g's children pointers to the copy,
   making g truly position independent now
 */
static dr_pi_dag_node * 
dr_pi_dag_copy_children(dr_dag_node * g, 
			dr_pi_dag_node * p, 
			dr_pi_dag_node * lim,
			dr_clock_t start_clock,
			dr_string_table * st) {
  /* where g has been copied */
  dr_pi_dag_node * g_pi = g->forward;
  /* sanity check. g_pi should be a copy of g */
  assert(g_pi->info.start.t == g->info.start.t);
  /* make the time relative */
  g_pi->info.start.t       -= start_clock;
  g_pi->info.end.t         -= start_clock;
  g_pi->info.first_ready_t -= start_clock;
  g_pi->info.last_start_t  -= start_clock;

  if (g_pi->info.kind < dr_dag_node_kind_section) {
    /* copy the child if it is a create_task node */
    if (g_pi->info.kind == dr_dag_node_kind_create_task) {
      dr_pi_dag_copy_1(g->child, p, lim, st);
      /* install the (relative) pointer to the child */
      g_pi->child_offset = p - g_pi;
      p++;
    }
  } else {
    dr_dag_node * head = g->subgraphs->head;
    dr_dag_node * ch;
    g_pi->subgraphs_begin_offset = p - g_pi;
    for (ch = head; ch; ch = ch->next) {
      dr_pi_dag_copy_1(ch, p, lim, st);
      p++;
    }
    g_pi->subgraphs_end_offset = p - g_pi;
  }
  return p;
}

/* --- count the number of nodes under g --- */

static long
dr_dag_count_nodes(dr_dag_node * g) {
  /* count the number of elements popped from stack */
  long n = 0;
  dr_dag_node_stack s[1];
  dr_dag_node_stack_init(s);
  dr_dag_node_stack_push(s, g);
  while (s->top) {
    dr_dag_node * x = dr_dag_node_stack_pop(s);
    n++;
    if (x->info.kind < dr_dag_node_kind_section) {
      if (x->info.kind == dr_dag_node_kind_create_task 
	  && x->child) {
	dr_dag_node_stack_push(s, x->child);
      }
    } else {
      dr_dag_node_stack_push_children(s, x);
    }
  }
  dr_dag_node_stack_fini(s);
  return n;
}


/* --- make position independent copy of g --- */

static void 
dr_pi_dag_init(dr_pi_dag * G) {
  G->n = 0;
  G->T = 0;
  G->m = 0;
  G->E = 0;
  G->S = 0;
}

static void
dr_pi_dag_enum_nodes(dr_pi_dag * G,
		     dr_dag_node * g, dr_clock_t start_clock,
		     dr_string_table * st) {
  dr_dag_node_stack s[1];
  long n = dr_dag_count_nodes(g);
  dr_pi_dag_node * T 
    = (dr_pi_dag_node *)dr_malloc(sizeof(dr_pi_dag_node) * n);
  dr_pi_dag_node * lim = T + n; 
  dr_pi_dag_node * p = T; /* allocation pointer */

  memset(T, 0, sizeof(dr_pi_dag_node) * n);

  dr_dag_node_stack_init(s);
  dr_pi_dag_copy_1(g, p, lim, st);
  p++;
  dr_dag_node_stack_push(s, g);
  while (s->top) {
    dr_dag_node * x = dr_dag_node_stack_pop(s);
    p = dr_pi_dag_copy_children(x, p, lim, start_clock, st);
    dr_dag_node_stack_push_children(s, x);
  }
  //dr_dag_node_stack_clear(s);
  G->n = n;
  G->T = T;
  dr_dag_node_stack_fini(s);
}

/* --------------------- enumurate edges ------------------- */

/* count the number of edges left uncollapsed in the memory */
static long
dr_pi_dag_count_edges_uncollapsed(dr_pi_dag * G) {
  long n_edges = 0;
  long i;
  for (i = 0; i < G->n; i++) {
    dr_pi_dag_node * u = &G->T[i];
    if (u->info.kind >= dr_dag_node_kind_section
	&& u->subgraphs_begin_offset < u->subgraphs_end_offset) {
      /* for each section or a task, count edges
	 between its direct children.
	 (1) task : each child's last node
	 -> the next child's first node
	 (2) section : the above +
	 each create_task -> the task's first insn
	 each task's last insn -> 
      */
      dr_pi_dag_node * ua = u + u->subgraphs_begin_offset;
      dr_pi_dag_node * ub = u + u->subgraphs_end_offset;
      dr_pi_dag_node * x;
      /* edges between u's i-th child to (i+1)-th child */
      n_edges += ub - ua - 1;
      if (u->info.kind == dr_dag_node_kind_section) {
	for (x = ua; x < ub; x++) {
	  if (x->info.kind == dr_dag_node_kind_create_task) {
	    n_edges += 2;
	  }
	}
      }
    }
  }
  return n_edges;
}

#if 0
/* count edges, including those that have been collapsed */
static long
dr_pi_dag_count_edges(dr_pi_dag * G) {
  long n_edges = 0;
  dr_dag_edge_kind_t k;
  for (k = 0; k < dr_dag_edge_kind_max; k++) {
    n_edges += G->T[0].info.edge_counts[k];
  }
  (void)dr_check(G->T[0].info.n_child_create_tasks == 0);
  return n_edges;
}
#endif

static void 
dr_pi_dag_add_edge(dr_pi_dag_edge * e, dr_pi_dag_edge * lim, 
		   dr_dag_edge_kind_t kind, long u, long v) {
  assert(e < lim);
  memset(e, 0, sizeof(dr_pi_dag_edge));
  e->kind = kind;
  e->u = u;
  e->v = v;
}

dr_pi_dag_node *
dr_pi_dag_node_first(dr_pi_dag_node * g, dr_pi_dag * G) {
  dr_pi_dag_node * lim = G->T + G->n;
  assert(g < lim);
  while (g->info.kind >= dr_dag_node_kind_section
	 && g->subgraphs_begin_offset < g->subgraphs_end_offset) {
    g = g + g->subgraphs_begin_offset;
    assert(g < lim);
  }
  return g;
}

dr_pi_dag_node *
dr_pi_dag_node_last(dr_pi_dag_node * g, dr_pi_dag * G) {
  dr_pi_dag_node * lim = G->T + G->n;
  assert(g < lim);
  while (g->info.kind >= dr_dag_node_kind_section 
	 && g->subgraphs_begin_offset < g->subgraphs_end_offset) {
    g = g + g->subgraphs_end_offset - 1;
    assert(g < lim);
  }
  return g;
}

/* count in G and list them. put the number of edges
   in G->m and array of edges in G->E */
static void 
dr_pi_dag_enum_edges(dr_pi_dag * G) {
  dr_pi_dag_node * T = G->T;
  long m = dr_pi_dag_count_edges_uncollapsed(G);
  dr_pi_dag_edge * E
    = (dr_pi_dag_edge *)dr_malloc(sizeof(dr_pi_dag_edge) * m); /* leaking */
  dr_pi_dag_edge * e = E;
  dr_pi_dag_edge * E_lim = E + m;
  long i;
  for (i = 0; i < G->n; i++) {
    dr_pi_dag_node * u = T + i;
    if (u->info.kind >= dr_dag_node_kind_section) {
      dr_pi_dag_node * ua = u + u->subgraphs_begin_offset;
      dr_pi_dag_node * ub = u + u->subgraphs_end_offset;
      dr_pi_dag_node * x;
      for (x = ua; x < ub - 1; x++) {
	/* x's last node -> x+1 */
	dr_pi_dag_node * s = dr_pi_dag_node_last(x, G);
	dr_pi_dag_node * t = dr_pi_dag_node_first(x + 1, G);
	assert(e < E_lim);
#if 1
	switch (t->info.in_edge_kind) {
	case dr_dag_edge_kind_create_cont:
	  dr_pi_dag_add_edge(e, E_lim, dr_dag_edge_kind_create_cont, 
			     s - T, t - T);
	  break;
	case dr_dag_edge_kind_end:
	case dr_dag_edge_kind_wait_cont:
	  /* t is a node that follows wait node */
	  dr_pi_dag_add_edge(e, E_lim, dr_dag_edge_kind_wait_cont, 
			     s - T, t - T);
	  break;
	default:
	  /* create_cont can't happen. */
	  (void)dr_check(t->info.in_edge_kind 
			 != dr_dag_edge_kind_create);
	  (void)dr_check(0);
	  break;
	}
#else
	switch (s->info.last_node_kind) {
	case dr_dag_node_kind_create_task:
	  dr_pi_dag_add_edge(e, E_lim, dr_dag_edge_kind_create_cont, s - T, t - T);
	  break;
	case dr_dag_node_kind_wait_tasks:
	  dr_pi_dag_add_edge(e, E_lim, dr_dag_edge_kind_wait_cont, s - T, t - T);
	  break;
	default:
	  (void)dr_check(0);
	}
#endif
	e++;
	if (x->info.kind == dr_dag_node_kind_section) {
	  dr_pi_dag_node * xa = x + x->subgraphs_begin_offset;
	  dr_pi_dag_node * xb = x + x->subgraphs_end_offset;
	  dr_pi_dag_node * y;
	  for (y = xa; y < xb; y++) {
	    if (y->info.kind == dr_dag_node_kind_create_task) {
	      (void)dr_check(y < xb - 1);
	      /* y's last node -> x+1 */
	      dr_pi_dag_node * c = y + y->child_offset;
	      dr_pi_dag_node * z = dr_pi_dag_node_first(c, G);
	      dr_pi_dag_node * w = dr_pi_dag_node_last(c, G);
	      assert(e < E_lim);
	      dr_pi_dag_add_edge(e, E_lim, dr_dag_edge_kind_create, y - T, z - T);
	      e++;
	      assert(e < E_lim);
	      dr_pi_dag_add_edge(e, E_lim, dr_dag_edge_kind_end, w - T, t - T);
	      e++;
	    }
	  }
	}
      }
    }
  }
  assert(e == E_lim);
  G->m = m;
  G->E = E;
}

static int edge_cmp(const void * e_, const void * f_) {
  const dr_pi_dag_edge * e = e_;
  const dr_pi_dag_edge * f = f_;
  if (e->u < f->u) return -1;
  if (e->u > f->u) return 1;
  if (e->v < f->v) return -1;
  if (e->v > f->v) return 1;
  return 0;
}

static void 
dr_pi_dag_sort_edges(dr_pi_dag * G) {
  dr_pi_dag_edge * E = G->E;
  long m = G->m;
  qsort((void *)E, m, sizeof(dr_pi_dag_edge), edge_cmp);
}

/* we have G->E. put edges_begin/edges_end 
   ptrs of G-T */
static void 
dr_pi_dag_set_edge_ptrs(dr_pi_dag * G) {
  dr_pi_dag_node * T = G->T;
  dr_pi_dag_edge * E = G->E;
  long m = G->m, n = G->n;
  long i = 0;
  long j;
  T[i].edges_begin = 0;
  for (j = 0; j < m; j++) {
    long u = E[j].u;
    while (i < u) {
      T[i].edges_end = j;
      T[i+1].edges_begin = j;
      i++;
    }
  }
  while (i < n - 1) {
    T[i].edges_end = m;
    T[i+1].edges_begin = m;
    i++;
  }
  T[n - 1].edges_end = m;
}

static void 
dr_pi_dag_set_string_table(dr_pi_dag * G, dr_string_table * st) {
  G->S = dr_string_table_flatten(st); /* G->S */
}

/* convert pointer-based dag structure (g)
   into a "position-independent" format (G)
   suitable for dumping into disk */
static void
dr_make_pi_dag(dr_pi_dag * G, dr_dag_node * g, 
	       dr_clock_t start_clock, int num_workers) {
  dr_string_table st[1];
  dr_string_table_init(st);
  G->num_workers = num_workers;
  dr_pi_dag_init(G);
  dr_pi_dag_enum_nodes(G, g, start_clock, st); /* G->T */
  dr_pi_dag_enum_edges(G);	/* G->E */
  dr_pi_dag_sort_edges(G);
  dr_pi_dag_set_edge_ptrs(G);
  dr_pi_dag_set_string_table(G, st); /* G->S */
  dr_string_table_destroy(st);
}

static void
dr_destroy_pi_dag(dr_pi_dag * G) {
  dr_free(G->T, G->n * sizeof(dr_pi_dag_node));
  dr_free(G->E, G->m * sizeof(dr_pi_dag_edge));
  dr_free(G->S, G->S->sz);	/* string table */
}



/* format of the dag file
   n 
   m 
   num_workers
   array of nodes
   array of edges
   number of strings
   string offset table
   flat string arrays
 */

static int 
dr_pi_dag_dump(dr_pi_dag * G, FILE * wp, 
	       const char * filename) {
  if (fwrite(&G->n, sizeof(G->n), 1, wp) != 1
      || fwrite(&G->m, sizeof(G->m), 1, wp) != 1
      || fwrite(&G->num_workers, sizeof(G->num_workers), 1, wp) != 1
      || (long)fwrite(G->T, sizeof(dr_pi_dag_node), G->n, wp) != G->n
      || (long)fwrite(G->E, sizeof(dr_pi_dag_edge), G->m, wp) != G->m
      || fwrite(G->S, G->S->sz, 1, wp) != 1) {
    fprintf(stderr, "fwrite: %s (%s)\n", 
	    strerror(errno), filename);
    return 0;
  } else {
    return 1;
  }
}

/* dump the position-independent dag into a file */
int dr_gen_pi_dag(dr_pi_dag * G) {
  FILE * wp = NULL;
  int must_close = 0;
  const char * filename = GS.opts.dag_file;
  if (filename && strcmp(filename, "") != 0) {
    if (strcmp(filename, "-") == 0) {
      fprintf(stderr, "writing dag to stdout\n");
      wp = stdout;
    } else {
      fprintf(stderr, "writing dag to %s\n", filename);
      wp = fopen(filename, "wb");
      if (!wp) { 
	fprintf(stderr, "fopen: %s (%s)\n", strerror(errno), filename); 
	return 0;
      }
      must_close = 1;
    }
  } else {
    fprintf(stderr, "not writing dag\n");
  }
  if (wp) {
    dr_pi_dag_dump(G, wp, filename);
  }
  if (must_close) fclose(wp);
  return 1;
}



/* initialization */


#if 0
static int 
getenv_bool(const char * v, char * y) {
  char * x = getenv(v);
  if (!x) return 0;
  if (strcasecmp(x, "true") == 0
      || strcasecmp(x, "yes") == 0) {
    *y = 1;
  } else {
    *y = atoi(x); 
  } 
  return 1;
} 
#endif

static int 
getenv_byte(const char * v, char * y) {
  char * x = getenv(v);
  if (!x) return 0;
  *y = atoi(x);
  return 1;
} 

static int 
getenv_int(const char * v, int * y) {
  char * x = getenv(v);
  if (!x) return 0;
  *y = atoi(x);
  return 1;
} 

static int 
getenv_long(const char * v, long * y) {
  char * x = getenv(v);
  if (!x) return 0;
  *y = atol(x);
  return 1;
} 

static int 
getenv_ull(const char * v, unsigned long long * y) {
  char * x = getenv(v);
  if (!x) return 0;
  long long z = atoll(x);
  *y = (unsigned long long)z;
  return 1;
} 

static int 
getenv_str(const char * v, const char ** y) {
  char * x = getenv(v);
  if (!x) return 0;
  *y = strdup(x);
  return 1;
} 

void dr_options_default(dr_options * opts) {
  * opts = dr_options_default_values;

  if (getenv_str("DAG_RECORDER_DAG_FILE",     &opts->dag_file)
      || getenv_str("DR_DAG",                 &opts->dag_file)) {}
  if (getenv_str("DAG_RECORDER_STAT_FILE",    &opts->stat_file)
      || getenv_str("DR_STAT",                &opts->stat_file)) {}
  if (getenv_str("DAG_RECORDER_GPL_FILE",     &opts->gpl_file)
      || getenv_str("DR_GPL",                 &opts->gpl_file)) {}
  if (getenv_str("DAG_RECORDER_DOT_FILE",     &opts->dot_file)
      || getenv_str("DR_DOT",                 &opts->dot_file)) {}
  if (getenv_str("DAG_RECORDER_TEXT_FILE",    &opts->text_file)
      || getenv_str("DR_TEXT",                &opts->text_file)) {}
  /* NOTE: we do not set sqlite_file via environment variables */
  if (getenv_int("DAG_RECORDER_GPL_SIZE",     &opts->gpl_sz)
      || getenv_int("DR_GPL_SZ",              &opts->gpl_sz)) {}
  if (getenv_str("DAG_RECORDER_TEXT_FILE_SEP",    &opts->text_file_sep)
      || getenv_str("DR_TEXT_SEP",                &opts->text_file_sep)) {}
  if (getenv_byte("DAG_RECORDER_DBG_LEVEL",   &opts->dbg_level)
      || getenv_byte("DR_DBG",                &opts->dbg_level)) {}
  if (getenv_byte("DAG_RECORDER_VERBOSE_LEVEL",  &opts->verbose_level)
      || getenv_byte("DR_VERBOSE",               &opts->verbose_level)) {}
  if (getenv_byte("DAG_RECORDER_CHK_LEVEL",   &opts->chk_level)
      || getenv_byte("DR_CHK",                &opts->chk_level)) {}
  if (getenv_ull("DAG_RECORDER_UNCOLLAPSE_MIN", &opts->uncollapse_min)
      || getenv_ull("DR_UNCOLLAPSE_MIN",        &opts->uncollapse_min)) {}
  if (getenv_ull("DAG_RECORDER_COLLAPSE_MAX", &opts->collapse_max)
      || getenv_ull("DR_COLLAPSE_MAX",        &opts->collapse_max)) {}
  if (getenv_long("DAG_RECORDER_NODE_COUNT",  &opts->node_count_target)
      || getenv_long("DR_NC",                 &opts->node_count_target)) {}
  if (getenv_long("DAG_RECORDER_PRUNE_THRESHOLD",  &opts->prune_threshold)
      || getenv_long("DR_PRUNE",              &opts->prune_threshold)) {}
  if (getenv_long("DAG_RECORDER_ALLOC_UNIT_MB", &opts->alloc_unit_mb)
      || getenv_long("DR_ALLOC_UNIT_MB",      &opts->alloc_unit_mb)) {}
  if (getenv_long("DAG_RECORDER_PRE_ALLOC",   &opts->pre_alloc)
      || getenv_long("DR_PRE_ALLOC",          &opts->pre_alloc)) {}
}

static void 
dr_opts_print(dr_options * opts) {
  if (!opts) opts = &GS.opts;
  FILE * wp = stderr;
  fprintf(wp, "DAG Recorder Options:\n");
  fprintf(wp, "dag_file (DAG_RECORDER_DAG_FILE,DR_DAG) : %s\n", 
	  opts->dag_file);
  fprintf(wp, "stat_file (DAG_RECORDER_STAT_FILE,DR_STAT) : %s\n", 
	  opts->stat_file);
  fprintf(wp, "gpl_file (DAG_RECORDER_GPL_FILE,DR_GPL) : %s\n", 
	  opts->gpl_file);
  fprintf(wp, "dot_file (DAG_RECORDER_DOT_FILE,DR_DOT) : %s\n", 
	  opts->dot_file);
  fprintf(wp, "text_file (DAG_RECORDER_TEXT_FILE,DR_TEXT) : %s\n", 
	  opts->text_file);
  fprintf(wp, "gpl_sz (DAG_RECORDER_GPL_SIZE,DR_GPL_SZ) : %d\n", 
	  opts->gpl_sz);
  fprintf(wp, "text_file (DAG_RECORDER_TEXT_FILE_SEP,DR_TEXT_SEP) : %s\n", 
	  opts->text_file_sep);
  fprintf(wp, "dbg_level (DAG_RECORDER_DBG_LEVEL,DR_DBG) : %d\n", 
	  opts->dbg_level);
  fprintf(wp, "verbose_level (DAG_RECORDER_VERBOSE_LEVEL,DR_VERBOSE) : %d\n", 
	  opts->verbose_level);
  fprintf(wp, "chk_level (DAG_RECORDER_CHK_LEVEL,DR_CHK) : %d\n", 
	  opts->chk_level);
  fprintf(wp, "uncollapse_min (DAG_RECORDER_UNCOLLAPSE_MIN,DR_UNCOLLAPSE_MIN) : %llu\n", 
	  opts->uncollapse_min);
  fprintf(wp, "collapse_max (DAG_RECORDER_COLLAPSE_MAX,DR_COLLAPSE_MAX) : %llu\n", 
	  opts->collapse_max);
  fprintf(wp, "node_count_target (DAG_RECORDER_NODE_COUNT,DR_NC) : %ld\n", 
	  opts->node_count_target);
  fprintf(wp, "prune_threshold (DAG_RECORDER_PRUNE_THRESHOLD,DR_PRUNE) : %ld\n", 
	  opts->prune_threshold);
  fprintf(wp, "alloc_unit_mb (DAG_RECORDER_ALLOC_UNIT_MB,DR_ALLOC_UNIT_MB) : %ld\n", 
	  opts->alloc_unit_mb);
  fprintf(wp, "pre_alloc (DAG_RECORDER_PRE_ALLOC,DR_PRE_ALLOC) : %ld\n", 
	  opts->pre_alloc);
}

static void 
dr_dag_node_freelist_init(dr_dag_node_freelist * fl) {
  fl->tail = fl->head = 0;
  fl->pages = 0;
}

/* procedures for non-recursive pruning */
static void 
dr_prune_nodes_stack_init(dr_prune_nodes_stack * S) {
  S->sz = 1000;
  S->entries 
    = (dr_prune_nodes_stack_ent*)
    dr_malloc(S->sz * sizeof(dr_prune_nodes_stack_ent));
  S->n = 0;
}

static void 
dr_prune_nodes_stack_destroy(dr_prune_nodes_stack * S) {
  dr_free(S->entries, S->sz * sizeof(dr_prune_nodes_stack_ent));
}

static dr_thread_specific_state *
dr_make_thread_specific_state(int num_workers) {
  dr_thread_specific_state * ts 
    = dr_malloc(sizeof(dr_thread_specific_state) 
		* num_workers);
  int i;
  long pages_total = GS.opts.pre_alloc;
  size_t alloc_sz = (GS.opts.alloc_unit_mb << 20);
  long pages_per_worker = (pages_total + num_workers - 1) / num_workers;

  for (i = 0; i < num_workers; i++) {
    long j;
    dr_dag_node_freelist * fl = ts[i].freelist;
    dr_dag_node_freelist_init(fl);
    for (j = 0; j < pages_per_worker; j++) {
      dr_dag_node_freelist_add_page(fl, alloc_sz);
    }
    dr_prune_nodes_stack_init(ts[i].prune_stack);
  }
  return ts;
}

static void
dr_free_thread_specific_state(int num_workers) {
  dr_free(GS.thread_specific, 
	  sizeof(dr_thread_specific_state) * num_workers);
  GS.thread_specific = 0;
  GS.ts = 0;
}

void
dr_opts_init(dr_options * opts) {
  dr_options opts_[1];
  if (!opts) {
    opts = opts_;
    dr_options_default(opts);
  }
  GS.opts = *opts;
}


/* initialize dag recorder, when called 
   for the first time.
   second or later invocations have no effects
 */
static void 
dr_init_(dr_options * opts, int num_workers) {
  if (!GS.initialized) {
    GS.initialized = 1;
    GS.num_workers = num_workers;
    const char * dag_recorder = getenv("DAG_RECORDER");
    if (!dag_recorder) dag_recorder = getenv("DR"); /* abbrev */
    if (!dag_recorder || atoi(dag_recorder)) {
      dr_opts_init(opts);
      dr_opts_print(opts);
      GS.thread_specific = dr_make_thread_specific_state(num_workers);
      GS.ts = 0;
    }  
  }
}

/* stop profiling */
void dr_stop__(const char * file, int line, int worker) {
  dr_end_task__(file, line, worker);
  GS.ts = 0;
}

/* --------------------- free dag ------------------- */

static void 
dr_free_freelist(dr_dag_node_freelist * fl) {
  dr_dag_node_page * head = fl->pages;
  dr_dag_node_page * page;
  dr_dag_node_page * next;
  for (page = head; page; page = next) {
    next = page->next;
    dr_free(page, page->sz);
  }
}

static void 
dr_free_freelists(int num_workers) {
  int i;
  for (i = 0; i < num_workers; i++) {
    dr_free_freelist(GS.thread_specific[i].freelist);
  }
}

static void
dr_destroy_prune_stacks(int num_workers) {
  int i;
  for (i = 0; i < num_workers; i++) {
    dr_prune_nodes_stack_destroy(GS.thread_specific[i].prune_stack);
  }
}

/* completely uninitialize */
void dr_cleanup__(const char * file, int line,
		  int worker, int num_workers) {
  if (GS.initialized) {
    if (GS.ts) dr_stop__(file, line, worker);
    /* get dag tree back into the free list of the calling worker */
    if (GS.thread_specific) {
      (void)dr_check(GS.root);
      dr_free_dag(GS.root, 1, GS.thread_specific[worker].freelist); 
      GS.root = 0;
      /* get everybody's freelists back into the underlying malloc */
      dr_free_freelists(num_workers);
      /* free thread specific data structure */
      dr_free_thread_specific_state(num_workers);
      /* destroy prune stacks */
      dr_destroy_prune_stacks(num_workers);
    }
    GS.initialized = 0;
  }
}

/* initialize when called for the first time;
   and start profiling */
void dr_start__(dr_options * opts, const char * file, int line,
		int worker, int num_workers) {
  dr_init_(opts, num_workers);
  if (GS.thread_specific) {
    if (GS.root) {
      dr_free_dag(GS.root, 1, GS.thread_specific[worker].freelist); 
    }
    GS.ts = GS.thread_specific;
    GS.start_clock = dr_get_tsc();
    dr_start_task__(0, file, line, worker);
    GS.root = dr_get_cur_task_(worker);
  }
}

void dr_dump() {
  if (GS.root) {
    dr_pi_dag G[1];
    dr_make_pi_dag(G, GS.root, GS.start_clock, GS.num_workers);
    dr_gen_pi_dag(G);
    dr_gen_basic_stat(G);
    dr_gen_gpl(G);
    dr_gen_dot(G);
    dr_gen_text(G);
    dr_destroy_pi_dag(G);
  }
}


