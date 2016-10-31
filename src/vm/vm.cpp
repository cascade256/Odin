struct VirtualMachine;

struct vmValueProc {
	ssaProcedure *proc; // If `NULL`, use `ptr` instead and call external procedure
	void *        ptr;
};


struct vmValue {
	// NOTE(bill): Shouldn't need to store type here as the type checking
	// has already been handled in the SSA
	union {
		f32            val_f32;
		f64            val_f64;
		void *         val_ptr;
		i64            val_int;
		vmValueProc    val_proc;
		Array<vmValue> val_comp; // NOTE(bill): Will be freed through stack
	};
};

vmValue vm_make_value_ptr(void *ptr) {
	vmValue v = {};
	v.val_ptr = ptr;
	return v;
}

vmValue vm_make_value_int(i64 i) {
	vmValue v = {};
	v.val_int = i;
	return v;
}



struct vmFrame {
	VirtualMachine *  vm;
	vmFrame *         caller;
	ssaProcedure *    curr_proc;
	ssaBlock *        curr_block;
	isize             instr_index; // For the current block

	Map<vmValue>      values; // Key: ssaValue *
	gbTempArenaMemory temp_arena_memory;
	gbAllocator       stack_allocator;
	Array<void *>     locals; // Memory to locals
	vmValue           result;
};

struct VirtualMachine {
	ssaModule *         module;
	gbArena             stack_arena;
	gbAllocator         stack_allocator;
	gbAllocator         heap_allocator;
	Array<vmFrame>      frame_stack;
	Map<vmValue>        globals;             // Key: ssaValue *
	Map<vmValue>        const_compound_lits; // Key: ssaValue *
	vmValue             exit_value;
};

void    vm_exec_instr   (VirtualMachine *vm, ssaValue *value);
vmValue vm_operand_value(VirtualMachine *vm, ssaValue *value);
void    vm_store        (VirtualMachine *vm, void *dst, vmValue val, Type *type);

vmFrame *vm_back_frame(VirtualMachine *vm) {
	if (vm->frame_stack.count > 0) {
		return &vm->frame_stack[vm->frame_stack.count-1];
	}
	return NULL;
}

i64 vm_type_size_of(VirtualMachine *vm, Type *type) {
	return type_size_of(vm->module->sizes, vm->heap_allocator, type);
}
i64 vm_type_align_of(VirtualMachine *vm, Type *type) {
	return type_align_of(vm->module->sizes, vm->heap_allocator, type);
}
i64 vm_type_offset_of(VirtualMachine *vm, Type *type, i64 index) {
	return type_offset_of(vm->module->sizes, vm->heap_allocator, type, index);
}


void vm_init(VirtualMachine *vm, ssaModule *module) {
	gb_arena_init_from_allocator(&vm->stack_arena, heap_allocator(), gb_megabytes(64));

	vm->module = module;
	vm->stack_allocator = gb_arena_allocator(&vm->stack_arena);
	vm->heap_allocator = heap_allocator();
	array_init(&vm->frame_stack, vm->heap_allocator);
	map_init(&vm->globals, vm->heap_allocator);
	map_init(&vm->const_compound_lits, vm->heap_allocator);

	for_array(i, vm->module->values.entries) {
		ssaValue *v = vm->module->values.entries[i].value;
		switch (v->kind) {
		case ssaValue_Global: {
			Type *t = ssa_type(v);
			i64 size  = vm_type_size_of(vm, t);
			i64 align = vm_type_align_of(vm, t);
			void *mem = gb_alloc_align(vm->heap_allocator, size, align);
			vmValue init = vm_make_value_ptr(mem);
			if (v->Global.value != NULL && v->Global.value->kind == ssaValue_Constant) {
				vmValue *address = cast(vmValue *)init.val_ptr;
				vm_store(vm, address, vm_operand_value(vm, v->Global.value), type_deref(t));
			}
			map_set(&vm->globals, hash_pointer(v), init);
		} break;
		}
	}

}
void vm_destroy(VirtualMachine *vm) {
	array_free(&vm->frame_stack);
	map_destroy(&vm->globals);
	map_destroy(&vm->const_compound_lits);
	gb_arena_free(&vm->stack_arena);
}






void vm_set_value(vmFrame *f, ssaValue *v, vmValue val) {
	if (v != NULL) {
		map_set(&f->values, hash_pointer(v), val);
	}
}



vmFrame *vm_push_frame(VirtualMachine *vm, ssaProcedure *proc) {
	vmFrame frame = {};

	frame.vm          = vm;
	frame.curr_proc   = proc;
	frame.curr_block  = proc->blocks[0];
	frame.instr_index = 0;
	frame.caller      = vm_back_frame(vm);
	frame.stack_allocator   = vm->stack_allocator;
	frame.temp_arena_memory = gb_temp_arena_memory_begin(&vm->stack_arena);

	map_init(&frame.values, vm->heap_allocator);
	array_init(&frame.locals, vm->heap_allocator, proc->local_count);
	array_add(&vm->frame_stack, frame);
	return vm_back_frame(vm);
}

void vm_pop_frame(VirtualMachine *vm) {
	vmFrame *f = vm_back_frame(vm);

	gb_temp_arena_memory_end(f->temp_arena_memory);
	array_free(&f->locals);
	map_destroy(&f->values);

	array_pop(&vm->frame_stack);

}

vmValue vm_call_procedure(VirtualMachine *vm, ssaProcedure *proc, Array<vmValue> values) {
	Type *type = base_type(proc->type);
	GB_ASSERT_MSG(type->Proc.param_count == values.count,
	              "Incorrect number of arguments passed into procedure call!\n"
	              "%.*s -> %td vs %td",
	              LIT(proc->name),
	              type->Proc.param_count, values.count);


	vmValue result = {};

	if (proc->body == NULL) {
		GB_PANIC("TODO(bill): external procedure");
		return result;
	}
	gb_printf("call: %.*s\n", LIT(proc->name));

	vmFrame *f = vm_push_frame(vm, proc);
	for_array(i, proc->params) {
		vm_set_value(f, proc->params[i], values[i]);
	}

	while (f->curr_block != NULL) {
		ssaValue *curr_instr = f->curr_block->instrs[f->instr_index++];
		vm_exec_instr(vm, curr_instr);
	}

	Type *proc_type = base_type(proc->type);
	if (proc_type->Proc.result_count > 0) {
		result = f->result;

		Type *rt = base_type(proc_type->Proc.results);
		GB_ASSERT(is_type_tuple(rt));

		if (rt->Tuple.variable_count == 1) {
			rt = base_type(rt->Tuple.variables[0]->type);
		}

		if (is_type_string(rt)) {
			vmValue data  = result.val_comp[0];
			vmValue count = result.val_comp[1];
			gb_printf("String: %.*s\n", cast(isize)count.val_int, cast(u8 *)data.val_ptr);
		} else if (is_type_integer(rt)) {
			gb_printf("Integer: %lld\n", cast(i64)result.val_int);
		}
		// gb_printf("%lld\n", cast(i64)result.val_int);
	}

	vm_pop_frame(vm);
	return result;
}

vmValue vm_exact_value(VirtualMachine *vm, ssaValue *ptr, ExactValue value, Type *t) {
	vmValue result = {};
	Type *original_type = t;
	t = base_type(get_enum_base_type(t));
	// i64 size = vm_type_size_of(vm, t);
	if (is_type_boolean(t)) {
		result.val_int = value.value_bool != 0;
	} else if (is_type_integer(t)) {
		result.val_int = value.value_integer;
	} else if (is_type_float(t)) {
		if (t->Basic.kind == Basic_f32) {
			result.val_f32 = cast(f32)value.value_float;
		} else if (t->Basic.kind == Basic_f64) {
			result.val_f64 = cast(f64)value.value_float;
		}
	} else if (is_type_pointer(t)) {
		result.val_ptr = cast(void *)cast(intptr)value.value_pointer;
	} else if (is_type_string(t)) {
		array_init(&result.val_comp, vm->heap_allocator, 2);

		String str = value.value_string;
		i64 len = str.len;
		u8 *text = gb_alloc_array(vm->heap_allocator, u8, len);
		gb_memcopy(text, str.text, len);

		vmValue data = {};
		vmValue count = {};
		data.val_ptr = text;
		count.val_int = len;
		array_add(&result.val_comp, data);
		array_add(&result.val_comp, count);
	} else if (value.kind == ExactValue_Compound) {
		if (ptr != NULL) {
			vmValue *found = map_get(&vm->const_compound_lits, hash_pointer(ptr));
			if (found != NULL)  {
				return *found;
			}
		}

		ast_node(cl, CompoundLit, value.value_compound);

		if (is_type_array(t)) {
			vmValue result = {};

			isize elem_count = cl->elems.count;
			if (elem_count == 0) {
				if (ptr != NULL) {
					map_set(&vm->const_compound_lits, hash_pointer(ptr), result);
				}
				return result;
			}

			Type *type = base_type(t);
			array_init(&result.val_comp, vm->heap_allocator, type->Array.count);
			array_resize(&result.val_comp, type->Array.count);
			for (isize i = 0; i < elem_count; i++) {
				TypeAndValue *tav = type_and_value_of_expression(vm->module->info, cl->elems[i]);
				vmValue elem = vm_exact_value(vm, NULL, tav->value, tav->type);
				result.val_comp[i] = elem;
			}

			if (ptr != NULL) {
				map_set(&vm->const_compound_lits, hash_pointer(ptr), result);
			}

			return result;
		} else if (is_type_struct(t)) {
			ast_node(cl, CompoundLit, value.value_compound);

			if (cl->elems.count == 0) {
				return result;
			}

			isize value_count = t->Record.field_count;
			array_init(&result.val_comp, vm->heap_allocator, value_count);
			array_resize(&result.val_comp, value_count);

			if (cl->elems[0]->kind == AstNode_FieldValue) {
				isize elem_count = cl->elems.count;
				for (isize i = 0; i < elem_count; i++) {
					ast_node(fv, FieldValue, cl->elems[i]);
					String name = fv->field->Ident.string;

					TypeAndValue *tav = type_and_value_of_expression(vm->module->info, fv->value);
					GB_ASSERT(tav != NULL);

					Selection sel = lookup_field(vm->heap_allocator, t, name, false);
					Entity *f = t->Record.fields[sel.index[0]];

					result.val_comp[f->Variable.field_index] = vm_exact_value(vm, NULL, tav->value, f->type);
				}
			} else {
				for (isize i = 0; i < value_count; i++) {
					TypeAndValue *tav = type_and_value_of_expression(vm->module->info, cl->elems[i]);
					GB_ASSERT(tav != NULL);
					Entity *f = t->Record.fields_in_src_order[i];
					result.val_comp[f->Variable.field_index] = vm_exact_value(vm, NULL, tav->value, f->type);
				}
			}
		} else {
			GB_PANIC("TODO(bill): Other compound types\n");
		}

	} else if (value.kind == ExactValue_Invalid) {
		// NOTE(bill): "zero value"
	} else {
		gb_printf_err("TODO(bill): Other constant types: %s\n", type_to_string(original_type));
	}

	return result;
}


vmValue vm_operand_value(VirtualMachine *vm, ssaValue *value) {
	vmFrame *f = vm_back_frame(vm);
	vmValue v = {};
	switch (value->kind) {
	case ssaValue_Constant: {
		v = vm_exact_value(vm, value, value->Constant.value, value->Constant.type);
	} break;
	case ssaValue_ConstantSlice: {
		array_init(&v.val_comp, vm->heap_allocator, 3);

		auto *cs = &value->ConstantSlice;
		vmValue data = {};
		vmValue count = {};
		data = vm_operand_value(vm, cs->backing_array);
		count.val_int = cs->count;
		array_add(&v.val_comp, data);
		array_add(&v.val_comp, count);
		array_add(&v.val_comp, count);
	} break;
	case ssaValue_Nil:
		GB_PANIC("TODO(bill): ssaValue_Nil");
		break;
	case ssaValue_TypeName:
		GB_PANIC("TODO(bill): ssaValue_TypeName");
		break;
	case ssaValue_Global:
		v = *map_get(&vm->globals, hash_pointer(value));
		break;
	case ssaValue_Param:
		v = *map_get(&f->values, hash_pointer(value));
		break;
	case ssaValue_Proc: {
		v.val_proc.proc = &value->Proc;
		// GB_PANIC("TODO(bill): ssaValue_Proc");
	} break;
	case ssaValue_Block:
		GB_PANIC("TODO(bill): ssaValue_Block");
		break;
	case ssaValue_Instr: {
		vmValue *found = map_get(&f->values, hash_pointer(value));
		if (found) {
			v = *found;
		}
	} break;
	}

	return v;
}

void vm_store_integer(VirtualMachine *vm, void *dst, vmValue val, i64 store_bytes) {
	// TODO(bill): I assume little endian here
	GB_ASSERT(dst != NULL);
	gb_memcopy(dst, &val.val_int, store_bytes);
}

void vm_store(VirtualMachine *vm, void *dst, vmValue val, Type *type) {
	i64 size = vm_type_size_of(vm, type);
	type = base_type(get_enum_base_type(type));

	// TODO(bill): I assume little endian here

	switch (type->kind) {
	case Type_Basic:
		switch (type->Basic.kind) {
		case Basic_bool:
		case Basic_i8:
		case Basic_u8:
		case Basic_i16:
		case Basic_u16:
		case Basic_i32:
		case Basic_u32:
		case Basic_i64:
		case Basic_u64:
		case Basic_int:
		case Basic_uint:
			vm_store_integer(vm, dst, val, size);
			break;
		case Basic_f32:
			*cast(f32 *)dst = val.val_f32;
			break;
		case Basic_f64:
			*cast(f64 *)dst = val.val_f64;
			break;
		case Basic_rawptr:
			*cast(void **)dst = val.val_ptr;
			break;
		case Basic_string: {
			u8 *data  = cast(u8 *)val.val_comp[0].val_ptr;
			i64 word_size = vm_type_size_of(vm, t_int);

			u8 *mem = cast(u8 *)dst;
			gb_memcopy(mem, data, word_size);
			vm_store_integer(vm, mem+word_size, val.val_comp[1], word_size);
		} break;
		case Basic_any: {
			void *type_info = val.val_comp[0].val_ptr;
			void *data      = val.val_comp[1].val_ptr;
			i64 word_size = vm_type_size_of(vm, t_int);

			u8 *mem = cast(u8 *)dst;
			gb_memcopy(mem,           type_info, word_size);
			gb_memcopy(mem+word_size, data,      word_size);
		} break;
		default:
			gb_printf_err("TODO(bill): other basic types for `vm_store` %s\n", type_to_string(type));
			break;
		}
		break;

	case Type_Record: {
		if (is_type_struct(type)) {
			u8 *mem = cast(u8 *)dst;

			GB_ASSERT_MSG(type->Record.field_count >= val.val_comp.count,
			              "%td vs %td",
			              type->Record.field_count, val.val_comp.count);

			isize field_count = gb_min(val.val_comp.count, type->Record.field_count);

			for (isize i = 0; i < field_count; i++) {
				Entity *f = type->Record.fields[i];
				i64 offset = vm_type_offset_of(vm, type, i);
				void *ptr = mem+offset;
				vmValue member = val.val_comp[i];
				vm_store(vm, ptr, member, f->type);
			}

		} else {
			gb_printf_err("TODO(bill): records for `vm_store` %s\n", type_to_string(type));
		}
	} break;

	case Type_Array: {
		Type *elem_type = type->Array.elem;
		u8 *mem = cast(u8 *)dst;
		i64 elem_size = vm_type_size_of(vm, elem_type);
		i64 elem_count = gb_min(val.val_comp.count, type->Array.count);

		for (i64 i = 0; i < elem_count; i++) {
			void *ptr = mem + (elem_size*i);
			vmValue member = val.val_comp[i];
			*cast(i64 *)ptr = 123;
			vm_store(vm, ptr, member, elem_type);
		}
		gb_printf_err("%lld\n", *cast(i64 *)mem);

	} break;

	default:
		gb_printf_err("TODO(bill): other types for `vm_store` %s\n", type_to_string(type));
		break;
	}
}

vmValue vm_load_integer(VirtualMachine *vm, void *ptr, i64 store_bytes) {
	// TODO(bill): I assume little endian here
	vmValue v = {};
	// NOTE(bill): Only load the needed amount
	gb_memcopy(&v.val_int, ptr, store_bytes);
	return v;
}

vmValue vm_load(VirtualMachine *vm, void *ptr, Type *type) {
	i64 size = vm_type_size_of(vm, type);
	type = base_type(get_enum_base_type(type));

	vmValue result = {};

	switch (type->kind) {
	case Type_Basic:
		switch (type->Basic.kind) {
		case Basic_bool:
		case Basic_i8:
		case Basic_u8:
		case Basic_i16:
		case Basic_u16:
		case Basic_i32:
		case Basic_u32:
		case Basic_i64:
		case Basic_u64:
		case Basic_int:
		case Basic_uint:
			result = vm_load_integer(vm, ptr, size);
			break;
		case Basic_f32:
			result.val_f32 = *cast(f32 *)ptr;
			break;
		case Basic_f64:
			result.val_f64 = *cast(f64 *)ptr;
			break;
		case Basic_rawptr:
			result.val_ptr = *cast(void **)ptr;
			break;

		case Basic_string: {
			i64 word_size = vm_type_size_of(vm, t_int);
			u8 *mem = *cast(u8 **)ptr;
			array_init(&result.val_comp, vm->heap_allocator, 2);

			i64 count = 0;
			u8 *data = mem + 0*word_size;
			u8 *count_data = mem + 1*word_size;
			switch (word_size) {
			case 4: count = *cast(i32 *)count_data; break;
			case 8: count = *cast(i64 *)count_data; break;
			default: GB_PANIC("Unknown int size");  break;
			}

			array_add(&result.val_comp, vm_make_value_ptr(mem));
			array_add(&result.val_comp, vm_make_value_int(count));

		} break;

		default:
			GB_PANIC("TODO(bill): other basic types for `vm_load` %s", type_to_string(type));
			break;
		}
		break;

	case Type_Record: {
		if (is_type_struct(type)) {
			isize field_count = type->Record.field_count;

			array_init(&result.val_comp, vm->heap_allocator, field_count);
			array_resize(&result.val_comp, field_count);

			u8 *mem = cast(u8 *)ptr;
			for (isize i = 0; i < field_count; i++) {
				Entity *f = type->Record.fields[i];
				i64 offset = vm_type_offset_of(vm, type, i);
				vmValue val = vm_load(vm, mem+offset, f->type);
				result.val_comp[i] = val;
			}
		}
	} break;

	default:
		GB_PANIC("TODO(bill): other types for `vm_load` %s", type_to_string(type));
		break;
	}

	return result;
}

ssaProcedure *vm_lookup_procedure(VirtualMachine *vm, String name) {
	ssaValue *v = ssa_lookup_member(vm->module, name);
	GB_ASSERT(v->kind == ssaValue_Proc);
	ssaProcedure *proc = &v->Proc;
	return proc;
}

void vm_exec_instr(VirtualMachine *vm, ssaValue *value) {
	GB_ASSERT(value->kind == ssaValue_Instr);
	ssaInstr *instr = &value->Instr;
	vmFrame *f = vm_back_frame(vm);

#if 0
	if (instr->kind != ssaInstr_Comment) {
		gb_printf("exec_instr: %.*s\n", LIT(ssa_instr_strings[instr->kind]));
	}
#endif

	switch (instr->kind) {
	case ssaInstr_StartupRuntime: {
#if 0
		ssaProcedure *proc = vm_lookup_procedure(vm, make_string(SSA_STARTUP_RUNTIME_PROC_NAME));
		Array<vmValue> args = {}; // Empty
		vm_call_procedure(vm, proc, args); // NOTE(bill): No return value
#endif
	} break;

	case ssaInstr_Comment:
		break;

	case ssaInstr_Local: {
		Type *type = ssa_type(value);
		isize size  = gb_max(1, vm_type_size_of(vm, type));
		isize align = gb_max(1, vm_type_align_of(vm, type));
		void *memory = gb_alloc_align(vm->stack_allocator, size, align);
		GB_ASSERT(memory != NULL);
		vmValue v = vm_make_value_ptr(memory);
		vm_set_value(f, value, v);
		array_add(&f->locals, memory);
	} break;

	case ssaInstr_ZeroInit: {
		Type *t = type_deref(ssa_type(instr->ZeroInit.address));
		vmValue addr = vm_operand_value(vm, instr->ZeroInit.address);
		void *data = addr.val_ptr;
		i64 size = vm_type_size_of(vm, t);
		gb_zero_size(data, size);
	} break;

	case ssaInstr_Store: {
		vmValue addr = vm_operand_value(vm, instr->Store.address);
		vmValue val = vm_operand_value(vm, instr->Store.value);
		Type *t = ssa_type(instr->Store.value);
		vm_store(vm, addr.val_ptr, val, t);
	} break;

	case ssaInstr_Load: {
		vmValue addr = vm_operand_value(vm, instr->Load.address);
		vmValue v = vm_load(vm, addr.val_ptr, ssa_type(value));
		vm_set_value(f, value, v);
	} break;

	case ssaInstr_ArrayElementPtr: {
		vmValue address    = vm_operand_value(vm, instr->ArrayElementPtr.address);
		vmValue elem_index = vm_operand_value(vm, instr->ArrayElementPtr.elem_index);

		Type *t = ssa_type(instr->ArrayElementPtr.address);
		i64 elem_size = vm_type_size_of(vm, type_deref(t));
		void *ptr = cast(u8 *)address.val_ptr + elem_index.val_int*elem_size;
		vm_set_value(f, value, vm_make_value_ptr(ptr));
	} break;

	case ssaInstr_StructElementPtr: {
		vmValue address = vm_operand_value(vm, instr->StructElementPtr.address);
		i32 elem_index  = instr->StructElementPtr.elem_index;

		Type *t = ssa_type(instr->StructElementPtr.address);
		i64 offset = vm_type_offset_of(vm, type_deref(t), elem_index);
		void *ptr = cast(u8 *)address.val_ptr + offset;
		vm_set_value(f, value, vm_make_value_ptr(ptr));
	} break;

	case ssaInstr_PtrOffset: {
		Type *t = ssa_type(instr->PtrOffset.address);
		i64 elem_size = vm_type_size_of(vm, type_deref(t));
		vmValue address = vm_operand_value(vm, instr->PtrOffset.address);
		vmValue offset  = vm_operand_value(vm, instr->PtrOffset.offset);

		void *ptr = cast(u8 *)address.val_ptr + offset.val_int*elem_size;
		vm_set_value(f, value, vm_make_value_ptr(ptr));
	} break;

	case ssaInstr_Phi: {
		GB_PANIC("TODO(bill): ssaInstr_Phi");
	} break;

	case ssaInstr_ArrayExtractValue: {
		vmValue s = vm_operand_value(vm, instr->ArrayExtractValue.address);
		vmValue v = s.val_comp[instr->ArrayExtractValue.index];
		vm_set_value(f, value, v);
	} break;

	case ssaInstr_StructExtractValue: {
		vmValue s = vm_operand_value(vm, instr->StructExtractValue.address);
		vmValue v = s.val_comp[instr->StructExtractValue.index];
		vm_set_value(f, value, v);
	} break;

	case ssaInstr_Jump: {
		f->curr_block = instr->Jump.block;
		f->instr_index = 0;
	} break;

	case ssaInstr_If: {
		vmValue cond = vm_operand_value(vm, instr->If.cond);
		if (cond.val_int != 0) {
			f->curr_block = instr->If.true_block;
		} else {
			f->curr_block = instr->If.false_block;
		}
		f->instr_index = 0;
	} break;

	case ssaInstr_Return: {
		Type *return_type = NULL;
		vmValue result = {};

		if (instr->Return.value != NULL) {
			return_type = ssa_type(instr->Return.value);
			result = vm_operand_value(vm, instr->Return.value);
		}

		f->result = result;
		f->curr_block = NULL;
		return;
	} break;

	case ssaInstr_Conv: {
		// TODO(bill): Assuming little endian
		vmValue dst = {};
		vmValue src = vm_operand_value(vm, instr->Conv.value);
		i64 from_size = vm_type_size_of(vm, instr->Conv.from);
		i64 to_size   = vm_type_size_of(vm, instr->Conv.to);
		switch (instr->Conv.kind) {
		case ssaConv_trunc:
			gb_memcopy(&dst, &src, to_size);
			break;
		case ssaConv_zext:
			gb_memcopy(&dst, &src, from_size);
			break;
		case ssaConv_fptrunc: {
			GB_ASSERT(from_size > to_size);
			GB_ASSERT(base_type(instr->Conv.from) == t_f64);
			GB_ASSERT(base_type(instr->Conv.to) == t_f32);
			dst.val_f32 = cast(f32)src.val_f64;
		} break;
		case ssaConv_fpext: {
			GB_ASSERT(from_size < to_size);
			GB_ASSERT(base_type(instr->Conv.from) == t_f32);
			GB_ASSERT(base_type(instr->Conv.to) == t_f64);
			dst.val_f64 = cast(f64)src.val_f32;
		} break;
		case ssaConv_fptoui: {
			Type *from = base_type(instr->Conv.from);
			if (from == t_f64) {
				u64 u = cast(u64)src.val_f64;
				gb_memcopy(&dst, &u, to_size);
			} else {
				u64 u = cast(u64)src.val_f32;
				gb_memcopy(&dst, &u, to_size);
			}
		} break;
		case ssaConv_fptosi: {
			Type *from = base_type(instr->Conv.from);
			if (from == t_f64) {
				i64 i = cast(i64)src.val_f64;
				gb_memcopy(&dst, &i, to_size);
			} else {
				i64 i = cast(i64)src.val_f32;
				gb_memcopy(&dst, &i, to_size);
			}
		} break;
		case ssaConv_uitofp: {
			Type *to = base_type(instr->Conv.to);
			if (to == t_f64) {
				dst.val_f64 = cast(f64)cast(u64)src.val_int;
			} else {
				dst.val_f32 = cast(f32)cast(u64)src.val_int;
			}
		} break;
		case ssaConv_sitofp: {
			Type *to = base_type(instr->Conv.to);
			if (to == t_f64) {
				dst.val_f64 = cast(f64)cast(i64)src.val_int;
			} else {
				dst.val_f32 = cast(f32)cast(i64)src.val_int;
			}
		} break;

		case ssaConv_ptrtoint:
			dst.val_int = cast(i64)src.val_ptr;
			break;
		case ssaConv_inttoptr:
			dst.val_ptr = cast(void *)src.val_int;
			break;
		case ssaConv_bitcast:
			dst = src;
			break;
		}

		vm_set_value(f, value, dst);
	} break;

	case ssaInstr_Unreachable: {
		GB_PANIC("Unreachable");
	} break;

	case ssaInstr_BinaryOp: {
		auto *bo = &instr->BinaryOp;
		Type *t = base_type(ssa_type(bo->left));
		Type *et = t;
		while (et->kind == Type_Vector) {
			et = base_type(et->Vector.elem);
		}

		if (gb_is_between(bo->op, Token__ComparisonBegin+1, Token__ComparisonEnd-1)) {
			switch (bo->op) {
			case Token_CmpEq: break;
			case Token_NotEq: break;
			case Token_Lt:    break;
			case Token_Gt:    break;
			case Token_LtEq:  break;
			case Token_GtEq:  break;
			}
			gb_printf_err("TODO(bill): Comparison operations %.*s\n", LIT(token_strings[bo->op]));
			vm_set_value(f, value, vm_make_value_int(1)); // HACK(bill): always true
		} else {
			vmValue v = {};
			vmValue l = vm_operand_value(vm, bo->left);
			vmValue r = vm_operand_value(vm, bo->right);

			if (is_type_integer(t)) {
				switch (bo->op) {
				case Token_Add: v.val_int = l.val_int + r.val_int;  break;
				case Token_Sub: v.val_int = l.val_int - r.val_int;  break;
				case Token_And: v.val_int = l.val_int & r.val_int;  break;
				case Token_Or:  v.val_int = l.val_int | r.val_int;  break;
				case Token_Xor: v.val_int = l.val_int ^ r.val_int;  break;
				case Token_Shl: v.val_int = l.val_int << r.val_int; break;
				case Token_Shr: v.val_int = l.val_int >> r.val_int; break;
				case Token_Mul: v.val_int = l.val_int * r.val_int;  break;
				case Token_Not: v.val_int = l.val_int ^ r.val_int;  break;

				case Token_AndNot: v.val_int = l.val_int & (~r.val_int); break;

				// TODO(bill): Take into account size of integer and signedness
				case Token_Quo: GB_PANIC("TODO(bill): BinaryOp Integer Token_Quo"); break;
				case Token_Mod: GB_PANIC("TODO(bill): BinaryOp Integer Token_Mod"); break;

				}
			} else if (is_type_float(t)) {
				if (t == t_f32) {
					switch (bo->op) {
					case Token_Add: v.val_f32 = l.val_f32 + r.val_f32;  break;
					case Token_Sub: v.val_f32 = l.val_f32 - r.val_f32;  break;
					case Token_Mul: v.val_f32 = l.val_f32 * r.val_f32;  break;
					case Token_Quo: v.val_f32 = l.val_f32 / r.val_f32;  break;

					case Token_Mod: GB_PANIC("TODO(bill): BinaryOp f32 Token_Mod"); break;
					}
				} else if (t == t_f64) {
					switch (bo->op) {
					case Token_Add: v.val_f64 = l.val_f64 + r.val_f64;  break;
					case Token_Sub: v.val_f64 = l.val_f64 - r.val_f64;  break;
					case Token_Mul: v.val_f64 = l.val_f64 * r.val_f64;  break;
					case Token_Quo: v.val_f64 = l.val_f64 / r.val_f64;  break;

					case Token_Mod: GB_PANIC("TODO(bill): BinaryOp f64 Token_Mod"); break;
					}
				}
			} else {
				GB_PANIC("TODO(bill): Vector BinaryOp");
			}

			vm_set_value(f, value, v);
		}
	} break;

	case ssaInstr_Call: {
		Array<vmValue> args = {};
		array_init(&args, f->stack_allocator, instr->Call.arg_count);
		for (isize i = 0; i < instr->Call.arg_count; i++) {
			array_add(&args, vm_operand_value(vm, instr->Call.args[i]));
		}
		vmValue proc = vm_operand_value(vm, instr->Call.value);
		if (proc.val_proc.proc != NULL) {
			vmValue result = vm_call_procedure(vm, proc.val_proc.proc, args);
			vm_set_value(f, value, result);
		} else {
			GB_PANIC("TODO(bill): external procedure calls");
		}

	} break;

	case ssaInstr_Select: {
		vmValue v = {};
		vmValue cond = vm_operand_value(vm, instr->Select.cond);
		if (cond.val_int != 0) {
			v = vm_operand_value(vm, instr->Select.true_value);
		} else {
			v = vm_operand_value(vm, instr->Select.false_value);
		}

		vm_set_value(f, value, v);
	} break;

	case ssaInstr_VectorExtractElement: {

	} break;

	case ssaInstr_VectorInsertElement: {

	} break;

	case ssaInstr_VectorShuffle: {

	} break;

	case ssaInstr_BoundsCheck: {
		auto *bc = &instr->BoundsCheck;
		Array<vmValue> args = {};
		array_init(&args, vm->stack_allocator, 5);
		array_add(&args, vm_exact_value(vm, NULL, make_exact_value_string(bc->pos.file), t_string));
		array_add(&args, vm_exact_value(vm, NULL, make_exact_value_integer(bc->pos.line), t_int));
		array_add(&args, vm_exact_value(vm, NULL, make_exact_value_integer(bc->pos.column), t_int));
		array_add(&args, vm_operand_value(vm, bc->index));
		array_add(&args, vm_operand_value(vm, bc->len));

		ssaProcedure *proc = vm_lookup_procedure(vm, make_string("__bounds_check_error"));
		vm_call_procedure(vm, proc, args);
	} break;

	case ssaInstr_SliceBoundsCheck: {
		auto *bc = &instr->SliceBoundsCheck;
		Array<vmValue> args = {};
		ssaProcedure *proc;

		array_init(&args, vm->stack_allocator, 7);
		array_add(&args, vm_exact_value(vm, NULL, make_exact_value_string(bc->pos.file), t_string));
		array_add(&args, vm_exact_value(vm, NULL, make_exact_value_integer(bc->pos.line), t_int));
		array_add(&args, vm_exact_value(vm, NULL, make_exact_value_integer(bc->pos.column), t_int));
		array_add(&args, vm_operand_value(vm, bc->low));
		array_add(&args, vm_operand_value(vm, bc->high));
		if (!bc->is_substring) {
			array_add(&args, vm_operand_value(vm, bc->max));
			proc = vm_lookup_procedure(vm, make_string("__slice_expr_error"));
		} else {
			proc = vm_lookup_procedure(vm, make_string("__substring_expr_error"));
		}

		vm_call_procedure(vm, proc, args);
	} break;

	default: {
		GB_PANIC("<unknown instr> %d\n", instr->kind);
	} break;
	}
}