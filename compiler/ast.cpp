/*
  Copyright 2003 John Plevyak, All Rights Reserved, see COPYRIGHT file
*/

#include "geysa.h"

char *AST_name[] = {
#define S(_x) #_x,
#include "ast_kinds.h"
#undef S
};

AST *
AST::get(AST_kind k) {
  for (int i = 0; i < n; i++)
    if (v[i]->kind == k)
      return v[i];
  AST *res;
  for (int i = 0; i < n; i++)
    if ((res = v[i]->get(k)))
      return res;
  return NULL;
}

void 
AST::add(AST *a) { 
  if (a) { 
    if (!line) {
      pathname = a->pathname;
      line = a->line;
    }
    Vec<AST *>::add(a); 
  }
}

static void
dig_ast(AST *ast, D_ParseNode *pn) {
  if (pn->user.ast)
    ast->add(pn->user.ast);
  else
    for (int i = 0; i < d_get_number_of_children(pn); i++)
      dig_ast(ast, d_get_child(pn, i));
}

void
AST::add(D_ParseNode *pn) {
  dig_ast(this, pn);
} 

void
AST::set_location(D_ParseNode *pn) {
  scope_kind = pn->scope->kind;
  pathname = pn->start_loc.pathname;
  line = pn->start_loc.line;
}

AST *
AST::copy(PNodeMap *nmap) {
  AST *a = new AST(*this);
  if (nmap)
    for (int i = 0; i < a->pnodes.n; i++)
      a->pnodes.v[i] = nmap->get(a->pnodes.v[i]);
  for (int i = 0; i < a->n; i++)
    a->v[i] = a->v[i]->copy();
  return a;
}

AST::AST(AST_kind k, D_ParseNode *pn) {
  memset(this, 0, sizeof(*this)); // no virtuals here!
  kind = k;
  if (pn) {
    set_location(pn);
    add_below(pn);
  }
}

void
AST::add_below(D_ParseNode *pn) {
  for (int i = 0; i < d_get_number_of_children(pn); i++)
    dig_ast(this, d_get_child(pn, i));
}


Scope *
ast_qualified_ident_scope(AST *a) {
  int i = 0;
  Scope *s = a->scope;
  if (a->v[0]->kind == AST_global) {
    i = 1;
    s = s->global();
  }
  for (int x = i; x < a->n - 1; x++) {
    Sym *sym = s->get(a->v[x]->string);
    if (!sym || !sym->scope) {
      show_error("unresolved identifier qualifier '%s'", a, a->v[x]->string);
      return 0;
    }
    s = sym->scope;
  }
  return s;
}

Sym *
ast_qualified_ident_sym(AST *a) {
  Scope *s = ast_qualified_ident_scope(a);
  if (!s)
    return NULL;
  AST *id = ast_qualified_ident_ident(a);
  return s->get(id->string);
}

char *
ast_qualified_ident_string(AST *ast) {
  char *s = ast->v[0]->string;
  for (int i = 1; i < ast->n; i++) {
    char *ss = (char*)MALLOC(strlen(s) + strlen(ast->v[i]->string) + 3);
    strcpy(ss, s);
    strcat(ss,"::");
    strcat(ss, ast->v[i]->string);
    s = ss;
  }
  return s;
}

Sym *
checked_ast_qualified_ident_sym(AST *ast) {
  Sym *sym = ast_qualified_ident_sym(ast);
  if (!sym) { 
    show_error("unresolved identifier '%s'", ast, ast_qualified_ident_string(ast));
    return NULL;
  }
  ast->sym = sym;
  return sym;
}

static char *SPACES = "                                        "
                      "                                        ";
#define SP(_fp, _n) fputs(&SPACES[80-(_n)], _fp)
void
ast_print(FILE *fp, AST *a, int indent) {
  SP(fp, indent);
  fprintf(fp, "%s", AST_name[a->kind]);
  if (a->sym) {
    if (a->sym->constant)
      fprintf(fp, " constant %s", a->sym->constant);
    else if (a->sym->symbol)
      fprintf(fp, " symbol %s", a->sym->name);
    else if (a->sym->name)
      fprintf(fp, " sym %s", a->sym->name);
    else 
      fprintf(fp, " id(%d)", a->sym->id);
  }
  if (a->string)
    fprintf(fp, " %s", a->string);
  if (a->builtin)
    fprintf(fp, " builtin %s", a->builtin);
  fputs("\n", fp);
}

void
ast_print_recursive(FILE *fp, AST *a, int indent) {
  ast_print(fp, a, indent);
  forv_AST(aa, *a)
    ast_print_recursive(fp, aa, indent + 1);
}

void
ast_write(AST *a, char *filename) {
  FILE *fp = fopen(filename, "w");
  ast_print_recursive(fp, a);
  fclose(fp);
}

static Sym *
new_constant(IF1 *i, char *string, char *constant_type) {
  char *e = string + strlen(string);
  while (e > string && isspace(e[-1])) e--;
  char *s = string;
  if (s + 1 < e && s[0] == '#') {
    if (s[1] == '"') {
      s += 2;
      e--;
    } else
      s++;
    return if1_make_symbol(i, s, e);
  }
  return if1_const(i, if1_get_builtin(i, constant_type), dupstr(s, e));
}

Sym *
new_sym(IF1 *i, Scope *scope, char *s = 0, Sym *sym = 0) {
  if (!sym)
    sym = if1_alloc_sym(i, s);
  else
    assert(!sym->in);
  // unnamed temporaries are local to the module
  if (scope->in && scope->in->module && !s)
    sym->in = scope->in->init;
  else
    sym->in = scope->in;
  if (s)
    scope->put(s, sym);
  if (verbose_level)
    printf("new sym %X %s in %X\n", (int)sym, s, (int)scope);
  return sym;
}

static void
set_builtin(IF1 *i, Sym *sym, char *start, char *end = 0) {
  if1_set_builtin(i, sym, start, end);
  if (!end) end = start + strlen(start);
  int x = 0;
  for (; builtin_strings[x]; x++)
    if ((int)strlen(builtin_strings[x]) == (int)(end-start) && 
	!strncmp(builtin_strings[x], start, end-start))
      goto Lfound;
  fail("builtin not found '%s'", dupstr(start, end));
 Lfound:
  switch (x) {
#define S(_n) case Builtin_##_n: sym_##_n = sym; break;
#include "builtin_symbols.h"
#undef S
    default: assert(!"bad case");
  }
  switch (x) {
    default: break;
    case Builtin_int8:
    case Builtin_int16:
    case Builtin_int32:
    case Builtin_int64:
    case Builtin_uint8:
    case Builtin_uint16:
    case Builtin_uint32:
    case Builtin_uint64:
    case Builtin_float32:
    case Builtin_float64:
    case Builtin_float80:
    case Builtin_float128:
      sym->type_kind = Type_PRIMITIVE;
      break;
    case Builtin_tuple: sym->type_kind = Type_PRODUCT; break;
    case Builtin_vector: sym->type_kind = Type_VECTOR; break;
    case Builtin_ref: sym->type_kind = Type_REF; break;
    case Builtin_symbol: 
      if1_set_symbols_type(i); 
      sym->type_kind = Type_PRIMITIVE;
      break;
    case Builtin_continuation: 
      sym->type_kind = Type_PRIMITIVE;
      break;
  }
}

static void
build_builtin_syms(IF1 *i, AST *ast) {
  AST *ident;
  if (ast->builtin) {
    switch (ast->kind) {
      case AST_ident:
	ident = ast;
	goto Lok;
      case AST_in_module:
      case AST_def_type:
      case AST_def_fun:
      case AST_qualified_ident: {
	ident = ast->get(AST_ident);
      Lok:
	ast->sym = ident->sym = if1_alloc_sym(i, ident->string);
	set_builtin(i, ident->sym, ast->builtin);
	break;
      }
      case AST_const:
	ast->sym = new_constant(i, ast->string, ast->constant_type);
	set_builtin(i, ast->sym, ast->builtin);
	break;
      default: assert(!"bad case");
    }
  }
  forv_AST(a, *ast)
    build_builtin_syms(i, a);
}

static void
build_constant_syms(IF1 *i, AST *ast) {
  switch (ast->kind) {
    case AST_const:
      if (!ast->sym)
	ast->sym = new_constant(i, ast->string, ast->constant_type);
      break;
    default:
      forv_AST(a, *ast)
	build_constant_syms(i, a);
      break;
  }
}

static Type_kind
ast_to_type(AST *ast) {
  switch (ast->kind) {
    case AST_vector_type: return Type_VECTOR;
    case AST_ref_type: return Type_REF;
    case AST_product_type: return Type_PRODUCT;
    case AST_sum_type: return Type_SUM;
    case AST_fun_type: return Type_FUN;
    case AST_tagged_type: return Type_TAGGED;
    case AST_type_application: return Type_APPLICATION;
    case AST_record_type: return Type_RECORD;
    default: break;
  }
  assert(!"bad case");
  return Type_NONE;
}

static Sym * 
make_module(IF1 *i, char *mod_name, Scope *global, Sym *sym = 0) {
  sym = new_sym(i, global, mod_name, sym);
  sym->type = sym_module;
  sym->module = 1;
  sym->scope = new Scope(global, Scope_RECURSIVE, sym);
  if (sym != sym_system)
    sym->scope->dynamic.add(sym_system->scope);
  sym->labelmap = new LabelMap;

  Sym *fun = new_sym(i, sym->scope, "__init");
  fun->scope = new Scope(sym->scope, Scope_RECURSIVE, fun);
  fun->type = sym_function;
  sym->init = fun;
  return sym;
}

static Sym *
in_module(IF1 *i, char *mod_name, Scope *scope, Sym *sym = 0) {
  Scope *global = scope->global();
  Sym *s = global->hash.get(mod_name);
  if (!s || !s->scope) 
    s = make_module(i, mod_name, global, sym);
  assert(!sym || s == sym);
  return s;
}

/* define modules and types and defer functions
 */
static int
define_types(IF1 *i, AST *ast, Vec<AST *> &funs, Scope *scope, int skip = 0) {
  if (!skip) {
    ast->scope = scope;
    switch (ast->kind) {
      case AST_in_module: {
	ast->sym = in_module(i, ast->get(AST_ident)->string, scope, ast->sym);
	scope = ast->sym->scope;
	break;
      }
      case AST_def_type: {
	Sym *sym = scope->get(ast->get(AST_ident)->string);
	if (sym && sym->type_kind != Type_UNKNOWN)
	  return show_error("duplicate identifier '%s'", ast, ast->get(AST_ident)->string);
	if (!sym)
	  ast->sym = new_sym(i, scope, ast->get(AST_ident)->string, ast->sym);
	else {
	  assert(!ast->sym);
	  ast->sym = sym;
	}
	if (ast->sym->type_kind == Type_NONE || ast->sym->type_kind == Type_UNKNOWN) {
	  int i = 1;
	  for (; i < ast->n; i++) {
	    if (ast->v[i]->kind == AST_def_type_param ||
		ast->v[i]->kind == AST_constraint) 
	    {
	      // handled below
	    } else { 
	      ast->sym->type_kind = Type_ALIAS;
	      goto Lkind_assigned;
	    }
	  }
	  ast->sym->type_kind = Type_UNKNOWN;
	Lkind_assigned:;
	}
	scope = ast->sym->scope = new Scope(scope, Scope_RECURSIVE, ast->sym);
	if (verbose_level)
	  printf("creating scope %X for %s\n", (int)ast->sym->scope, ast->sym->name);
	{
	  AST *rtype = ast->get(AST_record_type);
	  if (rtype)
	    rtype->def_record_type = 1;
	}
	break;
      }
      case AST_def_fun: // defer functions
	funs.add(ast);
	return 0;
      case AST_declare_ident:
	ast->sym = new_sym(i, scope, ast->get(AST_ident)->string, ast->sym);
	break;
      case AST_vector_type:
      case AST_ref_type:
      case AST_product_type:
      case AST_sum_type:
      case AST_fun_type:
      case AST_tagged_type:
      case AST_type_application:
	ast->sym = new_sym(i, scope);
	ast->sym->type_kind = ast_to_type(ast);
	break;
      case AST_record_type:
      case AST_loop:
      case AST_with:
	ast->sym = new_sym(i, scope);
	if (ast->kind == AST_record_type) {
	  ast->sym->type_kind = Type_RECORD;
	  if (ast->def_record_type)
	    break;
	}
	scope = ast->sym->scope = new Scope(scope, Scope_RECURSIVE, scope->in);
	break;
      case AST_scope:
	scope = new Scope(scope, ast->scope_kind, scope->in);
	break;
      case AST_def_type_param:
	ast->sym = new_sym(i, scope, ast->get(AST_ident)->string);
	ast->sym->type_kind = Type_UNKNOWN;
	break;
      default:
	break;
    }
  }
  forv_AST(a, *ast) {
    if (define_types(i, a, funs, scope) < 0)
      return -1;
    if (a->kind == AST_in_module)
      scope = a->sym->scope;
  }
  return 0;
}

static int
scope_pattern(IF1 *i, AST *ast, Scope *scope) {
  switch (ast->kind) {
    case AST_pattern:
      ast->sym = new_sym(i, scope);
      ast->sym->type = sym_tuple;
      forv_AST(a, *ast) {
	if (scope_pattern(i, a, scope) < 0)
	  return -1;
	assert(a->sym);
	ast->sym->has.add(a->sym);
      }
      break;
    case AST_arg:
    case AST_vararg: {
      AST *c = ast->get(AST_const);
      AST *id = ast->get(AST_ident);
      AST *ty = ast->get(AST_qualified_ident);
      Sym *ty_sym = 0;
      if (ty) {
	ty->scope = scope;
	if (!(ty_sym = checked_ast_qualified_ident_sym(ty)))
	  return -1;
      }
      if (id)
	ast->sym = id->sym = new_sym(i, scope, id->string);
      else if (c)
	ast->sym = ast->v[0]->sym;
      else
	ast->sym = new_sym(i, scope);
      if (ast->kind == AST_vararg)
	ast->sym->vararg = 1;
      if (ty_sym)
	ast->sym->type = ty_sym;
      break;
    }
    default: break;
  }
  return 0;
}

static int
define_function(IF1 *i, AST *ast) {
  AST *fqid = ast->get(AST_qualified_ident);
  fqid->scope = ast->scope;
  Scope *fscope = ast_qualified_ident_scope(fqid);
  if (!fscope)
    return -1;
  AST *fid = ast_qualified_ident_ident(fqid);
  ast->sym = new_sym(i, fscope, fid->string, fqid->sym);
  ast->sym->scope = new Scope(ast->scope, Scope_RECURSIVE, ast->sym);
  if (fscope != ast->scope)
    ast->sym->scope->dynamic.add(fscope);
  for (int x = 1; x < ast->n - 1; x++)
    if (scope_pattern(i, ast->v[x], ast->sym->scope) < 0)
      return -1;
  ast->sym->cont = new_sym(i, ast->sym->scope);
  ast->sym->ret = new_sym(i, ast->sym->scope);
  ast->sym->labelmap = new LabelMap;
  ast->sym->ast = ast;
  return 0;
}

static int
resolve_parameterized_type(IF1 *i, AST *ast) {
  Sym *sym;
  if (!(sym = checked_ast_qualified_ident_sym(ast->get(AST_qualified_ident))))
    return -1;
  if (ast->n > 1) {
    Sym *s = new_sym(i, ast->scope);
    s->type_kind = Type_APPLICATION;
    s->has.add(sym);
    for (int i = 1; i < ast->n; i++) {
      assert(ast->v[i]->kind == AST_type_param && ast->v[i]->sym);
      if (!(sym = checked_ast_qualified_ident_sym(ast->v[i]->get(AST_qualified_ident))))
	return -1;
      s->args.add(sym);
    }
    ast->sym = s;
  } else
    ast->sym = sym;
  return 0;
}

static int
resolve_types_and_define_recursive_functions(IF1 *i, AST *ast, int skip = 0) {
  Sym *sym;
  if (!skip)
    switch (ast->kind) {
      case AST_arg: {
	AST *a = ast->get(AST_constraint);
	if (a)
	  if (resolve_parameterized_type(i, a) < 0)
	    return -1;
	break;
      }
      case AST_inherits:
      case AST_implements:
      case AST_includes:
	if (resolve_parameterized_type(i, ast) < 0)
	  return -1;
	break;
      case AST_def_type:
	sym = ast->sym;
	goto Lmore;
      case AST_where:
	if (!(sym = checked_ast_qualified_ident_sym(ast->get(AST_qualified_ident))))
	  return -1;
    Lmore:
	forv_AST(a, *ast) {
	  if (a->kind == AST_constraint) {
	    if (!(a->sym = ast_qualified_ident_sym(a->get(AST_qualified_ident))))
	      return -1;
	    sym->constraints.set_add(a->sym);
	    sym->scope->dynamic.add(a->sym->scope);
	  } if (a->kind == AST_def_type_param) {
	    sym->args.set_add(a->sym);
	    if (verbose_level)
	      printf("%s has param %s\n", sym->name, a->sym->name);
	  }
	}
	break;
      case AST_def_fun:
	if (ast->scope->kind == Scope_RECURSIVE) 
	  if (define_function(i, ast) < 0)
	    return -1;
	return 0; // defer
      default: break;
    }
  forv_AST(a, *ast)
    if (resolve_types_and_define_recursive_functions(i, a) < 0)
      return -1;
  return 0;
}

static int
variables_and_nonrecursive_functions(IF1 *i, AST *ast, int skip = 0) {
  if (!skip)
    switch (ast->kind) {
      case AST_def_ident: {
	AST *id = ast->get(AST_ident);
	ast->sym = new_sym(i, ast->scope, id->string, id->sym);
	ast->sym->ast = ast;
	break;
      }
      case AST_def_fun:
	if (ast->scope->kind != Scope_RECURSIVE) 
	  if (define_function(i, ast) < 0)
	    return -1;
	return 0;
      case AST_qualified_ident:
	if (!ast->sym)
	  if (!checked_ast_qualified_ident_sym(ast))
	    return -1;
	return 0;
      default: break;
    }
  forv_AST(a, *ast)
    if (variables_and_nonrecursive_functions(i, a) < 0)
      return -1;
  return 0;
}

static int
build_scopes(IF1 *i, AST *ast, Scope *scope) {
  Vec<AST *> funs;
  ast->scope = scope;
  funs.add(ast);
  while (1) {
    Vec<AST *> last_funs;
    last_funs.move(funs);
    forv_AST(a, last_funs) {
      int fun = a->kind == AST_def_fun;
      scope = fun ? a->sym->scope : a->scope;
      if (define_types(i, a, funs, scope, fun) < 0) 
	return -1;
      if (resolve_types_and_define_recursive_functions(i, a, fun) < 0) 
	return -1;
      if (variables_and_nonrecursive_functions(i, a, fun) < 0) 
	return -1;
    }
    if (!funs.n)
      break;
  }
  return 0;
}

static int
build_types(IF1 *i, AST *ast) {
  forv_AST(a, *ast)
    if (build_types(i, a) < 0)
      return -1;
  switch (ast->kind) {
    case AST_type_param:
      ast->sym = ast->v[0]->sym;
      break;
    case AST_vector_type:
    case AST_ref_type:
    case AST_product_type:
    case AST_fun_type:
    case AST_tagged_type:
    case AST_type_application:
    case AST_record_type:
      forv_AST(a, *ast)
	if (a->sym) {
	  switch (ast->kind) {
	    case AST_type_param:
	      ast->sym->args.add(a->sym);
	      break;
	    case AST_inherits:
	      ast->sym->implements.add(a->v[0]->sym);
	      ast->sym->includes.add(a->v[0]->sym);
	      break;
	    case AST_implements:
	      ast->sym->implements.add(a->v[0]->sym);
	      break;
	    case AST_includes:
	      ast->sym->includes.add(a->v[0]->sym);
	      break;
	    default:
	      ast->sym->has.add(a->sym);
	      break;
	  }
	}
      break;
    case AST_sum_type:
      forv_AST(a, *ast)
	if (a->sym)
	  a->sym->implements.set_add(ast->sym);
      break;
    case AST_def_type:
      for (int i = 1; i < ast->n; i++) {
	if (ast->v[i]->kind != AST_def_type_param && ast->v[i]->kind != AST_constraint) {
	  ast->sym->has.add(ast->v[i]->sym);
	  break;
	}
      }
      if (ast->sym->type_kind == Type_ALIAS && ast->sym->has.n && !ast->sym->has.v[0]->name)
	ast->sym->has.v[0]->name = ast->sym->name;
      break;
    default: break;
  }
  return 0;
}

static int
define_labels(IF1 *i, AST *ast, LabelMap *labelmap) {
  switch (ast->kind) {
    case AST_def_ident:
      if (ast->def_ident_label) {
	ast->label[0] = if1_alloc_label(i);
	ast->label[1] = if1_alloc_label(i);
	labelmap->put(ast->get(AST_ident)->string, ast);
      }
      break;
    case AST_in_module:
    case AST_def_fun:
      labelmap = ast->sym->labelmap;
      break;
    case AST_label:
      ast->label[0] = if1_alloc_label(i);
      ast->label[1] = ast->label[0];
      labelmap->put(ast->get(AST_ident)->string, ast);
      break;
    default: break;
  }
  forv_AST(a, *ast)
    if (define_labels(i, a, labelmap) < 0)
      return -1;
  return 0;
}

static int
resolve_labels(IF1 *i, AST *ast, LabelMap *labelmap,
	       Label *break_label = 0, Label *continue_label = 0, Label *return_label = 0) 
{
  AST *target;
  switch (ast->kind) {
    case AST_def_fun:
      labelmap = ast->sym->labelmap;
      return_label = ast->label[0] = if1_alloc_label(i);
      break;
    case AST_loop:
      continue_label = ast->label[0] = if1_alloc_label(i);
      break_label = ast->label[1] = if1_alloc_label(i);
      break;
    case AST_break:
      if ((target = ast->get(AST_ident))) {
	target = labelmap->get(target->string);
	ast->label[0] = target->label[1];
      } else
	ast->label[0] = break_label;
      break;
    case AST_continue:
      if ((target = ast->get(AST_ident))) {
	target = labelmap->get(target->string);
	ast->label[0] = target->label[0];
      } else
	ast->label[0] = continue_label;
      break;
    case AST_goto:
      target = labelmap->get(ast->get(AST_ident)->string);
      ast->label[0] = target->label[0];
    case AST_return:
      ast->label[0] = return_label;
      break;
    case AST_op:
      if (ast->v[ast->op_index]->sym->name[0] == ',')
	ast->v[0]->in_tuple = 1;
      if (ast->v[ast->op_index]->sym->name[0] == '^' && ast->v[ast->op_index]->sym->name[1] == '^')
	ast->v[0]->in_apply = 1;
      break;
    case AST_scope:
      forv_AST(a, *ast)
	a->constructor = ast->scope->kind == Scope_PARALLEL ? Make_VECTOR : Make_SET;
      break;
    default: break;
  }
  forv_AST(a, *ast)
    if (resolve_labels(i, a, labelmap, break_label, continue_label, return_label) < 0)
      return -1;
  return 0;
}

static void
gen_fun(IF1 *i, AST *ast) {
  Sym *fn = ast->sym;
  AST *expr = ast->last();
  Code *body = NULL, *c;
  if1_gen(i, &body, expr->code);
  if (expr->rval)
    c = if1_move(i, &body, expr->rval, fn->ret, ast);
  if1_label(i, &body, ast, ast->label[0]);
  c = if1_send(i, &body, 3, 0, sym_reply, fn->cont, fn->ret);
  c->ast = ast;
  int n = ast->n - 2;
  AST **args = &ast->v[1];
  Sym *as[n + 1];
  as[0] = if1_make_symbol(i, fn->name);
  for (int j = 0; j < n; j++)
    as[j + 1] = args[j]->rval;
  if1_closure(i, fn, body, n + 1, as);
  fn->type = sym_function;
  fn->ast = ast;
  ast->rval = new_sym(i, ast->scope);
  c = if1_move(i, &ast->code, fn, ast->rval, ast);
}

static int
get_tuple_args(IF1 *i, Code *send, AST *ast) {
  if (ast->kind == AST_op && ast->v[ast->op_index]->sym->name[0] == ',') {
    int r = get_tuple_args(i, send, ast->v[0]);
    if1_add_send_arg(i, send, ast->v[2]->rval);
    return 1 + r;
  }
  if1_add_send_arg(i, send, ast->rval);
  return 1;
}

static int
get_apply_args(IF1 *i, Code *send, AST *ast) {
  if (ast->kind == AST_op && (ast->v[ast->op_index]->sym->name[0] == '^' &&
			      ast->v[ast->op_index]->sym->name[1] == '^'))
  {
    int r = get_apply_args(i, send, ast->v[0]);
    if1_add_send_arg(i, send, ast->v[2]->rval);
    return 1 + r;
  }
  if1_add_send_arg(i, send, ast->rval);
  return 1;
}

static void
gen_op(IF1 *i, AST *ast) {
  char *op = ast->v[ast->op_index]->sym->name;
  int assign = op[1] == '=' && op[0] != '=';
  int ref = op[0] == '.' || (op[0] == '-' && op[1] == '>');
  int binary = ast->n > 2;
  Code **c = &ast->code;
  AST *a0 = ast->op_index ? ast->v[0] : 0, *a1 = ast->n > (int)(1 + ast->op_index) ? ast->last() : 0;
  ast->rval = new_sym(i, ast->scope);
  if (a0) if1_gen(i, c, a0->code);
  if (a1) if1_gen(i, c, a1->code);
  if (op[0] == ',') {
    if (ast->in_tuple)
      return;
    Code *send = if1_send1(i, c);
    send->ast = ast;
    Sym *constructor;
    switch (ast->constructor) {
      case Make_TUPLE: constructor = sym_make_tuple; break;
      case Make_VECTOR: constructor = sym_make_vector; break;
      case Make_SET: constructor = sym_make_set; break;
    }
    if1_add_send_arg(i, send, constructor);
    get_tuple_args(i, send, a0);
    if1_add_send_arg(i, send, a1->rval);
    if1_add_send_result(i, send, ast->rval);
  } else if (op[0] == '^' && op[1] == '^') {
    if (ast->in_apply)
      return;
    Code *send = if1_send1(i, c);
    send->ast = ast;
    get_apply_args(i, send, a0);
    if (a1)
      if1_add_send_arg(i, send, a1->rval);
    if1_add_send_result(i, send, ast->rval);
  } else {
    Sym *args = new_sym(i, ast->scope);
    Sym *res = ref ? new_sym(i, ast->scope) : ast->rval;
    Code *send;
    if (binary)
      send = if1_send(i, c, 4, 1, sym_make_tuple, a0->rval, ast->v[ast->op_index]->rval, a1->rval, args);
    else if (a0)
      send = if1_send(i, c, 3, 1, sym_make_tuple, a0->rval, ast->v[ast->op_index]->rval, args);
    else
      send = if1_send(i, c, 3, 1, sym_make_tuple, ast->v[ast->op_index]->rval, a1->rval, args);
    send->ast = ast;
    send = if1_send(i, c, 2, 1, sym_operator, args, res);
    send->ast = ast;
    if (ref) {
      send = if1_send(i, c, 3, 1, sym_primitive, sym_deref, res, ast->rval);
      send->ast = ast;
    }
    if (assign) {
      if (a0)
	send = if1_move(i, c, ast->rval, a0->lval, ast);
      else
	send = if1_move(i, c, ast->rval, a1->lval, ast);
    }
  }
}

static void
gen_loop(IF1 *i, AST *ast) {
  AST *cond = ast->get(AST_loop_cond);
  int dowhile = cond == ast->last();
  AST *body = dowhile ? ast->v[ast->n - 2] : ast->last();
  AST *before = ast->n > 2 ? ast->v[0] : 0;
  if1_loop(i, &ast->code, ast->label[0], ast->label[1],
	   cond->rval, before ? before->code : 0, 
	   cond->code, 0, body->code, ast);
  ast->rval = body->rval;
}

static void
gen_if(IF1 *i, AST *ast) {
  AST *ifcond = ast->v[0];
  AST *ifif = ast->v[1];
  AST *ifthen = ast->v[2];
  ast->rval = new_sym(i, ast->scope);
  if1_if(i, &ast->code, ifcond->code, ifcond->rval, ifif->code, ifif->rval,
	 ifthen ? ifthen->code:0, ifthen ? ifthen->rval:0, ast->rval, ast);
}

static void
gen_constructor(IF1 *i, AST *ast) {
  Sym *constructor;
  for (int x = 0; x < ast->n; x++)
    if1_gen(i, &ast->code, ast->v[x]->code);
  Code *send = if1_send1(i, &ast->code);
  send->ast = ast;
  ast->rval = new_sym(i, ast->scope);
  switch (ast->kind) {
    default: assert(!"bad case"); break;
    case AST_object: 
      if (!ast->n)
	constructor = sym_make_set;
      else
	constructor = sym_make_tuple; 
      break;
    case AST_list: 
      if (!ast->n)
	constructor = sym_make_tuple;
      else
	constructor = sym_make_list;
      break;
    case AST_vector: 
      constructor = sym_make_vector; 
      break;
  }
  if1_add_send_arg(i, send, constructor);
  for (int x = 0; x < ast->n; x++)
    if1_add_send_arg(i, send, ast->v[x]->rval);
  if1_add_send_result(i, send, ast->rval);
}

static int
gen_if1(IF1 *i, AST *ast) {
  // bottom's up
  forv_AST(a, *ast)
    if (gen_if1(i, a) < 0)
      return -1;
  switch (ast->kind) {
    case AST_def_fun: gen_fun(i, ast); break;
    case AST_def_ident: {
      if1_gen(i, &ast->code, ast->v[1]->code);
      if1_move(i, &ast->code, ast->v[1]->rval, ast->sym, ast); 
      ast->rval = ast->sym;
      break;
    }
    case AST_pattern: 
      ast->rval = ast->sym; 
      ast->rval->pattern = 1;
      break;
    case AST_const:
    case AST_arg: 
      ast->rval = ast->sym; break;
    case AST_list:
    case AST_vector:
    case AST_object:
      gen_constructor(i, ast); break;
    case AST_scope:
    case AST_block:
      for (int x = 0; x < ast->n; x++)
	if1_gen(i, &ast->code, ast->v[x]->code);
      if (ast->n)
	ast->rval = ast->last()->rval; 
      break;
    case AST_qualified_ident: 
      ast->lval = ast->rval = ast->sym; break;
    case AST_loop: gen_loop(i, ast); break;
    case AST_op: gen_op(i, ast); break;
    case AST_if: gen_if(i, ast); break;
    default: 
      if (ast->n == 1) {
	if (ast->code)
	  if1_gen(i, &ast->code, ast->v[0]->code);
	else
	  ast->code = ast->v[0]->code;
	ast->rval = ast->v[0]->rval;
      } else
	for (int x = 0; x < ast->n; x++)
	  if1_gen(i, &ast->code, ast->v[x]->code);
      break;
  }
  return 0;
}

static Sym *
collect_module_init(IF1 *i, AST *ast, Sym *mod) {
  forv_AST(a, *ast) 
    switch (a->kind) {
      case AST_in_module: 
	mod = a->sym->init; break;
      case AST_block: 
	forv_AST(a, *ast)
	  mod = collect_module_init(i, a, mod);
	break;
      default: 
	if1_gen(i, &mod->code, a->code);
	if (!mod->ast)
	  mod->ast = new AST(AST_block);
	mod->ast->add(a);
	mod->ret = a->rval;
	break;
    }
  return mod;
}


static int
build_functions(IF1 *i, AST *ast, Sym *mod) {
  if (define_labels(i, ast, mod->labelmap) < 0) return -1;
  if (resolve_labels(i, ast, mod->labelmap) < 0) return -1;
  if (gen_if1(i, ast) < 0) return -1;
  collect_module_init(i, ast, mod->init);
  return 0;
}

static void
build_modules(IF1 *i) {
  forv_Sym(s, i->allsyms) {
    if (s->module) {
      Sym *fun = s->init;
      fun->ret = sym_null;
      fun->cont = new_sym(i, fun->scope);
      Code *body = NULL;
      if1_gen(i, &body, fun->code);
      if1_send(i, &body, 3, 0, sym_reply, fun->cont, fun->ret);
      Sym *as = if1_make_symbol(i, fun->name);
      if1_closure(i, fun, body, 1, &as);
    }
  }
}

AST *
ast_qid(Sym *s) {
  AST *a = new AST(AST_qualified_ident);
  a->sym = s;
  return a;
}

AST *
ast_symbol(IF1 *i, char *s) {
  AST *a = new AST(AST_const);
  a->sym = if1_make_symbol(i, s);
  return a;
}

AST *
ast_call(IF1 *i, int n, ...) {
  assert(n > 0);
  va_list ap;
  va_start(ap, n);
  Sym *s = va_arg(ap, Sym *);
  AST *a = ast_qid(s);
  for (int x = 1; x < n; x++) {
    AST *aa = new AST(AST_op);
    aa->add(a);
    aa->add(ast_symbol(i, "^^"));
    aa->add(ast_qid(va_arg(ap, Sym *)));
    a = aa;
  }
  if (n == 1) {
    AST *aa = new AST(AST_op);
    aa->add(a);
    aa->add(ast_symbol(i, "^^"));
    a = aa;
  }
  va_end(ap);
  return a;
}

static void
build_init(IF1 *i) {
  Sym *fun = sym_init;
  fun->scope = new Scope(fun->ast->scope, Scope_RECURSIVE, fun);
  Sym *rval = 0;
  Code *body = 0;
  AST *ast = new AST(AST_block);
  forv_Sym(s, i->allsyms)
    if (s->module) {
      rval = new_sym(i, fun->scope);
      Code *send = if1_send(i, &body, 1, 1, s->init, rval);
      send->ast = ast_call(i, 1, s->init);
      ast->add(send->ast);
    }
  fun->cont = new_sym(i, fun->scope);
  fun->ret = sym_null;
  if1_send(i, &body, 3, 0, sym_reply, fun->cont, fun->ret);
  Sym *as = if1_make_symbol(i, fun->name);
  if1_closure(i, fun, body, 1, &as);
  fun->ast = ast;
}

static void
global_asserts() {
  assert(Scope_INHERIT == D_SCOPE_INHERIT && Scope_RECURSIVE == D_SCOPE_RECURSIVE);
  assert(Scope_PARALLEL == D_SCOPE_PARALLEL && Scope_SEQUENTIAL == D_SCOPE_SEQUENTIAL);
}

static void
finalize_types(IF1 *i) {
  // unalias
  forv_Sym(s, i->allsyms) {
    for (int x = 0; x < s->implements.n; x++)
      if (s->implements.v[x])
	s->implements.v[x] = unalias_type(s->implements.v[x]);
    for (int x = 0; x < s->constraints.n; x++)
      if (s->constraints.v[x])
	s->constraints.v[x] = unalias_type(s->constraints.v[x]);
  }
  forv_Sym(s, i->allsyms) {
    Sym *us = unalias_type(s);
    if (s != us) {
      forv_Sym(ss, s->implements) if (ss) {
	assert(ss != us);
	us->implements.set_add(ss);
      }
    }
  }
  // transitive closure of implements
  int changed = 1;
  while (changed) {
    changed = 0;
    forv_Sym(s, i->allsyms) {
      if (s->implements.n) {
	Vec<Sym *> supers(s->implements);
	forv_Sym(ss, supers) if (ss)
	  forv_Sym(sss, ss->implements) if (sss)
	    changed = s->implements.set_add(sss) || changed;
      }
    }
  }
  forv_Sym(s, i->allsyms)
    s->implements.set_to_vec();
}

int
ast_gen_if1(IF1 *i, Vec<AST *> &av) {
  Scope *global = new Scope();
  global_asserts();
  forv_AST(a, av)
    build_builtin_syms(i, a);
#define S(_n) assert(sym_##_n);
#include "builtin_symbols.h"
#undef S
  make_module(i, sym_system->name, global, sym_system);
  forv_AST(a, av)
    build_constant_syms(i, a);
  Sym *user_mod = in_module(i, if1_cannonicalize_string(i, "user"), global);
  Scope *scope = user_mod->scope;
  forv_AST(a, av)
    if (build_scopes(i, a, scope) < 0) 
      return -1;
  forv_AST(a, av)
    if (build_types(i, a) < 0) 
      return -1;
  finalize_types(i);
  forv_AST(a, av)
    if (build_functions(i, a, user_mod) < 0) 
      return -1;
  build_modules(i);
  build_init(i);
  return 0;
}
