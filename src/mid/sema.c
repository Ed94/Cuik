#include "sema.h"
#include "settings.h"

#include <targets/targets.h>
#include <back/ir_gen.h>
#include <stdarg.h>

static _Atomic int sema_error_count;
static _Thread_local StmtIndex function_stmt;

// two simple temporary buffers to represent type_as_string results
static _Thread_local char temp_string0[256], temp_string1[256];

static TypeIndex sema_expr(TranslationUnit* tu, ExprIndex e);

#define sema_info(loc, fmt, ...) report(REPORT_INFO, &ir_gen_tokens.line_arena[loc], fmt, __VA_ARGS__)
#define sema_warn(loc, fmt, ...) report(REPORT_WARNING, &ir_gen_tokens.line_arena[loc], fmt, __VA_ARGS__)
#define sema_error(loc, fmt, ...) report(REPORT_ERROR, &ir_gen_tokens.line_arena[loc], fmt, __VA_ARGS__)

static size_t type_as_string(TranslationUnit* tu, size_t max_len, char buffer[static max_len], TypeIndex type_index) {
	Type* restrict type = &tu->types[type_index];
	
	size_t i = 0;
	switch (type->kind) {
		case KIND_VOID:  i += snprintf(&buffer[i], max_len - i, "void"); break;
		case KIND_BOOL:  i += snprintf(&buffer[i], max_len - i, "_Bool"); break;
		case KIND_CHAR:  i += snprintf(&buffer[i], max_len - i, "char"); break;
		case KIND_SHORT: i += snprintf(&buffer[i], max_len - i, "short"); break;
		case KIND_INT:   i += snprintf(&buffer[i], max_len - i, "int"); break;
		case KIND_LONG:  i += snprintf(&buffer[i], max_len - i, "long"); break;
		case KIND_FLOAT: i += snprintf(&buffer[i], max_len - i, "float"); break;
		case KIND_DOUBLE:i += snprintf(&buffer[i], max_len - i, "double"); break;
		case KIND_ENUM: {
			i += cstr_copy(max_len - i, &buffer[i], "enum ");
			
			if (type->enumerator.name) {
				i += cstr_copy(max_len - i, &buffer[i], (char*)type->enumerator.name);
			} else {
				i += cstr_copy(max_len - i, &buffer[i], "__unnamed__");
			}
			break;
		}
		case KIND_UNION: {
			i += cstr_copy(max_len - i, &buffer[i], "union ");
			
			if (type->record.name) {
				i += cstr_copy(max_len - i, &buffer[i], (char*)type->record.name);
			} else {
				i += cstr_copy(max_len - i, &buffer[i], "__unnamed__");
			}
			break;
		}
		case KIND_STRUCT: {
			i += cstr_copy(max_len - i, &buffer[i], "struct ");
			
			if (type->record.name) {
				i += cstr_copy(max_len - i, &buffer[i], (char*)type->record.name);
			} else {
				i += cstr_copy(max_len - i, &buffer[i], "__unnamed__");
			}
			break;
		}
		case KIND_PTR: {
			i += type_as_string(tu, max_len - i, &buffer[i], type->ptr_to);
			buffer[i++] = '*';
			break;
		}
		case KIND_ARRAY: {
			i += type_as_string(tu, max_len - i, &buffer[i], type->array_of);
			
			if (i+12 < max_len) {
				buffer[i++] = '[';
				
				i += sprintf_s(&buffer[i], max_len - i, "%zu", type->array_count);
				
				buffer[i++] = ']';
			} else {
				abort();
			}
			break;
		}
		case KIND_FUNC: {
			ParamIndex param_list = type->func.param_list;
			ParamIndex param_count = type->func.param_count;
			
			i += type_as_string(tu, max_len - i, &buffer[i], type->func.return_type);
			if (type->func.name) {
				buffer[i++] = ' ';
				i += cstr_copy(max_len - i, &buffer[i], (char*)type->func.name);
			}
			
			buffer[i++] = '(';
			for (size_t j = 0; j < param_count; j++) {
				if (j) buffer[i++] = ',';
				Param* p = &tu->params[param_list + j];
				
				i += type_as_string(tu, max_len - i, &buffer[i], p->type);
				if (p->name) {
					buffer[i++] = ' ';
					i += cstr_copy(max_len - i, &buffer[i], (char*)p->name);
				}
			}
			
			buffer[i++] = ')';
			break;
		}
		case KIND_TYPEOF: {
			// TODO(NeGate): give some nicer look to this crap
			i += cstr_copy(max_len - i, &buffer[i], "typeof(???)");
			break;
		}
		default: abort();
	}
	
	buffer[i] = '\0';
	return i;
}

static _Noreturn void sema_fatal(SourceLocIndex loc, const char* fmt, ...) {
	SourceLoc* l = &ir_gen_tokens.line_arena[loc];
	printf("%s:%d: error: ", l->file, l->line);
	
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	
	printf("\n");
	abort();
}

static bool is_scalar_type(TranslationUnit* tu, TypeIndex type_index) {
	Type* restrict type = &tu->types[type_index];
	return (type->kind >= KIND_BOOL && type->kind <= KIND_FUNC);
}

// Also checks if expression is an integer literal because we
// have a special case for 0 to pointer conversions.
static bool type_compatible(TranslationUnit* tu, TypeIndex a, TypeIndex b, ExprIndex a_expr) {
	if (a == b) return true;
	
	Type* restrict src = &tu->types[a];
	Type* restrict dst = &tu->types[b];
	
	// implictly convert arrays into pointers
	if (src->kind == KIND_ARRAY) {
		TypeIndex as_ptr = new_pointer(tu, src->array_of);
		src = &tu->types[as_ptr];
	}
	
	if (dst->kind == KIND_ARRAY) {
		TypeIndex as_ptr = new_pointer(tu, dst->array_of);
		dst = &tu->types[as_ptr];
	}
	
	if (src->kind != dst->kind) {
		if (src->kind >= KIND_BOOL &&
			src->kind <= KIND_LONG &&
			dst->kind >= KIND_BOOL && 
			dst->kind <= KIND_LONG) {
#if 0
			// we allow for implicit up-casts (char -> long)
			if (dst->kind >= src->kind) return true;
			else if (dst->kind == KIND_BOOL) return true;
			else if (tu->exprs[a_expr].op == EXPR_INT) {
				// allow integer literals to represent any integer
				return true;
			}
#else
			// just all integer casts are good
			return true;
#endif
		} else if (src->kind >= KIND_BOOL &&
				   src->kind <= KIND_LONG &&
				   dst->kind == KIND_PTR) {
			bool is_nullptr = tu->exprs[a_expr].op == EXPR_INT &&
				tu->exprs[a_expr].int_num.num == 0;
			
			if (is_nullptr) {
				return true;
			}
		} else if (src->kind == KIND_FLOAT ||
				   dst->kind == KIND_DOUBLE) {
			return true;
		} else if (src->kind == KIND_DOUBLE ||
				   dst->kind == KIND_FLOAT) {
			return true;
		} else if (src->kind == KIND_FUNC &&
				   dst->kind == KIND_PTR) {
			if (tu->types[dst->ptr_to].kind == KIND_FUNC) {
				return type_equal(tu, a, dst->ptr_to);
			}
		}
		
		return false;
	}
	
	if (src->kind == KIND_FUNC) {
		return type_equal(tu, a, b);
	} else if (dst->kind == KIND_PTR) {
		// void* -> T* is fine
		if (tu->types[src->ptr_to].kind == KIND_VOID) {
			return true;
		}
		
		// T* -> void* is fine
		if (tu->types[dst->ptr_to].kind == KIND_VOID) {
			return true;
		}
		
		return type_equal(tu, src->ptr_to, dst->ptr_to);
	}
	
	// but by default kind matching is enough
	// like for integers, booleans and floats
	return true;
}

static InitNode* walk_initializer_for_sema(TranslationUnit* tu, int node_count, InitNode* node) {
	for (int i = 0; i < node_count; i++) {
		if (node->kids_count == 0) {
			sema_expr(tu, node->expr);
			
			node += 1;
		} else {
			node = walk_initializer_for_sema(tu, node->kids_count, node + 1);
		}
	}
	
	return node;
}

static void try_resolve_typeof(TranslationUnit* tu, TypeIndex type) {
	Type* restrict ty = &tu->types[type];
	
	// TODO(NeGate): clean this up but essentially we just walk the 
	// types to ideally find a typeof with an expression
	while (ty->kind == KIND_PTR ||
		   ty->kind == KIND_ARRAY ||
		   ty->kind == KIND_UNION ||
		   ty->kind == KIND_STRUCT) {
		// this is soooooo nasty
		if (ty->kind == KIND_PTR) {
			ty = &tu->types[ty->ptr_to];
		} else if (ty->kind == KIND_ARRAY) {
			ty = &tu->types[ty->array_of];
		} else if (ty->kind == KIND_UNION || ty->kind == KIND_STRUCT) {
			MemberIndex start = ty->record.kids_start;
			MemberIndex end = ty->record.kids_end;
			
			for (MemberIndex m = start; m < end; m++) {
				try_resolve_typeof(tu, tu->members[m].type);
			}
		}
	}
	
	if (ty->kind == KIND_TYPEOF) {
		// spoopy...
		TypeIndex resolved = sema_expr(tu, ty->typeof_.src);
		*ty = tu->types[resolved];
	}
}

static TypeIndex sema_expr(TranslationUnit* tu, ExprIndex e) {
	Expr* restrict ep = &tu->exprs[e];
	
	switch (ep->op) {
		case EXPR_INT: {
			switch (ep->int_num.suffix) {
				case INT_SUFFIX_NONE: {
					unsigned int original = (unsigned int)ep->int_num.num;
					unsigned long long expected = (unsigned long long)ep->int_num.num;
					
					if (original != expected) {
						sema_error(ep->loc, "Could not represent integer literal as int. (%ulld or %ullx)", expected, expected);
					}
					
					return (ep->type = TYPE_INT);
				}
				
				case INT_SUFFIX_U: {
					unsigned int original = (unsigned int)ep->int_num.num;
					unsigned long long expected = (unsigned long long)ep->int_num.num;
					
					if (original != expected) {
						sema_error(ep->loc, "Could not represent integer literal as unsigned int.");
					}
					
					return (ep->type = TYPE_UINT);
				}
				
				case INT_SUFFIX_L:
				case INT_SUFFIX_LL:
				return (ep->type = TYPE_LONG);
				
				case INT_SUFFIX_UL:
				case INT_SUFFIX_ULL:
				return (ep->type = TYPE_ULONG);
				
				default:
				sema_error(ep->loc, "Could not represent integer literal.");
				return (ep->type = TYPE_VOID);
			}
		}
		case EXPR_ENUM: {
			return (ep->type = TYPE_INT);
		}
		case EXPR_FLOAT32: {
			return (ep->type = TYPE_FLOAT);
		}
		case EXPR_FLOAT64: {
			return (ep->type = TYPE_DOUBLE);
		}
		case EXPR_CHAR: {
			return (ep->type = TYPE_CHAR);
		}
		case EXPR_STR: {
			const char* start = (const char*)(ep->str.start + 1);
			const char* end = (const char*)(ep->str.end - 1);
			bool is_wide_string = ep->str.start[0] == 'L';
			
			return (ep->type = new_array(tu, is_wide_string ? TYPE_SHORT : TYPE_CHAR, (end-start) + 1));
		}
		case EXPR_SIZEOF: {
			TypeIndex src = sema_expr(tu, ep->x_of_expr.expr);
			
			*ep = (Expr) {
				.op = EXPR_INT,
				.type = TYPE_ULONG,
				.int_num = { tu->types[src].size, INT_SUFFIX_ULL }
			};
			return (ep->type = TYPE_ULONG);
		}
		case EXPR_ALIGNOF: {
			TypeIndex src = sema_expr(tu, ep->x_of_expr.expr);
			
			*ep = (Expr) {
				.op = EXPR_INT,
				.type = TYPE_ULONG,
				.int_num = { tu->types[src].align, INT_SUFFIX_ULL }
			};
			return (ep->type = TYPE_ULONG);
		}
		case EXPR_SIZEOF_T: {
			try_resolve_typeof(tu, ep->x_of_type.type);
			
			*ep = (Expr) {
				.op = EXPR_INT,
				.type = TYPE_ULONG,
				.int_num = { tu->types[ep->x_of_type.type].size, INT_SUFFIX_NONE }
			};
			return (ep->type = TYPE_ULONG);
		}
		case EXPR_ALIGNOF_T: {
			try_resolve_typeof(tu, ep->x_of_type.type);
			
			*ep = (Expr) {
				.op = EXPR_INT,
				.type = TYPE_ULONG,
				.int_num = { tu->types[ep->x_of_type.type].align, INT_SUFFIX_NONE }
			};
			return (ep->type = TYPE_ULONG);
		}
		case EXPR_INITIALIZER: {
			try_resolve_typeof(tu, ep->init.type);
			walk_initializer_for_sema(tu, ep->init.count, ep->init.nodes);
			
			return (ep->type = ep->init.type);
		}
		case EXPR_LOGICAL_NOT: {
			/* TypeIndex src = */ sema_expr(tu, ep->unary_op.src);
			return (ep->type = TYPE_BOOL);
		}
		case EXPR_NOT:
		case EXPR_NEGATE:
		case EXPR_PRE_INC:
		case EXPR_PRE_DEC:
		case EXPR_POST_INC:
		case EXPR_POST_DEC: {
			TypeIndex src = sema_expr(tu, ep->unary_op.src);
			
			return (ep->type = src);
		}
		case EXPR_ADDR: {
			TypeIndex src = sema_expr(tu, ep->unary_op.src);
			return (ep->type = new_pointer(tu, src));
		}
		case EXPR_SYMBOL: {
			StmtIndex stmt = ep->symbol;
			
			if (tu->stmts[stmt].op == STMT_LABEL) {
				return (ep->type = 0);
			} else {
				TypeIndex type = tu->stmts[stmt].decl.type;
				
				if (tu->types[type].kind == KIND_ARRAY) {
					// this is the only example where something sets it's own
					// cast_type it's an exception to the rules.
					ep->cast_type = new_pointer(tu, tu->types[type].array_of);
				}
				
				return (ep->type = type);
			}
		}
		case EXPR_PARAM: {
			int param_num = ep->param_num;
			
			Type* func_type = &tu->types[tu->stmts[function_stmt].decl.type];
			Param* params = &tu->params[func_type->func.param_list];
			return (ep->type = params[param_num].type);
		}
		case EXPR_CAST: {
			try_resolve_typeof(tu, ep->cast.type);
			
			/* TypeIndex src = */ sema_expr(tu, ep->cast.src);
			
			// set child's cast type
			tu->exprs[ep->cast.src].cast_type = ep->cast.type;
			return (ep->type = ep->cast.type);
		}
		case EXPR_SUBSCRIPT: {
			TypeIndex base = sema_expr(tu, ep->subscript.base);
			TypeIndex index = sema_expr(tu, ep->subscript.index);
			
			if (tu->types[index].kind == KIND_PTR ||
				tu->types[index].kind == KIND_ARRAY) {
				swap(base, index);
				swap(ep->subscript.base, ep->subscript.index);
			}
			
			if (tu->types[base].kind == KIND_ARRAY) {
				base = new_pointer(tu, tu->types[base].array_of);
			}
			
			return (ep->type = tu->types[base].ptr_to);
		}
		case EXPR_DEREF: {
			TypeIndex base = sema_expr(tu, ep->unary_op.src);
			Type* type = &tu->types[base];
			
			if (type->kind == KIND_PTR) {
				return (ep->type = type->ptr_to);
			} else if (type->kind == KIND_ARRAY) {
				return (ep->type = type->array_of);
			} else {
				sema_fatal(ep->loc, "TODO");
			}
		}
		case EXPR_CALL: {
			// Call function
			TypeIndex func_ptr = sema_expr(tu, ep->call.target);
			
			// implicit dereference
			if (tu->types[func_ptr].kind == KIND_PTR) {
				func_ptr = tu->types[func_ptr].ptr_to;
			}
			
			tu->exprs[ep->call.target].cast_type = func_ptr;
			
			Type* func_type = &tu->types[func_ptr];
			if (func_type->kind != KIND_FUNC) {
				sema_error(ep->loc, "function call target must be a function-type.");
				goto failure;
			}
			
			ExprIndex* args = ep->call.param_start;
			int arg_count = ep->call.param_count;
			
			Param* params = &tu->params[func_type->func.param_list];
			int param_count = func_type->func.param_count;
			
			if (func_type->func.has_varargs) {
				if (arg_count < param_count) {
					sema_error(ep->loc, "Not enough arguments (expected at least %d, got %d)", param_count, arg_count);
					goto failure;
				}
				
				// type-check the parameters with a known type
				for (size_t i = 0; i < param_count; i++) {
					TypeIndex arg_type = sema_expr(tu, args[i]);
					
					// TODO(NeGate): Error messages
					if (!type_compatible(tu, arg_type, params[i].type, args[i])) {
						type_as_string(tu, sizeof(temp_string0), temp_string0, arg_type);
						type_as_string(tu, sizeof(temp_string1), temp_string1, params[i].type);
						
						SourceLocIndex loc = tu->exprs[args[i]].loc;
						sema_error(loc, "Could not implicitly convert type %s into %s.", temp_string0, temp_string1);
						goto failure;
					}
					
					tu->exprs[args[i]].cast_type = params[i].type;
				}
				
				// type-check the untyped arguments
				for (size_t i = param_count; i < arg_count; i++) {
					TypeIndex src = sema_expr(tu, args[i]);
					
					tu->exprs[args[i]].cast_type = src;
				}
			} else {
				if (arg_count != param_count) {
					sema_error(ep->loc, "Argument count mismatch (expected %d, got %d)", param_count, arg_count);
					goto failure;
				}
				
				for (size_t i = 0; i < arg_count; i++) {
					TypeIndex arg_type = sema_expr(tu, args[i]);
					
					// TODO(NeGate): Error messages
					if (!type_compatible(tu, arg_type, params[i].type, args[i])) {
						type_as_string(tu, sizeof(temp_string0), temp_string0, arg_type);
						type_as_string(tu, sizeof(temp_string1), temp_string1, params[i].type);
						
						SourceLocIndex loc = tu->exprs[args[i]].loc;
						sema_error(loc, "Could not implicitly convert type %s into %s.", temp_string0, temp_string1);
						goto failure;
					}
					
					tu->exprs[args[i]].cast_type = tu->exprs[args[i]].type;
				}
			}
			
			failure:
			return (ep->type = tu->types[func_ptr].func.return_type);
		}
		case EXPR_TERNARY: {
			TypeIndex cond_type = sema_expr(tu, ep->ternary_op.left);
			if (!is_scalar_type(tu, cond_type)) {
				type_as_string(tu, sizeof(temp_string0), temp_string0, cond_type);
				
				sema_error(ep->loc, "Could not convert type %s into boolean.", temp_string0);
			}
			tu->exprs[ep->ternary_op.left].cast_type = TYPE_BOOL;
			
			TypeIndex type = get_common_type(tu,
											 sema_expr(tu, ep->ternary_op.middle),
											 sema_expr(tu, ep->ternary_op.right));
			
			tu->exprs[ep->ternary_op.middle].cast_type = type;
			tu->exprs[ep->ternary_op.right].cast_type = type;
			
			return (ep->type = type);
		}
		case EXPR_COMMA: {
			sema_expr(tu, ep->bin_op.left);
			
			return (ep->type = sema_expr(tu, ep->bin_op.right));
		}
		case EXPR_DOT: {
			TypeIndex base_type = sema_expr(tu, ep->dot.base);
			Type* restrict record_type = &tu->types[base_type];
			
			// Implicit dereference
			if (record_type->kind == KIND_PTR) {
				record_type = &tu->types[record_type->ptr_to];
				
				if (settings.pedantic) {
					sema_error(ep->loc, "Implicit dereference is a non-standard extension (disable -P to allow it).");
					return (ep->type = TYPE_VOID);
				}
			}
			
			if (record_type->kind != KIND_STRUCT && record_type->kind != KIND_UNION) {
				sema_error(ep->loc, "Cannot get the member of a non-record type.");
				return (ep->type = TYPE_VOID);
			}
			
			Atom name = ep->dot.name;
			
			MemberIndex start = record_type->record.kids_start;
			MemberIndex end = record_type->record.kids_end;
			for (MemberIndex m = start; m < end; m++) {
				Member* member = &tu->members[m];
				
				// TODO(NeGate): String interning would be nice
				if (cstr_equals(name, member->name)) {
					ep->dot.member = m;
					return (ep->type = member->type);
				}
			}
			
			sema_error(ep->loc, "Could not find member under that name.");
			return (ep->type = TYPE_VOID);
		}
		case EXPR_ARROW: {
			TypeIndex base_type = sema_expr(tu, ep->arrow.base);
			
			Type* restrict ptr_type = &tu->types[base_type];
			if (ptr_type->kind != KIND_PTR && ptr_type->kind != KIND_ARRAY) {
				sema_error(ep->loc, "Cannot do arrow operator on non-pointer type.");
				return (ep->type = TYPE_VOID);
			}
			
			Type* restrict record_type = &tu->types[ptr_type->ptr_to];
			if (record_type->kind != KIND_STRUCT && record_type->kind != KIND_UNION) {
				sema_error(ep->loc, "Cannot get the member of a non-record type.");
				return (ep->type = TYPE_VOID);
			}
			
			Atom name = ep->arrow.name;
			
			MemberIndex start = record_type->record.kids_start;
			MemberIndex end = record_type->record.kids_end;
			for (MemberIndex m = start; m < end; m++) {
				Member* member = &tu->members[m];
				
				// TODO(NeGate): String interning would be nice
				if (cstr_equals(name, member->name)) {
					ep->dot.member = m;
					return (ep->type = member->type);
				}
			}
			
			sema_error(ep->loc, "Could not find member under that name.");
			return (ep->type = TYPE_VOID);
		}
		case EXPR_LOGICAL_AND:
		case EXPR_LOGICAL_OR: {
			sema_expr(tu, ep->bin_op.left);
			sema_expr(tu, ep->bin_op.right);
			
			tu->exprs[ep->bin_op.left].cast_type = TYPE_BOOL;
			tu->exprs[ep->bin_op.right].cast_type = TYPE_BOOL;
			
			return (ep->type = TYPE_BOOL);
		}
		case EXPR_PLUS:
		case EXPR_MINUS:
		case EXPR_TIMES:
		case EXPR_SLASH:
		case EXPR_PERCENT:
		case EXPR_AND:
		case EXPR_OR:
		case EXPR_XOR:
		case EXPR_SHL:
		case EXPR_SHR: {
			TypeIndex lhs = sema_expr(tu, ep->bin_op.left);
			TypeIndex rhs = sema_expr(tu, ep->bin_op.right);
			
			if ((ep->op == EXPR_PLUS ||
				 ep->op == EXPR_MINUS) &&
				(tu->types[lhs].kind == KIND_PTR || 
				 tu->types[lhs].kind == KIND_ARRAY || 
				 tu->types[rhs].kind == KIND_PTR ||
				 tu->types[rhs].kind == KIND_ARRAY)) {
				// Pointer arithmatic
				if (ep->op == EXPR_PLUS && (tu->types[rhs].kind == KIND_PTR || tu->types[rhs].kind == KIND_ARRAY)) {
					swap(lhs, rhs);
					swap(ep->bin_op.left, ep->bin_op.right);
				}
				
				if (tu->types[rhs].kind == KIND_PTR || tu->types[rhs].kind == KIND_ARRAY) {
					if (ep->op == EXPR_MINUS) {
						// ptr - ptr = ptrdiff_t
						tu->exprs[ep->bin_op.left].cast_type = lhs;
						tu->exprs[ep->bin_op.right].cast_type = rhs;
						
						ep->op = EXPR_PTRDIFF;
						return (ep->type = TYPE_LONG);
					} else {
						sema_error(ep->loc, "Cannot do pointer addition with two pointer operands, one must be an integral type.");
						return (ep->type = TYPE_VOID);
					}
				} else {
					tu->exprs[ep->bin_op.left].cast_type = lhs;
					tu->exprs[ep->bin_op.right].cast_type = TYPE_ULONG;
					
					ep->op = (ep->op == EXPR_PLUS) ? EXPR_PTRADD : EXPR_PTRSUB;
					return (ep->type = lhs);
				}
			} else {
				if (!(tu->types[lhs].kind >= KIND_BOOL && 
					  tu->types[lhs].kind <= KIND_DOUBLE && 
					  tu->types[rhs].kind >= KIND_BOOL && 
					  tu->types[rhs].kind <= KIND_DOUBLE)) {
					type_as_string(tu, sizeof(temp_string0), temp_string0, lhs);
					type_as_string(tu, sizeof(temp_string1), temp_string1, rhs);
					
					sema_error(ep->loc, "Cannot apply binary operator to %s and %s.", temp_string0, temp_string1);
					return (ep->type = TYPE_VOID);
				}
				
				TypeIndex type = get_common_type(tu, lhs, rhs);
				tu->exprs[ep->bin_op.left].cast_type = type;
				tu->exprs[ep->bin_op.right].cast_type = type;
				
				return (ep->type = type);
			}
		}
		case EXPR_CMPEQ:
		case EXPR_CMPNE:
		case EXPR_CMPGT:
		case EXPR_CMPGE:
		case EXPR_CMPLT:
		case EXPR_CMPLE: {
			TypeIndex type = get_common_type(tu,
											 sema_expr(tu, ep->bin_op.left),
											 sema_expr(tu, ep->bin_op.right));
			
			tu->exprs[ep->bin_op.left].cast_type = type;
			tu->exprs[ep->bin_op.right].cast_type = type;
			
			return (ep->type = TYPE_BOOL);
		}
		case EXPR_PLUS_ASSIGN:
		case EXPR_MINUS_ASSIGN:
		case EXPR_ASSIGN:
		case EXPR_TIMES_ASSIGN:
		case EXPR_SLASH_ASSIGN:
		case EXPR_AND_ASSIGN:
		case EXPR_OR_ASSIGN:
		case EXPR_XOR_ASSIGN:
		case EXPR_SHL_ASSIGN:
		case EXPR_SHR_ASSIGN: {
			TypeIndex type = get_common_type(tu, 
											 sema_expr(tu, ep->bin_op.left),
											 sema_expr(tu, ep->bin_op.right));
			
			tu->exprs[ep->bin_op.left].cast_type = type;
			tu->exprs[ep->bin_op.right].cast_type = type;
			
			return (ep->type = type);
		}
		default:
		break;
	}
	
	abort();
}

void sema_stmt(TranslationUnit* tu, StmtIndex s) {
	Stmt* restrict sp = &tu->stmts[s];
	
	switch (sp->op) {
		case STMT_LABEL: {
			// hacky but we need to reserve the label
			TB_Function* func = tb_function_from_id(mod, tu->stmts[function_stmt].backing.f);
			sp->backing.l = tb_inst_new_label_id(func);
			break;
		}
		case STMT_GOTO: {
			sema_expr(tu, sp->goto_.target);
			break;
		}
		case STMT_COMPOUND: {
			StmtIndex* kids = sp->compound.kids;
			size_t count = sp->compound.kids_count;
			
			for (size_t i = 0; i < count; i++) {
				sema_stmt(tu, kids[i]);
			}
			break;
		}
		case STMT_DECL: {
			if (sp->decl.initial) {
				try_resolve_typeof(tu, sp->decl.type);
				
				TypeIndex expr_type = sema_expr(tu, sp->decl.initial);
				
				Expr* restrict ep = &tu->exprs[sp->decl.initial];
				if (ep->op == EXPR_INITIALIZER) {
					Type* restrict tp = &tu->types[sp->decl.type];
					
					// Auto-detect array count from initializer
					if (tp->kind == KIND_ARRAY && tp->array_count == 0) {
						tp->array_count = ep->init.count;
						tp->size = ep->init.count * tu->types[tp->array_of].size;
					}
					
					ep->init.type = expr_type = sp->decl.type;
				} else if (ep->op == EXPR_STR) {
					Type* restrict tp = &tu->types[sp->decl.type];
					
					// Auto-detect array count from string
					if (tp->kind == KIND_ARRAY && tp->array_count == 0) {
						sp->decl.type = expr_type;
					}
				}
				
				ep->cast_type = sp->decl.type;
				if (!type_compatible(tu, expr_type, sp->decl.type, sp->decl.initial)) {
					type_as_string(tu, sizeof(temp_string0), temp_string0, expr_type);
					type_as_string(tu, sizeof(temp_string1), temp_string1, sp->decl.type);
					
					sema_error(sp->loc, "Could not implicitly convert type %s into %s.", temp_string0, temp_string1);
				}
			}
			break;
		}
		case STMT_EXPR: {
			sema_expr(tu, sp->expr.expr);
			break;
		}
		case STMT_RETURN: {
			if (sp->return_.expr) {
				TypeIndex expr_type = sema_expr(tu, sp->return_.expr);
				TypeIndex return_type = tu->types[tu->stmts[function_stmt].decl.type].func.return_type;
				
				if (!type_compatible(tu, expr_type, return_type, sp->return_.expr)) {
					//sema_warn(sp->loc, "Value in return statement does not match function signature. (TODO this should be an error)");
				}
				
				tu->exprs[sp->return_.expr].cast_type = return_type;
			}
			break;
		}
		case STMT_IF: {
			TypeIndex cond_type = sema_expr(tu, sp->if_.cond);
			if (!is_scalar_type(tu, cond_type)) {
				type_as_string(tu, sizeof(temp_string0), temp_string0, cond_type);
				
				sema_error(sp->loc, "Could not convert type %s into boolean.", temp_string0);
			}
			tu->exprs[sp->if_.cond].cast_type = TYPE_BOOL;
			
			sema_stmt(tu, sp->if_.body);
			if (sp->if_.next) sema_stmt(tu, sp->if_.next);
			break;
		}
		case STMT_WHILE: {
			sema_expr(tu, sp->while_.cond);
			if (sp->while_.body) {
				sema_stmt(tu, sp->while_.body);
			}
			break;
		}
		case STMT_DO_WHILE: {
			if (sp->do_while.body) {
				sema_stmt(tu, sp->do_while.body);
			}
			sema_expr(tu, sp->do_while.cond);
			break;
		}
		case STMT_FOR: {
			if (sp->for_.first) {
				sema_stmt(tu, sp->for_.first);
			}
			
			if (sp->for_.cond) {
				sema_expr(tu, sp->for_.cond);
			}
			
			sema_stmt(tu, sp->for_.body);
			
			if (sp->for_.next) {
				sema_expr(tu, sp->for_.next);
			}
			break;
		}
		case STMT_SWITCH: {
			sema_expr(tu, sp->switch_.condition);
			sema_stmt(tu, sp->switch_.body);
			break;
		}
		case STMT_CASE: {
			sema_stmt(tu, sp->case_.body);
			break;
		}
		case STMT_DEFAULT: {
			sema_stmt(tu, sp->default_.body);
			break;
		}
		case STMT_CONTINUE: 
		case STMT_BREAK: {
			break;
		}
		default:
		assert(0);
	}
}

void sema_check(TranslationUnit* tu, StmtIndex s) {
	Stmt* restrict sp = &tu->stmts[s];
	
	TypeIndex type_index = sp->decl.type;
	Type* restrict type = &tu->types[type_index];
	
	char* name = (char*) sp->decl.name;
	switch (sp->op) {
		case STMT_FUNC_DECL: {
			assert(type->kind == KIND_FUNC);
			const Type* return_type = &tu->types[type->func.return_type];
			
			if (sp->decl.attrs.is_static && sp->decl.attrs.is_extern) {
				sema_error(sp->loc, "Function '%s' cannot be both static and extern.", name);
				sp->backing.f = 0;
				break;
			}
			
			if (sp->decl.attrs.is_static || sp->decl.attrs.is_inline) {
				if (!sp->decl.attrs.is_used) {
					//sema_warn(sp->loc, "Function '%s' is never used.", name);
					return;
				}
			}
			
			bool is_aggregate_return = false;
			if (return_type->kind == KIND_STRUCT ||
				return_type->kind == KIND_UNION) {
				is_aggregate_return = true;
			}
			
			// parameters
			ParamIndex param_list = type->func.param_list;
			ParamIndex param_count = type->func.param_count;
			
			// aggregate return values take up the first parameter slot.
			ParamIndex real_param_count = param_count + is_aggregate_return;
			
			TB_DataType return_dt = ctype_to_tbtype(return_type);
			TB_FunctionPrototype* proto = tb_prototype_create(mod, TB_STDCALL, return_dt, real_param_count, type->func.has_varargs);
			
			if (is_aggregate_return) {
				tb_prototype_add_param(proto, TB_TYPE_PTR);
			}
			
			for (size_t i = 0; i < param_count; i++) {
				Param* p = &tu->params[param_list + i];
				
				// Decide on the data type
				Type* param_type = &tu->types[p->type];
				TB_DataType dt = ctype_to_tbtype(param_type);
				
				assert(dt.width < 8);
				tb_prototype_add_param(proto, dt);
			}
			
			TB_Linkage linkage = sp->decl.attrs.is_static ? TB_LINKAGE_PRIVATE : TB_LINKAGE_PUBLIC;
			
			// TODO(NeGate): Fix this up because it's possibly wrong, essentially
			// inline linkage means all the definitions must match which isn't
			// necessarily the same as static where they all can share a name but
			// are different and internal.
			TB_Function* func;
			if (sp->decl.attrs.is_inline) {
				linkage = TB_LINKAGE_PRIVATE;
				
				char temp[1024];
				sprintf_s(temp, 1024, "%s@%d", name, s);
				
				func = tb_prototype_build(mod, proto, temp, linkage);
			} else {
				func = tb_prototype_build(mod, proto, name, linkage);
			}
			sp->backing.f = tb_function_get_id(mod, func);
			
			// type check function body
			function_stmt = s;
			sema_stmt(tu, (StmtIndex)tu->stmts[s].decl.initial);
			function_stmt = 0;
			break;
		}
		case STMT_DECL:
		case STMT_GLOBAL_DECL: {
			if (!sp->decl.attrs.is_used) break;
			
			if (sp->decl.attrs.is_static && sp->decl.attrs.is_extern) {
				sema_error(sp->loc, "Global declaration '%s' cannot be both static and extern.", name);
				sp->backing.g = 0;
				break;
			}
			
			if (sp->decl.attrs.is_extern || tu->types[sp->decl.type].kind == KIND_FUNC) {
				// forward decls
				// TODO(NeGate): This is hacky
				if (name[0] == '_') {
					ptrdiff_t search = shgeti(target_desc.builtin_func_map, name);
					
					// NOTE(NeGate): 0 doesn't mean a null index in this context, it
					// maps to the first external but i don't care we won't be using it
					// later on it's just clearer.
					if (search >= 0) {
						sp->backing.e = 0;
						break;
					}
				}
				
				sp->backing.e = tb_extern_create(mod, name);
			} else {
				if (type->align == 0) {
					sema_error(sp->loc, "Woah!!!");
				}
				
				TB_InitializerID init;
				if (sp->decl.initial && tu->exprs[sp->decl.initial].op == EXPR_INITIALIZER) {
					Expr* restrict ep = &tu->exprs[sp->decl.initial];
					
					int node_count = ep->init.count;
					InitNode* nodes = ep->init.nodes;
					
					// Walk initializer for max constant expression initializers.
					int max_tb_objects = 0;
					count_max_tb_init_objects(node_count, nodes, &max_tb_objects);
					
					init = tb_initializer_create(mod, type->size, type->align, max_tb_objects);
					
					// Initialize all const expressions
					// We don't support anything runtime in these expressions for now
					eval_initializer_objects(tu, NULL, sp->loc, init, TB_NULL_REG, type_index, node_count, nodes, 0);
					
					walk_initializer_for_sema(tu, ep->init.count, ep->init.nodes);
				} else {
					init = tb_initializer_create(mod, type->size, type->align, 0);
				}
				
				TB_Linkage linkage = sp->decl.attrs.is_static ? TB_LINKAGE_PRIVATE : TB_LINKAGE_PUBLIC;
				sp->backing.g = tb_global_create(mod, init, name, linkage);
				
				//report(REPORT_INFO, &ir_gen_tokens.line_arena[sp->loc], "Blah! %s %d", sp->decl.name, sp->backing.g);
			}
			break;
		}
		default: assert(0);
	}
}

static void sema_mark_children(TranslationUnit* tu, ExprIndex e) {
	Expr* restrict ep = &tu->exprs[e];
	if (ep->op == EXPR_ENUM) return;
	
	assert(ep->op == EXPR_SYMBOL);
	Stmt* restrict sp = &tu->stmts[ep->symbol];
	
	if (!sp->decl.attrs.is_used) {
		if (sp->op == STMT_FUNC_DECL) {
			sp->decl.attrs.is_used = true;
			ExprIndex sym = tu->stmts[(StmtIndex)sp->decl.initial].compound.first_symbol;
			
			while (sym) {
				sema_mark_children(tu, sym);
				sym = tu->exprs[sym].next_symbol_in_chain;
			}
		} else if (sp->op == STMT_DECL || sp->op == STMT_GLOBAL_DECL) {
			sp->decl.attrs.is_used = true;
		}
	}
}

void sema_remove_unused(TranslationUnit* tu) {
	// simple mark and sweep
	for (size_t s = 0, count = arrlen(tu->top_level_stmts); s < count; s++) {
		Stmt* restrict sp = &tu->stmts[tu->top_level_stmts[s]];
		
		if (sp->decl.attrs.is_root) {
			sp->decl.attrs.is_used = true;
			
			if (sp->op == STMT_FUNC_DECL) {
				ExprIndex sym = tu->stmts[(StmtIndex)sp->decl.initial].compound.first_symbol;
				while (sym) {
					sema_mark_children(tu, sym);
					sym = tu->exprs[sym].next_symbol_in_chain;
				}
			}
		}
	}
}