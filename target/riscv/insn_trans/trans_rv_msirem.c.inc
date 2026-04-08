/*
 * RISC-V translation routines for the custom MSI Remapper instructions
 */

static bool trans_msirem_rd(DisasContext *ctx, arg_msirem_rd *a)
{
    decode_save_opc(ctx);

    TCGv dest = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);

    translator_io_start(&ctx->base);
    gen_helper_msirem_rd(dest, tcg_env, src);
    gen_set_gpr(ctx, a->rd, dest);

    return true;
}

static bool trans_msirem_wr(DisasContext *ctx, arg_msirem_wr *a)
{
    decode_save_opc(ctx);

    TCGv src1 = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv src2 = get_gpr(ctx, a->rs2, EXT_NONE);

    translator_io_start(&ctx->base);
    gen_helper_msirem_wr(tcg_env, src1, src2);

    /* As write to memory region we are going to finish tb */
    gen_update_pc(ctx, ctx->cur_insn_len);
    exit_tb(ctx);
    ctx->base.is_jmp = DISAS_NORETURN;

    return true;
}

static bool trans_msirem_rdi(DisasContext *ctx, arg_msirem_rdi *a)
{
    decode_save_opc(ctx);

    TCGv dest = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv_i32 imm = tcg_constant_i32(a->imm_i);

    translator_io_start(&ctx->base);
    gen_helper_msirem_rdi(dest, tcg_env, imm);
    gen_set_gpr(ctx, a->rd, dest);

    return true;
}

static bool trans_msirem_fence(DisasContext *ctx, arg_msirem_fence *a)
{
    /* FENCE is a full memory barrier. */
    tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
    return true;
}
