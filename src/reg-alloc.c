/**
 * Allocate registers from IR. The linear-scan algorithm now expects a minimum
 * of 7 available registers (typical for RISC-style architectures).
 *
 * TODO: Implement the "-O level" option. This allocator now always drops the
 * dead variable and does NOT wrtie it back to the stack.
 */

int check_live_out(basic_block_t *bb, var_t *var)
{
    int i;
    for (i = 0; i < bb->live_out_idx; i++)
        if (bb->live_out[i] == var)
            return 1;
    return 0;
}

void refresh(basic_block_t *bb, Inst_t *inst)
{
    int i;
    for (i = 0; i < REG_CNT; i++) {
        if (!REGS[i].var)
            continue;
        if (check_live_out(bb, REGS[i].var))
            continue;
        if (REGS[i].var->consumed < inst->idx) {
            REGS[i].var = NULL;
            REGS[i].polluted = 0;
        }
    }
}

ph2_ir_t *bb_add_ph2_ir(basic_block_t *bb, opcode_t op)
{
    ph2_ir_t *n = calloc(1, sizeof(ph2_ir_t));
    n->op = op;

    if (!bb->ph2_ir_list.head)
        bb->ph2_ir_list.head = n;
    else
        bb->ph2_ir_list.tail->next = n;

    bb->ph2_ir_list.tail = n;
    return n;
}

/**
 * Priority of spilling:
 * - live_out variable
 * - farthest local variable
 */
void spill_var(basic_block_t *bb, var_t *var, int idx)
{
    if (!REGS[idx].polluted) {
        REGS[idx].var = NULL;
        return;
    }

    if (!var->offset) {
        var->offset = bb->belong_to->func->stack_size;
        bb->belong_to->func->stack_size += 4;
    }
    ph2_ir_t *ir = var->is_global ? bb_add_ph2_ir(bb, OP_global_store)
                                  : bb_add_ph2_ir(bb, OP_store);
    ir->src0 = idx;
    ir->src1 = var->offset;
    REGS[idx].var = NULL;
    REGS[idx].polluted = 0;
}

void load_var(basic_block_t *bb, var_t *var, int idx)
{
    ph2_ir_t *ir = var->is_global ? bb_add_ph2_ir(bb, OP_global_load)
                                  : bb_add_ph2_ir(bb, OP_load);
    ir->src0 = var->offset;
    ir->dest = idx;
    REGS[idx].var = var;
    REGS[idx].polluted = 0;
}

int prepare_operand(basic_block_t *bb, var_t *var, int operand_0)
{
    int i;
    for (i = 0; i < REG_CNT; i++)
        if (REGS[i].var == var)
            return i;

    for (i = 0; i < REG_CNT; i++)
        if (!REGS[i].var) {
            load_var(bb, var, i);
            return i;
        }

    for (i = 0; i < REG_CNT; i++) {
        if (i == operand_0)
            continue;
        if (check_live_out(bb, REGS[i].var)) {
            spill_var(bb, REGS[i].var, i);
            load_var(bb, var, i);
            return i;
        }
    }

    /* spill farthest local */
    int spilled = 0;
    for (i = 0; i < REG_CNT; i++) {
        if (!REGS[i].var)
            continue;
        if (REGS[i].var->consumed > REGS[spilled].var->consumed)
            spilled = i;
    }

    spill_var(bb, REGS[spilled].var, spilled);
    load_var(bb, var, spilled);

    return spilled;
}

int prepare_dest(basic_block_t *bb, var_t *var, int operand_0, int operand_1)
{
    int i;
    for (i = 0; i < REG_CNT; i++)
        if (REGS[i].var == var) {
            REGS[i].polluted = 1;
            return i;
        }

    for (i = 0; i < REG_CNT; i++)
        if (!REGS[i].var) {
            REGS[i].var = var;
            REGS[i].polluted = 1;
            return i;
        }

    for (i = 0; i < REG_CNT; i++) {
        if (i == operand_0)
            continue;
        if (i == operand_1)
            continue;
        if (check_live_out(bb, REGS[i].var)) {
            spill_var(bb, REGS[i].var, i);
            REGS[i].var = var;
            REGS[i].polluted = 1;
            return i;
        }
    }

    /* spill farthest local */
    int spilled = 0;
    for (i = 0; i < REG_CNT; i++) {
        if (i == operand_0)
            continue;
        if (i == operand_1)
            continue;
        if (!REGS[i].var)
            continue;
        if (REGS[i].var->consumed > REGS[spilled].var->consumed)
            spilled = i;
    }

    spill_var(bb, REGS[spilled].var, spilled);
    REGS[spilled].var = var;
    REGS[spilled].polluted = 1;

    return spilled;
}

void spill_alive(basic_block_t *bb, Inst_t *inst)
{
    int i;
    for (i = 0; i < REG_CNT; i++) {
        if (!REGS[i].var)
            continue;
        if (check_live_out(bb, REGS[i].var)) {
            spill_var(bb, REGS[i].var, i);
            continue;
        }
        if (REGS[i].var->consumed > inst->idx) {
            spill_var(bb, REGS[i].var, i);
            continue;
        }
    }
}

void spill_live_out(basic_block_t *bb)
{
    int i;
    for (i = 0; i < REG_CNT; i++) {
        if (!REGS[i].var)
            continue;
        if (!check_live_out(bb, REGS[i].var)) {
            REGS[i].var = NULL;
            REGS[i].polluted = 0;
            continue;
        }
        if (!var_check_killed(REGS[i].var, bb)) {
            REGS[i].var = NULL;
            REGS[i].polluted = 0;
            continue;
        }
        spill_var(bb, REGS[i].var, i);
    }
}

/**
 * The operand of `OP_push` should not been killed until function called.
 */
void extend_liveness(basic_block_t *bb, Inst_t *inst, var_t *var, int offset)
{
    if (check_live_out(bb, var))
        return;
    if (inst->idx + offset > var->consumed)
        var->consumed = inst->idx + offset;
}

void reg_alloc()
{
    /* TODO: .bss and .data section */
    Inst_t *global_inst;
    for (global_inst = GLOBAL_FUNC.fn->bbs->inst_list.head; global_inst;
         global_inst = global_inst->next) {
        ph2_ir_t *ir;
        int dest, src0;
        int sz;

        switch (global_inst->opcode) {
        case OP_allocat:
            sz = global_inst->rd->is_ptr
                     ? PTR_SIZE
                     : find_type(global_inst->rd->type_name)->size;
            if (global_inst->rd->array_size) {
                GLOBAL_FUNC.stack_size += (global_inst->rd->array_size * sz);
                global_inst->rd->offset = GLOBAL_FUNC.stack_size;
                GLOBAL_FUNC.stack_size += PTR_SIZE;
                dest =
                    prepare_dest(GLOBAL_FUNC.fn->bbs, global_inst->rd, -1, -1);
                ir = bb_add_ph2_ir(GLOBAL_FUNC.fn->bbs, OP_global_address_of);
                ir->src0 = global_inst->rd->offset;
                ir->dest = dest;
                spill_var(GLOBAL_FUNC.fn->bbs, global_inst->rd, dest);
            } else {
                global_inst->rd->offset = GLOBAL_FUNC.stack_size;
                GLOBAL_FUNC.stack_size += sz;
            }
            break;
        case OP_load_constant:
            dest = prepare_dest(GLOBAL_FUNC.fn->bbs, global_inst->rd, -1, -1);
            ir = bb_add_ph2_ir(GLOBAL_FUNC.fn->bbs, OP_load_constant);
            ir->src0 = global_inst->rd->init_val;
            ir->dest = dest;
            break;
        case OP_assign:
            src0 = prepare_operand(GLOBAL_FUNC.fn->bbs, global_inst->rs1, -1);
            dest = prepare_dest(GLOBAL_FUNC.fn->bbs, global_inst->rd, src0, -1);
            ir = bb_add_ph2_ir(GLOBAL_FUNC.fn->bbs, OP_assign);
            ir->src0 = src0;
            ir->dest = dest;
            spill_var(GLOBAL_FUNC.fn->bbs, global_inst->rd, dest);
            break;
        default:
            printf("Unsupported global operation\n");
            abort();
        }
    }

    fn_t *fn;
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        if (!strcmp(fn->func->return_def.var_name, "main"))
            MAIN_BB = fn->bbs;

        int i;
        for (i = 0; i < REG_CNT; i++)
            REGS[i].var = NULL;

        /* set arguments available */
        for (i = 0; i < fn->func->num_params; i++) {
            REGS[i].var = fn->func->param_defs[i].subscripts[0];
            REGS[i].polluted = 1;
        }

        /* variadic function implementation */
        if (fn->func->va_args) {
            for (i = 0; i < MAX_PARAMS; i++) {
                ph2_ir_t *ir = bb_add_ph2_ir(fn->bbs, OP_store);

                if (i < fn->func->num_params)
                    fn->func->param_defs[i].subscripts[0]->offset =
                        fn->func->stack_size;

                ir->src0 = i;
                ir->src1 = fn->func->stack_size;
                fn->func->stack_size += 4;
            }
        }

        basic_block_t *bb;
        for (bb = fn->bbs; bb; bb = bb->rpo_next) {
            int is_pushing_args = 0, args = 0;

            Inst_t *inst;
            for (inst = bb->inst_list.head; inst; inst = inst->next) {
                ph2_ir_t *ir;
                int dest, src0, src1;
                int i, sz;

                refresh(bb, inst);

                switch (inst->opcode) {
                case OP_unwound_phi:
                    src0 = prepare_operand(bb, inst->rs1, -1);

                    if (!inst->rd->offset) {
                        inst->rd->offset = bb->belong_to->func->stack_size;
                        bb->belong_to->func->stack_size += 4;
                    }

                    ir = bb_add_ph2_ir(bb, OP_store);
                    ir->src0 = src0;
                    ir->src1 = inst->rd->offset;
                    break;
                case OP_allocat:
                    if ((!strcmp(inst->rd->type_name, "void") ||
                         !strcmp(inst->rd->type_name, "int") ||
                         !strcmp(inst->rd->type_name, "char")) &&
                        inst->rd->array_size == 0)
                        break;

                    src0 = fn->func->stack_size;

                    if (inst->rd->is_ptr)
                        sz = PTR_SIZE;
                    else
                        sz = find_type(inst->rd->type_name)->size;

                    if (inst->rd->array_size)
                        fn->func->stack_size += (inst->rd->array_size * sz);
                    else
                        fn->func->stack_size += sz;

                    inst->rd->offset = fn->func->stack_size;
                    fn->func->stack_size += PTR_SIZE;

                    dest = prepare_dest(bb, inst->rd, -1, -1);
                    ir = bb_add_ph2_ir(bb, OP_address_of);
                    ir->src0 = src0;
                    ir->dest = dest;
                    break;
                case OP_load_constant:
                case OP_load_data_address:
                    dest = prepare_dest(bb, inst->rd, -1, -1);
                    ir = bb_add_ph2_ir(bb, inst->opcode);
                    ir->src0 = inst->rd->init_val;
                    ir->dest = dest;
                    break;
                case OP_address_of:
                    /* make sure variable is on stack */
                    if (!inst->rs1->offset) {
                        inst->rs1->offset = bb->belong_to->func->stack_size;
                        bb->belong_to->func->stack_size += 4;

                        int i;
                        for (i = 0; i < REG_CNT; i++)
                            if (REGS[i].var == inst->rs1) {
                                ir = bb_add_ph2_ir(bb, OP_store);
                                ir->src0 = i;
                                ir->src1 = inst->rs1->offset;
                            }
                    }

                    dest = prepare_dest(bb, inst->rd, -1, -1);
                    ir = bb_add_ph2_ir(bb, OP_address_of);
                    ir->src0 = inst->rs1->offset;
                    ir->dest = dest;
                    break;
                case OP_assign:
                    src0 = prepare_operand(bb, inst->rs1, -1);
                    dest = prepare_dest(bb, inst->rd, ir->src0, -1);
                    ir = bb_add_ph2_ir(bb, inst->opcode);
                    ir->src0 = src0;
                    ir->dest = dest;

                    /* store global variable immediately after assignment */
                    if (inst->rd->is_global) {
                        ir = bb_add_ph2_ir(bb, OP_global_store);
                        ir->src0 = dest;
                        ir->src1 = inst->rd->offset;
                        REGS[dest].polluted = 0;
                    }
                    break;
                case OP_read:
                    src0 = prepare_operand(bb, inst->rs1, -1);
                    dest = prepare_dest(bb, inst->rd, src0, -1);
                    ir = bb_add_ph2_ir(bb, OP_read);
                    ir->src0 = src0;
                    ir->src1 = inst->sz;
                    ir->dest = dest;
                    break;
                case OP_write:
                    if (inst->rs2->is_func) {
                        src0 = prepare_operand(bb, inst->rs1, -1);
                        ir = bb_add_ph2_ir(bb, OP_address_of_func);
                        ir->src0 = src0;
                        strcpy(ir->func_name, inst->rs2->var_name);
                    } else {
                        /* FIXME: Avoid outdated content in register after
                         * storing, but causing some redundant spilling. */
                        spill_alive(bb, inst);
                        src0 = prepare_operand(bb, inst->rs1, -1);
                        src1 = prepare_operand(bb, inst->rs2, src0);
                        ir = bb_add_ph2_ir(bb, OP_write);
                        ir->src0 = src0;
                        ir->src1 = src1;
                        ir->dest = inst->sz;
                    }
                    break;
                case OP_branch:
                    src0 = prepare_operand(bb, inst->rs1, -1);

                    /**
                     * REGS[src0].var had been set to NULL, but the actual
                     * content is still holded in the register.
                     */
                    spill_live_out(bb);

                    ir = bb_add_ph2_ir(bb, OP_branch);
                    ir->src0 = src0;
                    ir->then_bb = bb->then_;
                    ir->else_bb = bb->else_;
                    break;
                case OP_push:
                    extend_liveness(bb, inst, inst->rs1, inst->sz);

                    if (!is_pushing_args) {
                        spill_alive(bb, inst);
                        is_pushing_args = 1;
                    }

                    src0 = prepare_operand(bb, inst->rs1, -1);
                    ir = bb_add_ph2_ir(bb, OP_assign);
                    ir->src0 = src0;
                    ir->dest = args++;
                    break;
                case OP_call:
                    if (!find_func(inst->str)->num_params)
                        spill_alive(bb, inst);

                    ir = bb_add_ph2_ir(bb, OP_call);
                    strcpy(ir->func_name, inst->str);

                    is_pushing_args = 0;
                    args = 0;

                    for (i = 0; i < REG_CNT; i++)
                        REGS[i].var = NULL;

                    break;
                case OP_indirect:
                    if (!args)
                        spill_alive(bb, inst);

                    src0 = prepare_operand(bb, inst->rs1, -1);
                    ir = bb_add_ph2_ir(bb, OP_load_func);
                    ir->src0 = src0;

                    bb_add_ph2_ir(bb, OP_indirect);

                    is_pushing_args = 0;
                    args = 0;
                    break;
                case OP_func_ret:
                    dest = prepare_dest(bb, inst->rd, -1, -1);
                    ir = bb_add_ph2_ir(bb, OP_assign);
                    ir->src0 = 0;
                    ir->dest = dest;
                    break;
                case OP_return:
                    if (inst->rs1)
                        src0 = prepare_operand(bb, inst->rs1, -1);
                    else
                        src0 = -1;

                    ir = bb_add_ph2_ir(bb, OP_return);
                    ir->src0 = src0;
                    break;
                case OP_add:
                case OP_sub:
                case OP_mul:
                case OP_div:
                case OP_mod:
                case OP_lshift:
                case OP_rshift:
                case OP_eq:
                case OP_neq:
                case OP_gt:
                case OP_geq:
                case OP_lt:
                case OP_leq:
                case OP_bit_and:
                case OP_bit_or:
                case OP_bit_xor:
                case OP_log_and:
                case OP_log_or:
                    src0 = prepare_operand(bb, inst->rs1, -1);
                    src1 = prepare_operand(bb, inst->rs2, ir->src0);
                    dest = prepare_dest(bb, inst->rd, ir->src0, ir->src1);
                    ir = bb_add_ph2_ir(bb, inst->opcode);
                    ir->src0 = src0;
                    ir->src1 = src1;
                    ir->dest = dest;
                    break;
                case OP_negate:
                case OP_bit_not:
                case OP_log_not:
                    src0 = prepare_operand(bb, inst->rs1, -1);
                    dest = prepare_dest(bb, inst->rd, ir->src0, -1);
                    ir = bb_add_ph2_ir(bb, inst->opcode);
                    ir->src0 = src0;
                    ir->dest = dest;
                    break;
                default:
                    printf("Unknown opcode\n");
                    abort();
                }
            }

            if (bb->next)
                spill_live_out(bb);

            if (bb == fn->exit)
                continue;

            /* append jump instruction for the normal block only */
            if (!bb->next)
                continue;

            if (bb->next == fn->exit)
                continue;

            /* jump to the beginning of loop or over the else block */
            if (bb->next->visited == fn->visited ||
                bb->next->visited != bb->rpo + 1) {
                ph2_ir_t *ir = bb_add_ph2_ir(bb, OP_jump);
                ir->next_bb = bb->next;
            }
        }

        /* handle implicit return */
        for (i = 0; i < MAX_BB_PRED; i++) {
            basic_block_t *bb = fn->exit->prev[i].bb;

            if (!bb)
                continue;

            if (strcmp(fn->func->return_def.type_name, "void"))
                continue;

            if (bb->inst_list.tail && bb->inst_list.tail->opcode == OP_return)
                continue;

            ph2_ir_t *ir = bb_add_ph2_ir(bb, OP_return);
            ir->src0 = -1;
        }
    }
}

void dump_ph2_ir()
{
    ph2_ir_t *ph2_ir;
    int i, rd, rs1, rs2;

    for (i = 0; i < ph2_ir_idx; i++) {
        ph2_ir = &PH2_IR[i];

        rd = ph2_ir->dest + 48;
        rs1 = ph2_ir->src0 + 48;
        rs2 = ph2_ir->src1 + 48;

        switch (ph2_ir->op) {
        case OP_define:
            printf("%s:", ph2_ir->func_name);
            break;
        case OP_block_start:
        case OP_block_end:
        case OP_allocat:
            continue;
        case OP_assign:
            printf("\t%%x%c = %%x%c", rd, rs1);
            break;
        case OP_load_constant:
            printf("\tli %%x%c, $%d", rd, ph2_ir->src0);
            break;
        case OP_load_data_address:
            printf("\t%%x%c = .data(%d)", rd, ph2_ir->src0);
            break;
        case OP_address_of:
            printf("\t%%x%c = %%sp + %d", rd, ph2_ir->src0);
            break;
        case OP_global_address_of:
            printf("\t%%x%c = %%gp + %d", rd, ph2_ir->src0);
            break;
        case OP_label:
            printf("%s:", ph2_ir->func_name);
            break;
        case OP_branch:
            printf("\tbr %%x%c, %s, %s", rs1, ph2_ir->true_label,
                   ph2_ir->false_label);
            break;
        case OP_jump:
            printf("\tj %s", ph2_ir->func_name);
            break;
        case OP_call:
            printf("\tcall @%s", ph2_ir->func_name);
            break;
        case OP_return:
            if (ph2_ir->src0 == -1)
                printf("\tret");
            else
                printf("\tret %%x%c", rs1);
            break;
        case OP_load:
            printf("\tload %%x%c, %d(sp)", rd, ph2_ir->src0);
            break;
        case OP_store:
            printf("\tstore %%x%c, %d(sp)", rs1, ph2_ir->src1);
            break;
        case OP_global_load:
            printf("\tload %%x%c, %d(gp)", rd, ph2_ir->src0);
            break;
        case OP_global_store:
            printf("\tstore %%x%c, %d(gp)", rs1, ph2_ir->src1);
            break;
        case OP_read:
            printf("\t%%x%c = (%%x%c)", rd, rs1);
            break;
        case OP_write:
            printf("\t(%%x%c) = %%x%c", rs2, rs1);
            break;
        case OP_address_of_func:
            printf("\t(%%x%c) = @%s", rs1, ph2_ir->func_name);
            break;
        case OP_load_func:
            printf("\tload %%t0, %d(sp)", ph2_ir->src0);
            break;
        case OP_global_load_func:
            printf("\tload %%t0, %d(gp)", ph2_ir->src0);
            break;
        case OP_indirect:
            printf("\tindirect call @(%%t0)");
            break;
        case OP_negate:
            printf("\tneg %%x%c, %%x%c", rd, rs1);
            break;
        case OP_add:
            printf("\t%%x%c = add %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_sub:
            printf("\t%%x%c = sub %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_mul:
            printf("\t%%x%c = mul %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_div:
            printf("\t%%x%c = div %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_mod:
            printf("\t%%x%c = mod %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_eq:
            printf("\t%%x%c = eq %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_neq:
            printf("\t%%x%c = neq %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_gt:
            printf("\t%%x%c = gt %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_lt:
            printf("\t%%x%c = lt %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_geq:
            printf("\t%%x%c = geq %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_leq:
            printf("\t%%x%c = leq %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_bit_and:
            printf("\t%%x%c = and %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_bit_or:
            printf("\t%%x%c = or %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_bit_not:
            printf("\t%%x%c = not %%x%c", rd, rs1);
            break;
        case OP_bit_xor:
            printf("\t%%x%c = xor %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_log_and:
            printf("\t%%x%c = and %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_log_or:
            printf("\t%%x%c = or %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_log_not:
            printf("\t%%x%c = not %%x%c", rd, rs1);
            break;
        case OP_rshift:
            printf("\t%%x%c = rshift %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_lshift:
            printf("\t%%x%c = lshift %%x%c, %%x%c", rd, rs1, rs2);
            break;
        default:
            break;
        }
        printf("\n");
    }
}
