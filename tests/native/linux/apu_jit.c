/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The native bridge over the real DSP56300 JIT (AArch64, x86-64). No guest
 * title or sound device: standalone DSP programs and the APU's parked frame
 * thread. `apu-jit-smoke bench`: the JIT against the C interpreter on two
 * GP frame programs, their speed and whether the two leave the same memory
 * (not a test; host figures). */
#include "apu_fixture.h"
#include "native/dev/apu/dsp/dsp_dma_regs.h"
#include "native/state/sections.h"
#include "native/state/state.h"

enum {
    CLR_A = 0x200013, CLR_B = 0x20001b, MOVE_ONE_X0 = 0x240100,
    MOVE_TWO_X0 = 0x240200, ADD_X0_A = 0x200040,
    LOAD_X_R0_X0 = 0x44e000, FRAME_COMPLETE_X1 = 0x08c504,
    READ_DMA_X0 = 0x084416, JMP_ZERO = 0x0c0000, WAIT = 0x000086, STOP = 0x000087,
};

typedef struct { uint32_t words[0x800]; } Scratch;

static void scratch_rw(void *opaque, uint8_t *ptr, uint32_t address, size_t length, bool write)
{
    Scratch *scratch = opaque;
    if (address > sizeof(*scratch) || length > sizeof(*scratch) - address) {
        abort();
    }
    uint8_t *memory = (uint8_t *)scratch + address;
    if (write) memcpy(memory, ptr, length);
    else memcpy(ptr, memory, length);
}

static DSPState *new_dsp(Scratch *scratch)
{
    g_apu_config.use_dsp = g_apu_config.use_dsp_jit = true;
    DSPState *dsp = dsp_init(scratch, scratch_rw, NULL);
    check(!strcmp(dsp_engine_name(dsp), "JIT"), "full DSP creates the JIT engine");
    return dsp;
}

/* Changing PC through the FFI preserves compiled blocks, unlike loading a
 * native savestate, which deliberately invalidates them. */
static Dsp56300State at(DSPState *dsp, uint32_t pc)
{
    Dsp56300State state;
    dsp56300_get_state(dsp->jit, &state);
    state.pc = pc;
    state.halt_requested = false;
    state.power_state = 0;
    state.cycle_budget = 0;
    return state;
}

static void step_zero(DSPState *dsp)
{
    Dsp56300State state = at(dsp, 0);
    dsp56300_set_state(dsp->jit, &state);
    dsp_step(dsp);
}

static void run_zero(DSPState *dsp)
{
    Dsp56300State state = at(dsp, 0);
    state.registers[DSP_REG_X1] = 1;
    dsp56300_set_state(dsp->jit, &state);
    dsp_start_frame(dsp);
    dsp_run(dsp, 1000);
    dsp_state_prepare_save(dsp);
}

static void test_memory_and_rewrites(void)
{
    Scratch scratch = {0};
    DSPState *dsp = new_dsp(&scratch);
    dsp_write_memory(dsp, 'X', 0xc09, 0x123456);
    check(dsp_read_memory(dsp, 'X', 0x1409) == 0x123456 && dsp->core.mixbuffer[9] == 0x123456,
          "X:c00 and X:1400 alias the VP's shared mix buffer");
    dsp_write_memory(dsp, 'X', 0x1409, 0xabcdef);
    check(dsp_read_memory(dsp, 'X', 0xc09) == 0xabcdef,
          "writes through the second mix-buffer alias reach the first");
    dsp->core.mixbuffer[9] = 0x654321;
    dsp_write_memory(dsp, 'P', 0, LOAD_X_R0_X0);
    for (unsigned alias = 0; alias < 2; alias++) {
        Dsp56300State state = at(dsp, 0);
        state.registers[DSP_REG_R0] = (alias ? 0x1400 : 0xc00) + 9;
        dsp56300_set_state(dsp->jit, &state);
        dsp_step(dsp);
        check(dsp->core.registers[DSP_REG_X0] == 0x654321,
              "compiled memory load sees a direct VP write through either X alias");
    }
    dsp_write_memory(dsp, 'Y', 3, 0xfedcba);
    check(dsp->core.yram[3] == 0xfedcba, "JIT Y memory shares the native array");

    dsp_write_memory(dsp, 'P', 0, CLR_A);
    Dsp56300State state = at(dsp, 0);
    state.registers[DSP_REG_A1] = 0x123456;
    state.registers[DSP_REG_B1] = 0x654321;
    dsp56300_set_state(dsp->jit, &state);
    dsp_step(dsp);
    check(!dsp->core.registers[DSP_REG_A1] && dsp->core.registers[DSP_REG_B1] == 0x654321,
          "warm compiled ALU instruction executes clr A");
    dsp_write_memory(dsp, 'P', 0, CLR_B);
    state = at(dsp, 0);
    state.registers[DSP_REG_A1] = 0x123456;
    state.registers[DSP_REG_B1] = 0x654321;
    dsp56300_set_state(dsp->jit, &state);
    dsp_step(dsp);
    check(dsp->core.registers[DSP_REG_A1] == 0x123456 && !dsp->core.registers[DSP_REG_B1],
          "native P rewrite replaces a warm JIT instruction");

    const uint32_t program[] = {CLR_A, MOVE_ONE_X0, ADD_X0_A, FRAME_COMPLETE_X1};
    for (unsigned i = 0; i < ARRAY_SIZE(program); i++) dsp_write_memory(dsp, 'P', i, program[i]);
    run_zero(dsp);
    check(dsp->core.registers[DSP_REG_A1] == 0x10000, "the DMA target program has a warm compiled block");
    scratch.words[0] = MOVE_TWO_X0;
    const uint32_t descriptor[] = {NODE_POINTER_EOL, (0xf << 5) | (2 << 10), 1, 0x2801, 0, 0,
                                   sizeof(scratch) - 1};
    for (unsigned i = 0; i < ARRAY_SIZE(descriptor); i++) {
        dsp_write_memory(dsp, 'X', 0x100 + i, descriptor[i]);
    }
    dsp_dma_write(&dsp->dma, DMA_NEXT_BLOCK, 0x100);
    dsp_write_memory(dsp, 'X', 0xffffd6, DMA_CONTROL_ACTION_START);
    run_zero(dsp);
    check(dsp_read_memory(dsp, 'P', 1) == MOVE_TWO_X0 && dsp->core.registers[DSP_REG_A1] == 0x20000,
          "scratch DMA replaces warm P code and executes the uploaded instruction");
    memcpy(scratch.words, program, sizeof(program));
    dsp_bootstrap(dsp);
    run_zero(dsp);
    check(dsp->core.registers[DSP_REG_A1] == 0x10000,
          "bootstrap invalidates warm JIT code before the uploaded program executes");
    dsp_write_memory(dsp, 'P', 0, READ_DMA_X0);
    uint32_t polls[3];
    for (unsigned i = 0; i < ARRAY_SIZE(polls); i++) {
        step_zero(dsp);
        polls[i] = dsp->core.registers[DSP_REG_X0];
    }
    check((polls[0] & DMA_CONTROL_RUNNING) && (polls[1] & DMA_CONTROL_RUNNING) &&
          !(polls[2] & DMA_CONTROL_RUNNING) && (polls[2] & DMA_CONTROL_STOPPED),
          "compiled DMA polls observe RUNNING before STOPPED");

    dsp_reset(dsp);
    dsp->core.pram[0] = MOVE_ONE_X0;
    dsp_step(dsp);
    check(dsp->core.registers[DSP_REG_X0] == 0x10000,
          "reset discards old code and starts the new program at zero");
    dsp_destroy(dsp);
}

static void test_frames_and_power(void)
{
    Scratch scratch = {0};
    DSPState *dsp = new_dsp(&scratch);
    const uint32_t program[] = {CLR_A, MOVE_ONE_X0, ADD_X0_A, FRAME_COMPLETE_X1};
    for (unsigned i = 0; i < ARRAY_SIZE(program); i++) dsp_write_memory(dsp, 'P', i, program[i]);
    Dsp56300State state = at(dsp, 0);
    state.registers[DSP_REG_X1] = 1;
    dsp56300_set_state(dsp->jit, &state);
    dsp_start_frame(dsp);
    dsp_run(dsp, 1000);
    dsp_state_prepare_save(dsp);
    check(dsp->core.is_idle && dsp->core.pc == ARRAY_SIZE(program) &&
          dsp->core.registers[DSP_REG_A1] == 0x10000 && dsp->core.cycle_count,
          "compiled arithmetic completes its frame through the native peripheral callback");
    dsp_write_memory(dsp, 'P', 1, MOVE_TWO_X0);
    state = at(dsp, 0);
    dsp56300_set_state(dsp->jit, &state);
    dsp_start_frame(dsp);
    dsp_run(dsp, 1000);
    dsp_state_prepare_save(dsp);
    check(dsp->core.is_idle && dsp->core.registers[DSP_REG_A1] == 0x20000,
          "a warm multi-instruction block observes a changed program word next frame");
    for (unsigned stopped = 0; stopped < 2; stopped++) {
        dsp_reset(dsp);
        dsp_write_memory(dsp, 'P', 0, stopped ? STOP : WAIT);
        dsp_start_frame(dsp);
        dsp_run(dsp, 1000);
        dsp_run(dsp, 1000);
        dsp_state_prepare_save(dsp);
        check(dsp->core.is_idle && dsp->jit_state.power_state == stopped + 1,
              "WAIT and STOP yield to the APU instead of spinning without executing cycles");
    }
    dsp_destroy(dsp);
}

static bool same_state(const Dsp56300State *a, const Dsp56300State *b)
{
    return a->pc == b->pc && a->pc_advance == b->pc_advance && a->pc_on_rep == b->pc_on_rep &&
           a->cycle_count == b->cycle_count && a->cycle_budget == b->cycle_budget &&
           a->loop_rep == b->loop_rep && a->halt_requested == b->halt_requested &&
           a->power_state == b->power_state &&
           !memcmp(a->registers, b->registers, sizeof(a->registers)) &&
           !memcmp(a->stack, b->stack, sizeof(a->stack)) &&
           !memcmp(a->interrupts.pending_bits, b->interrupts.pending_bits, sizeof(a->interrupts.pending_bits)) &&
           !memcmp(a->interrupts.ipl, b->interrupts.ipl, sizeof(a->interrupts.ipl)) &&
           a->interrupts.pipeline_stage == b->interrupts.pipeline_stage &&
           a->interrupts.vector_addr == b->interrupts.vector_addr &&
           a->interrupts.saved_pc == b->interrupts.saved_pc &&
           a->interrupts.state == b->interrupts.state &&
           a->interrupts.ipl_to_raise == b->interrupts.ipl_to_raise;
}

/* Copy the self-describing APU records while omitting the optional JIT
 * fields, producing the actual pre-JIT file layout rather than a raw struct. */
static bool legacy_record(NcRd *in, NcBuf *out, unsigned *removed)
{
    const uint32_t count = nc_rd_u32(in);
    const size_t count_at = out->len;
    uint32_t kept = 0;
    nc_buf_u32(out, 0);
    for (uint32_t i = 0; i < count && !in->fail; i++) {
        const size_t header = in->pos;
        const uint8_t length = nc_rd_u8(in);
        char name[256] = {0};
        nc_rd_get(in, name, length);
        const uint8_t kind = nc_rd_u8(in);
        const uint32_t size = nc_rd_u32(in), num = nc_rd_u32(in);
        const size_t header_size = in->pos - header;
        NcBuf value = {0};
        if (kind == 1) {
            const void *bytes = nc_rd_ptr(in, size);
            if (bytes) nc_buf_put(&value, bytes, size);
        } else if (kind == 2 || kind == 3) {
            for (uint32_t k = 0; k < (kind == 2 ? num : 1); k++) {
                if (!legacy_record(in, &value, removed)) break;
            }
        } else in->fail = true;
        if (!strcmp(name, "jit_state") || !strcmp(name, "jit_state_valid") || !strcmp(name, "jit_dma_reads")) {
            (*removed)++;
        } else {
            const size_t copied_header = out->len;
            nc_buf_put(out, in->p + header, header_size);
            if (!out->fail && kind == 3 && (!strcmp(name, "gp.dsp") || !strcmp(name, "ep.dsp"))) {
                const uint32_t legacy_size = 80136;
                memcpy(out->p + copied_header + length + 2, &legacy_size, sizeof(legacy_size));
            }
            nc_buf_put(out, value.p, value.len);
            kept++;
        }
        nc_buf_free(&value);
    }
    if (!out->fail) memcpy(out->p + count_at, &kept, sizeof(kept));
    return !in->fail && !out->fail;
}

static void test_apu_and_states(void)
{
    apu_up(true, false, 1);
    DSPState *gp = d->gp.dsp, *ep = d->ep.dsp;
    check(!strcmp(dsp_engine_name(gp), "JIT") && !strcmp(dsp_engine_name(ep), "JIT"),
          "a full APU creates independent GP and EP JIT engines");
    dsp_write_memory(gp, 'P', 0, FRAME_COMPLETE_X1);
    Dsp56300State state = at(gp, 0);
    state.registers[DSP_REG_X1] = 1;
    dsp56300_set_state(gp->jit, &state);
    d->gp.regs[NV_PAPU_GPRST] = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    bins[0][4] = 0.5f;
    bins[1][4] = -0.25f;
    mcpx_apu_dsp_frame(d, bins);
    check(gp->core.is_idle && dsp_read_memory(gp, 'X', 0xc04) == 0x400000 &&
          dsp_read_memory(gp, 'X', 0x1424) == 0xe00000 &&
          d->monitor.frame_buf[4][0] == 16384 && d->monitor.frame_buf[4][1] == -8192,
          "the APU feeds VP mixbins into shared JIT memory and preserves stereo monitor samples");
    dsp_write_memory(gp, 'P', 0, JMP_ZERO);
    state = at(gp, 0);
    state.cycle_budget = 100000000;
    dsp56300_set_state(gp->jit, &state);
    const int64_t before = apu_host_us();
    mcpx_apu_dsp_frame(d, bins);
    check(apu_host_us() - before < 2000000 && gp->core.cycle_count >= 200000 &&
          gp->core.cycle_count < 215000,
          "a nonterminating GP program returns at the bounded frame budget");
    d->gp.regs[NV_PAPU_GPRST] = 0;
    dsp_write_memory(ep, 'P', 0, JMP_ZERO);
    state = at(ep, 0);
    dsp56300_set_state(ep->jit, &state);
    d->ep.regs[NV_PAPU_EPRST] = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    d->ep_frame_div = 0;
    const int64_t ep_before = apu_host_us();
    mcpx_apu_dsp_frame(d, bins);
    check(apu_host_us() - ep_before < 2000000 && ep->core.cycle_count >= 1700000 &&
          ep->core.cycle_count < 1708000,
          "a nonterminating EP program returns at its eight-block frame budget");
    d->ep.regs[NV_PAPU_EPRST] = 0;

    Dsp56300State wanted[2] = {{0}}, got;
    for (unsigned k = 0; k < 2; k++) {
        Dsp56300State *s = &wanted[k];
        s->pc = 0x123456 + k;
        s->pc_advance = 2;
        s->pc_on_rep = s->loop_rep = s->halt_requested = true;
        s->power_state = k + 1;
        s->cycle_count = 0x89abcdef + k;
        s->cycle_budget = -7;
        s->interrupts.pending_bits[1] = 1ull << 33;
        s->interrupts.pipeline_stage = 2;
        s->interrupts.state = 1;
        s->interrupts.vector_addr = 0x654321;
        s->interrupts.saved_pc = 0xabcdef;
        s->interrupts.ipl_to_raise = 3;
        for (unsigned i = 0; i < 128; i++) s->interrupts.ipl[i] = i % 4;
        for (unsigned i = 0; i < 64; i++) s->registers[i] = (0x814357 + i * 0x10203 + k) & 0xffffff;
        for (unsigned i = 0; i < 16; i++) {
            s->stack[0][i] = 0x123400 + i;
            s->stack[1][i] = 0xfedc00 + i;
        }
        dsp56300_set_state(k ? ep->jit : gp->jit, s);
    }
    gp->core.yram[17] = 0x665544;
    gp->core.mixbuffer[9] = 0x123abc;
    gp->dma.control = DMA_CONTROL_RUNNING;
    gp->jit_dma_reads = 2;
    NcBuf saved = {0};
    nc_apu_state_save(&saved);
    dsp_reset(gp);
    dsp_reset(ep);
    gp->core.yram[17] = gp->core.mixbuffer[9] = 0;
    NcRd reader = {saved.p, saved.len, 0, false};
    const bool loaded = nc_apu_state_load(&reader, NC_STATE_V_APU);
    for (unsigned k = 0; k < 2; k++) {
        dsp56300_get_state(k ? ep->jit : gp->jit, &got);
        check(loaded && !saved.fail && same_state(&got, &wanted[k]),
              "native savestate restores GP/EP 24-bit CPU, interrupts, stack and WAIT/STOP state exactly");
    }
    check(gp->core.yram[17] == 0x665544 && dsp_read_memory(gp, 'X', 0xc09) == 0x123abc &&
          (dsp_read_memory(gp, 'X', 0xffffd6) & DMA_CONTROL_STOPPED),
          "savestate restores shared RAM and resumes DMA completion polling at the saved position");
    nc_buf_free(&saved);

    Dsp56300State old = {.pc_advance = 1};
    old.registers[DSP_REG_X0] = 0x123456;
    old.registers[DSP_REG_M0] = 0xffff;
    old.registers[DSP_REG_N0] = 0xfffe;
    dsp56300_set_state(gp->jit, &old);
    dsp_write_memory(gp, 'P', 0, 0x200041); /* tfr X0,A */
    nc_apu_state_save(&saved);
    NcBuf legacy = {0};
    nc_buf_put(&legacy, saved.p, 4);
    NcRd original = {saved.p, saved.len, 4, false};
    unsigned removed = 0;
    const bool copied = legacy_record(&original, &legacy, &removed);
    reader = (NcRd){legacy.p, legacy.len, 0, false};
    const bool old_loaded = copied && removed == 6 && original.pos == original.len &&
                            nc_apu_state_load(&reader, NC_STATE_V_APU);
    dsp56300_get_state(gp->jit, &got);
    check(old_loaded && got.registers[DSP_REG_M0] == 0xffffff &&
          got.registers[DSP_REG_N0] == 0xfffffe && got.registers[DSP_REG_X0] == 0x123456,
          "a pre-JIT APU state loads and widens legacy address modifiers correctly");
    dsp_step(gp);
    check(old_loaded && gp->core.registers[DSP_REG_A1] == 0x123456,
          "execution resumes correctly from the legacy APU state");
    nc_buf_free(&legacy);
    nc_buf_free(&saved);
    apu_down();
}

/* ---- `bench`: two built-in GP frames, P:0 a jump to P:$100: four passes
 * over the mix buffer, each sample through a 64-tap FIR (coefficients at
 * Y:0) and a comb, written back, then the frame's end (movep to X:$ffffc4);
 * the delay lines in modulo addressing (a 256-word FIR line at X:$800, a
 * 128-word comb at X:$400; about 74000 cycles), or linear (the FIR over the
 * next 64 words, 192 samples a pass; about 55000). */
static const uint32_t k_bench_modulo[] = {
    /* move #$1400,r0  move #$800,r1  movec #255,m1  move #0,r4  movec #63,m4
     * move #$400,r2  movec #127,m2  move #0.5,y1  do #4,passes  do #256,samples */
    0x60F400, 0x001400, 0x61F400, 0x000800, 0x05FFA1, 0x340000, 0x053FA4, 0x62F400, 0x000400,
    0x057FA2, 0x274000, 0x060480, 0x00011A, 0x060081, 0x000118,
    /* move x:(r0),x0  move x0,x:(r1)+  clr a x:(r1)+,x0 y:(r4)+,y0  rep #63
     * mac x0,y0,a x:(r1)+,x0 y:(r4)+,y0  macr x0,y0,a  move x:(r2),x1
     * mac x1,y1,a  move a,x:(r2)+  move a,x:(r0)+ */
    0x44E000, 0x445900, 0xF09913, 0x063FA0, 0xF099D2, 0x2000D3, 0x45E200, 0x2000F2, 0x565A00,
    0x565800,
    /* samples: move #$1400,r0; passes: move #1,r5  movep r5,x:$ffffc4  jmp $100 */
    0x60F400, 0x001400, 0x350100, 0x08D504, 0x0AF080, 0x000100,
};
static const uint32_t k_bench_linear[] = {
    /* move #$1400,r0  move #0.5,y1  do #4,passes  do #192,samples */
    0x60F400, 0x001400, 0x274000, 0x060480, 0x000111, 0x06C080, 0x00010F,
    /* move r0,r1  move #0,r4  clr a x:(r1)+,x0 y:(r4)+,y0  rep #63
     * mac x0,y0,a x:(r1)+,x0 y:(r4)+,y0  macr x0,y0,a  move x:(r0),x1
     * mac x1,y1,a  move a,x:(r0)+ */
    0x221100, 0x340000, 0xF09913, 0x063FA0, 0xF099D2, 0x2000D3, 0x45E000, 0x2000F2, 0x565800,
    /* samples: move #$1400,r0; passes: move #1,r5  movep r5,x:$ffffc4  jmp $100 */
    0x60F400, 0x001400, 0x350100, 0x08D504, 0x0AF080, 0x000100,
};

typedef struct {
    uint32_t p[DSP_PRAM_SIZE], x[DSP_XRAM_SIZE], y[DSP_YRAM_SIZE];
} DspImage;

/* X below the mix buffer's alias (the JIT's X:c00 is the mix buffer) */
static void load(DSPState *dsp, const DspImage *im)
{
    dsp_reset(dsp);
    for (uint32_t i = 0; i < DSP_PRAM_SIZE; i++) dsp_write_memory(dsp, 'P', i, im->p[i] & 0xffffff);
    for (uint32_t i = 0; i < 0xc00; i++) dsp_write_memory(dsp, 'X', i, im->x[i] & 0xffffff);
    for (uint32_t i = 0; i < DSP_YRAM_SIZE; i++) dsp_write_memory(dsp, 'Y', i, im->y[i] & 0xffffff);
}

/* what an engine leaves: X below c00, Y, the mix buffer */
typedef struct {
    double us, worst_us, cycles;
    uint32_t x[0xc00], y[DSP_YRAM_SIZE], mix[DSP_MIXBUFFER_SIZE];
} BenchRun;

/* frames GP frames of gp by an engine, after warm ones (the JIT compiles) */
static void bench_run(const DspImage *gp, bool jit, int warm, int frames, BenchRun *r)
{
    setenv("NC_APU_DSP_JIT", jit ? "1" : "0", 1);
    apu_up(true, false, 1);
    check(!strcmp(dsp_engine_name(d->gp.dsp), jit ? "JIT" : "interpreter"),
          jit ? "the frame program runs on the JIT" : "the frame program runs on the interpreter");
    load(d->gp.dsp, gp);
    d->gp.regs[NV_PAPU_GPRST] = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    static float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    double total = 0, cycles = 0, worst = 0;
    for (int f = -warm; f < frames; f++) {
        for (int b = 0; b < NUM_MIXBINS; b++) {
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                const int n = (f + warm) * NUM_SAMPLES_PER_FRAME + i;
                bins[b][i] = b < 6 ? (float)(0.25 * sin(2 * M_PI * (220.0 + 110 * b) * n / 48000)) : 0.0f;
            }
        }
        const int64_t t0 = apu_host_us();
        mcpx_apu_dsp_frame(d, bins);
        const double us = (double)(apu_host_us() - t0);
        if (f >= 0) {
            total += us;
            worst = fmax(worst, us);
            cycles += d->gp.dsp->core.cycle_count;
        }
    }
    r->us = total / frames;
    r->worst_us = worst;
    r->cycles = cycles / frames;
    for (uint32_t i = 0; i < 0xc00; i++) r->x[i] = dsp_read_memory(d->gp.dsp, 'X', i);
    for (uint32_t i = 0; i < DSP_YRAM_SIZE; i++) r->y[i] = dsp_read_memory(d->gp.dsp, 'Y', i);
    memcpy(r->mix, d->gp.dsp->core.mixbuffer, sizeof(r->mix));
    apu_down();
}

static bool same_memory(const BenchRun *a, const BenchRun *b)
{
    return !memcmp(a->x, b->x, sizeof(a->x)) && !memcmp(a->y, b->y, sizeof(a->y)) &&
           !memcmp(a->mix, b->mix, sizeof(a->mix));
}

static void bench_pair(const char *what, const DspImage *gp)
{
    static BenchRun runs[2];
    const int frames = 1500;
    for (int jit = 0; jit < 2; jit++) {
        bench_run(gp, jit, 200, frames, &runs[jit]);
        printf("bench %s, %-11s %8.1f us a frame (worst %7.1f), %6.0f cycles a frame: %7.1f MHz, %.1fx real time\n",
               what, jit ? "JIT" : "interpreter", runs[jit].us, runs[jit].worst_us, runs[jit].cycles,
               runs[jit].cycles / runs[jit].us, 1e6 / 1500 / runs[jit].us);
    }
    /* the first word that differs, and each engine's memory hash (FNV-1a:
     * another host's JIT against this one's) */
    char diff[96] = "the same";
    const struct { const char *name; size_t off, n; } spaces[] = {
        {"X", offsetof(BenchRun, x), 0xc00}, {"Y", offsetof(BenchRun, y), DSP_YRAM_SIZE},
        {"mix", offsetof(BenchRun, mix), DSP_MIXBUFFER_SIZE},
    };
    uint64_t hash[2] = {1469598103934665603ull, 1469598103934665603ull};
    unsigned differ = 0;
    for (size_t k = 0; k < sizeof(spaces) / sizeof(spaces[0]); k++) {
        const uint32_t *a = (const uint32_t *)((const char *)&runs[0] + spaces[k].off);
        const uint32_t *b = (const uint32_t *)((const char *)&runs[1] + spaces[k].off);
        for (size_t i = 0; i < spaces[k].n; i++) {
            hash[0] = (hash[0] ^ a[i]) * 1099511628211ull;
            hash[1] = (hash[1] ^ b[i]) * 1099511628211ull;
            if (a[i] != b[i] && !differ++) {
                snprintf(diff, sizeof(diff), "differ from %s:$%04zx (%06x, JIT %06x)", spaces[k].name, i, a[i], b[i]);
            }
        }
    }
    printf("bench %s: the JIT %.1fx the interpreter's speed; after %d frames their memories %s, %u words; "
           "hashes %016llx, JIT %016llx\n", what, runs[0].us / runs[1].us, frames + 200, diff, differ,
           (unsigned long long)hash[0], (unsigned long long)hash[1]);
}

static void builtin(DspImage *gp, const uint32_t *frame, size_t words)
{
    memset(gp, 0, sizeof(*gp));
    gp->p[0] = 0x0AF080;   /* jmp $100 */
    gp->p[1] = 0x000100;
    memcpy(gp->p + 0x100, frame, words * sizeof(*frame));
    for (int i = 0; i < 64; i++) {
        gp->y[i] = (uint32_t)(int32_t)(0x7fffff * 0.9 * sin(M_PI * (i + 1) / 65) / 64) & 0xffffff;
    }
}

/* The engines on the linear frame program: the same memory. Not yet the
 * modulo one: the pinned JIT's inline REP and DO loops address through Rn
 * as it was before a modulo update (its jit_update_rn path), on AArch64 as
 * on x86-64; single steps agree. */
static void test_engines_agree(void)
{
    static DspImage gp;
    static BenchRun runs[2];
    builtin(&gp, k_bench_linear, ARRAY_SIZE(k_bench_linear));
    bench_run(&gp, false, 0, 20, &runs[0]);
    bench_run(&gp, true, 0, 20, &runs[1]);
    check(same_memory(&runs[0], &runs[1]),
          "the JIT leaves the interpreter's memory after 20 frames of a 64-tap FIR and a comb");
}

static int bench(void)
{
    static DspImage gp;
    g_ram = calloc(1, RAM_SIZE);
    builtin(&gp, k_bench_modulo, ARRAY_SIZE(k_bench_modulo));
    bench_pair("modulo", &gp);
    builtin(&gp, k_bench_linear, ARRAY_SIZE(k_bench_linear));
    bench_pair("linear", &gp);
    free(g_ram);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        unsetenv("NC_APU_GP_PROBE");
        return bench();
    }
    alarm(25);
    setenv("NC_APU_DSP_JIT", "1", 1);
    unsetenv("NC_APU_GP_PROBE");
    test_memory_and_rewrites();
    test_frames_and_power();
    g_ram = malloc(RAM_SIZE);
    test_apu_and_states();
    test_engines_agree();
    free(g_ram);
    g_apu_config.use_dsp = false;
    Scratch scratch = {0};
    DSPState *fast = dsp_init(&scratch, scratch_rw, NULL);
    check(!strcmp(dsp_engine_name(fast), "interpreter"), "fast audio keeps the C interpreter");
    dsp_destroy(fast);
    g_apu_config.use_dsp = true;
    g_apu_config.use_dsp_jit = false;
    DSPState *fallback = dsp_init(&scratch, scratch_rw, NULL);
    check(!strcmp(dsp_engine_name(fallback), "interpreter"), "full audio can select the C interpreter fallback");
    dsp_destroy(fallback);
    return failures ? 1 : 0;
}
