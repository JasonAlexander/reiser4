/*
 * Copyright 2001, 2002 by Hans Reiser, licensing governed by reiser4/README
 */

/*
 * functions for parser.y
 */






static void yy_exit(void);
static void yyerror( struct reiser4_syscall_w_space *ws, int msgnum , ...);
static int yywrap(void);
static void freeList(freeSpace_t * list);
static int reiser4_pars_free(struct reiser4_syscall_w_space * ws);
static freeSpace_t * freeSpaceAlloc(void);
static freeSpace_t * freeSpaceNextAlloc(struct reiser4_syscall_w_space * ws);
static char* list_alloc(struct reiser4_syscall_w_space * ws, int size);
static streg_t *alloc_new_level(struct reiser4_syscall_w_space * ws);
static vnode_t * alloc_vnode(struct reiser4_syscall_w_space * ws, vnode_t * last_vnode);
static lnode * get_lnode(struct reiser4_syscall_w_space * ws, struct inode * inode);
static struct reiser4_syscall_w_space * reiser4_pars_init(void);
static void level_up(struct reiser4_syscall_w_space *ws, int type);
static  void  level_down(struct reiser4_syscall_w_space * ws, int type1, int type2);
static void move_selected_word(struct reiser4_syscall_w_space * ws, int exclude );
static int b_check_word(struct reiser4_syscall_w_space * ws );
static __inline__ wrd_t * _wrd_inittab(struct reiser4_syscall_w_space * ws );
static int reiser4_lex( struct reiser4_syscall_w_space * ws );
static expr_v4_t * alloc_new_expr(struct reiser4_syscall_w_space * ws, int type);
wrd_t * nullname(struct reiser4_syscall_w_space * ws);
static expr_v4_t *  init_root(struct reiser4_syscall_w_space * ws);
static expr_v4_t *  init_pwd(struct reiser4_syscall_w_space * ws);
static expr_v4_t *  pars_expr(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static expr_v4_t *  lookup_word(struct reiser4_syscall_w_space * ws, wrd_t * w);
static inline expr_v4_t * pars_lookup_curr(struct reiser4_syscall_w_space * ws);
static inline expr_v4_t * pars_lookup_root(struct reiser4_syscall_w_space * ws);
static vnode_t *  lookup_vnode_word(struct reiser4_syscall_w_space * ws, vnode_t * vnode, wrd_t * w);
static expr_v4_t * make_do_it(struct reiser4_syscall_w_space * ws, expr_v4_t * e1 );
static expr_v4_t * if_then_else(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2 , expr_v4_t * e3  );
static expr_v4_t * if_then(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2 );
static void goto_end(struct reiser4_syscall_w_space * ws);
static expr_v4_t * constToExpr(struct reiser4_syscall_w_space * ws, wrd_t * e1 );
static expr_v4_t * connect_expression(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static expr_v4_t * compare_EQ_expression(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static expr_v4_t * compare_NE_expression(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static expr_v4_t * compare_LE_expression(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static expr_v4_t * compare_GE_expression(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static expr_v4_t * compare_LT_expression(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static expr_v4_t * compare_GT_expression(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static expr_v4_t * compare_OR_expression(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static expr_v4_t * compare_AND_expression(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static expr_v4_t * not_expression(struct reiser4_syscall_w_space * ws, expr_v4_t * e1);
static expr_v4_t * check_exist(struct reiser4_syscall_w_space * ws, expr_v4_t * e1);
static expr_v4_t * list_expression(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2 );
static inline expr_v4_t * list_async_expression(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2 );
static expr_v4_t * assign(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static expr_v4_t * assign_invert(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static expr_v4_t * symlink(struct reiser4_syscall_w_space * ws, expr_v4_t * e1, expr_v4_t * e2);
static  int source_not_empty(expr_v4_t *source);
static tube_t * get_tube_general(vnode_t *sink, expr_v4_t *source);
static size_t reserv_space_in_sink(tube_t * tube, size_t len );
static size_t get_available_len(struct file * fl);
static int prep_tube_general(tube_t * tube);
static int source_to_tube_general(tube_t * tube);
static int tube_to_sink_general(tube_t * tube);
static void put_tube(tube_t * tube);
static int pump( vnode_t *sink, expr_v4_t *source );