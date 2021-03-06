#include <map>
#include <set>

#include "vendor/picosha2.h"

extern "C" {
#include "tracer_utils.h"
}

#include "server.hpp"

#include "common/sl2_server_api.hpp"
#include "common/sl2_dr_client.hpp"
#include "common/sl2_dr_client_options.hpp"

#include "dr_ir_instr.h"

static SL2Client client;
static sl2_conn sl2_conn;
static sl2_exception_ctx trace_exception_ctx;
static std::string run_id_s;
static void *mutatex;
static bool replay = false;
static bool no_mutate = false;
static bool crashed = false;
static uint32_t mutate_count = 0;

/*! Set that tracks over time which registers have become tainted */
static std::set<reg_id_t, std::less<reg_id_t>, sl2_dr_allocator<reg_id_t>> tainted_regs;
/*! Set that tracks over time which memory addresses have become tainted */
static std::set<app_pc, std::less<app_pc>, sl2_dr_allocator<app_pc>> tainted_mems;

#define LAST_COUNT 5 // WARNING: If you change this, you need to update the database schema

static int last_call_idx = 0;
static int last_insn_idx = 0;
static app_pc last_calls[LAST_COUNT] = {0};
static app_pc last_insns[LAST_COUNT] = {0};

/*! memory map information for target module */
static app_pc module_start = 0;
/*! memory map information for target module */
static app_pc module_end = 0;
/*! memory map information for target module */
static size_t baseAddr;

/** Mostly used to debug if taint tracking is too slow */
static droption_t<unsigned int> op_no_taint(DROPTION_SCOPE_CLIENT, "nt", 0, "no-taint",
                                            "Do not do instruction level instrumentation.");

/** Used when replaying a run from the server */
static droption_t<std::string> op_replay(DROPTION_SCOPE_CLIENT, "r", "", "replay",
                                         "The run id for a crash to replay.");

/**
 * Run the tracer without mutating anything (but still taint the input buffer)
 */
static droption_t<bool> op_no_mutate(DROPTION_SCOPE_CLIENT, "nm", false, "no-mutate",
                                     "Don't use the mutated buffer when replaying.");

/** Currently unused as this runs on 64 bit applications */
static reg_id_t reg_to_full_width32(reg_id_t reg) {
  switch (reg) {
  case DR_REG_AX:
  case DR_REG_AH:
  case DR_REG_AL:
    return DR_REG_EAX;
  case DR_REG_BX:
  case DR_REG_BH:
  case DR_REG_BL:
    return DR_REG_EBX;
  case DR_REG_CX:
  case DR_REG_CH:
  case DR_REG_CL:
    return DR_REG_ECX;
  case DR_REG_DX:
  case DR_REG_DH:
  case DR_REG_DL:
    return DR_REG_EDX;
  case DR_REG_SP:
    return DR_REG_ESP;
  case DR_REG_BP:
    return DR_REG_EBP;
  case DR_REG_SI:
    return DR_REG_ESI;
  case DR_REG_DI:
    return DR_REG_EDI;
  default:
    return reg;
  }
}

/** Converts a register to full width for taint tracking */
static reg_id_t reg_to_full_width64(reg_id_t reg) {
  switch (reg) {
  case DR_REG_EAX:
  case DR_REG_AX:
  case DR_REG_AH:
  case DR_REG_AL:
    return DR_REG_RAX;
  case DR_REG_EBX:
  case DR_REG_BX:
  case DR_REG_BH:
  case DR_REG_BL:
    return DR_REG_RBX;
  case DR_REG_ECX:
  case DR_REG_CX:
  case DR_REG_CH:
  case DR_REG_CL:
    return DR_REG_RCX;
  case DR_REG_EDX:
  case DR_REG_DX:
  case DR_REG_DH:
  case DR_REG_DL:
    return DR_REG_RDX;
  case DR_REG_R8D:
  case DR_REG_R8W:
  case DR_REG_R8L:
    return DR_REG_R8;
  case DR_REG_R9D:
  case DR_REG_R9W:
  case DR_REG_R9L:
    return DR_REG_R9;
  case DR_REG_R10D:
  case DR_REG_R10W:
  case DR_REG_R10L:
    return DR_REG_R10;
  case DR_REG_R11D:
  case DR_REG_R11W:
  case DR_REG_R11L:
    return DR_REG_R11;
  case DR_REG_R12D:
  case DR_REG_R12W:
  case DR_REG_R12L:
    return DR_REG_R12;
  case DR_REG_R13D:
  case DR_REG_R13W:
  case DR_REG_R13L:
    return DR_REG_R13;
  case DR_REG_R14D:
  case DR_REG_R14W:
  case DR_REG_R14L:
    return DR_REG_R14;
  case DR_REG_R15D:
  case DR_REG_R15W:
  case DR_REG_R15L:
    return DR_REG_R15;
  case DR_REG_ESP:
  case DR_REG_SP:
    return DR_REG_RSP;
  case DR_REG_EBP:
  case DR_REG_BP:
    return DR_REG_RBP;
  case DR_REG_ESI:
  case DR_REG_SI:
    return DR_REG_RSI;
  case DR_REG_EDI:
  case DR_REG_DI:
    return DR_REG_RDI;
  default:
    return reg;
  }
}

/** Check whether and operand is tainted */
static bool is_tainted(void *drcontext, opnd_t opnd) {
  if (opnd_is_reg(opnd)) {
    /** Check if a register is in tainted_regs */
    reg_id_t reg = opnd_get_reg(opnd);
    reg = reg_to_full_width64(reg);

    if (tainted_regs.find(reg) != tainted_regs.end()) {
      return true;
    }
  } else if (opnd_is_memory_reference(opnd)) {
    dr_mcontext_t mc = {sizeof(mc), DR_MC_ALL};
    dr_get_mcontext(drcontext, &mc);
    app_pc addr = opnd_compute_address(opnd, &mc);

    /* Check if a memory region overlaps a tainted address */
    opnd_size_t dr_size = opnd_get_size(opnd);
    uint size = opnd_size_in_bytes(dr_size);
    for (uint i = 0; i < size; i++) {
      if (tainted_mems.find(addr + i) != tainted_mems.end()) {
        return true;
      }
    }

    /* Check if a register used in calculating an address is tainted */
    if (opnd_is_base_disp(opnd)) {
      reg_id_t reg_base = opnd_get_base(opnd);
      reg_id_t reg_disp = opnd_get_disp(opnd);
      reg_id_t reg_indx = opnd_get_index(opnd);

      if (reg_base != NULL &&
          tainted_regs.find(reg_to_full_width64(reg_base)) != tainted_regs.end()) {
        return true;
      }

      if (reg_disp != NULL &&
          tainted_regs.find(reg_to_full_width64(reg_disp)) != tainted_regs.end()) {
        return true;
      }

      if (reg_indx != NULL &&
          tainted_regs.find(reg_to_full_width64(reg_indx)) != tainted_regs.end()) {
        return true;
      }
    }
  }
  // else if(opnd_is_pc(opnd)) {
  //     opnd_get_pc(opnd);
  // } else if(opnd_is_abs_addr(opnd)) {
  //     opnd_get_addr(opnd);
  // }
  return false;
}

/** Mark a memory address as tainted */
static void taint_mem(app_pc addr, size_t size) {
  for (size_t i = 0; i < size; i++) {
    tainted_mems.insert(addr + i);
  }
}

/** Unmark a memory address as tainted */
static bool untaint_mem(app_pc addr, uint size) {
  bool untainted = false;
  for (uint i = 0; i < size; i++) {
    size_t n = tainted_mems.erase(addr + i);
    if (n) {
      untainted = true;
    }
    if (untainted) {
      // TODO(ww): Why is this branch here?
    }
  }
  return untainted;
}

/** Mark an operand as tainted. Could be a register or memory reference. */
static void taint(void *drcontext, opnd_t opnd) {
  if (opnd_is_reg(opnd)) {
    reg_id_t reg = opnd_get_reg(opnd);
    reg = reg_to_full_width64(reg);

    tainted_regs.insert(reg);

    // char buf[100];
    // opnd_disassemble_to_buffer(drcontext, opnd, buf, 100);
  } else if (opnd_is_memory_reference(opnd)) {
    dr_mcontext_t mc = {sizeof(mc), DR_MC_ALL};
    dr_get_mcontext(drcontext, &mc);
    app_pc addr = opnd_compute_address(opnd, &mc);

    // opnd get size
    opnd_size_t dr_size = opnd_get_size(opnd);
    // opnd size in bytes
    uint size = opnd_size_in_bytes(dr_size);
    // loop insert
    taint_mem(addr, size);
  }
  // else if(opnd_is_pc(opnd)) {
  //     opnd_get_pc(opnd);
  // } else if(opnd_is_abs_addr(opnd)) {
  //     opnd_get_addr(opnd);
  // }

  return;
}

/** Untaint an operand */
static bool untaint(void *drcontext, opnd_t opnd) {
  bool untainted = false;
  if (opnd_is_reg(opnd)) {
    reg_id_t reg = opnd_get_reg(opnd);
    reg = reg_to_full_width64(reg);

    size_t n = tainted_regs.erase(reg);
    if (n) {
      // char buf[100];
      // opnd_disassemble_to_buffer(drcontext, opnd, buf, 100);
      untainted = true;
    }
  } else if (opnd_is_memory_reference(opnd)) {
    dr_mcontext_t mc = {sizeof(mc), DR_MC_ALL};
    dr_get_mcontext(drcontext, &mc);
    app_pc addr = opnd_compute_address(opnd, &mc);

    // opnd get size
    opnd_size_t dr_size = opnd_get_size(opnd);
    // opnd size in bytes
    uint size = opnd_size_in_bytes(dr_size);
    // loop insert
    untainted = untaint_mem(addr, size);
  }
  // else if(opnd_is_pc(opnd)) {
  //     opnd_get_pc(opnd);
  // } else if(opnd_is_abs_addr(opnd)) {
  //     opnd_get_addr(opnd);
  // }

  return untainted;
}

/** Handle special case of xor regA, regA - untaint the destination since it's inevitably 0 */
static bool handle_xor(void *drcontext, instr_t *instr) {
  bool result = false;
  int src_count = instr_num_srcs(instr);

  if (src_count == 2) {
    opnd_t opnd_0 = instr_get_src(instr, 0);
    opnd_t opnd_1 = instr_get_src(instr, 1);

    if (opnd_is_reg(opnd_0) && opnd_is_reg(opnd_1)) {
      reg_id_t reg_0 = reg_to_full_width64(opnd_get_reg(opnd_0));
      reg_id_t reg_1 = reg_to_full_width64(opnd_get_reg(opnd_1));

      if (reg_0 == reg_1) {
        size_t n = tainted_regs.erase(reg_0);
        // if(n) {
        //     char buf[100];
        //     opnd_disassemble_to_buffer(drcontext, opnd_0, buf, 100);
        // }
        result = true;
      }
    }
  }

  return result;
}

/** Handle push and pop by not tainting RSP (included in operands) */
static void handle_push_pop(void *drcontext, instr_t *instr) {
  int src_count = instr_num_srcs(instr);
  bool tainted = false;

  // check sources for taint
  for (int i = 0; i < src_count && !tainted; i++) {
    opnd_t opnd = instr_get_src(instr, i);
    tainted |= is_tainted(drcontext, opnd);
  }

  // if tainted
  // taint destinations that aren't rsp
  int dst_count = instr_num_dsts(instr);
  for (int i = 0; i < dst_count && tainted; i++) {
    opnd_t opnd = instr_get_dst(instr, i);

    if (opnd_is_reg(opnd)) {
      reg_id_t reg = opnd_get_reg(opnd);
      reg = reg_to_full_width64(reg);
      if (reg == DR_REG_RSP) {
        continue;
      }
    }

    taint(drcontext, opnd);
  }

  // if not tainted
  // untaint destinations that aren't rsp
  bool untainted = false;
  for (int i = 0; i < dst_count && !tainted; i++) {
    opnd_t opnd = instr_get_dst(instr, i);

    if (opnd_is_reg(opnd)) {
      reg_id_t reg = opnd_get_reg(opnd);
      reg = reg_to_full_width64(reg);
      if (reg == DR_REG_RSP) {
        continue;
      }
    }

    untainted |= untaint(drcontext, opnd);
  }

  // if(tainted | untainted) {
  //     int opcode = instr_get_opcode(instr);
  // char buf[100];
  // instr_disassemble_to_buffer(drcontext, instr, buf, 100);
  // }
}

/** Xchg of a tainted reg and non tainted reg should swap taint */
static bool handle_xchg(void *drcontext, instr_t *instr) {
  bool result = false;
  int src_count = instr_num_srcs(instr);

  if (src_count == 2) {
    opnd_t opnd_0 = instr_get_src(instr, 0);
    opnd_t opnd_1 = instr_get_src(instr, 1);

    if (opnd_is_reg(opnd_0) && opnd_is_reg(opnd_1)) {
      reg_id_t reg_0 = reg_to_full_width64(opnd_get_reg(opnd_0));
      reg_id_t reg_1 = reg_to_full_width64(opnd_get_reg(opnd_1));

      bool reg_0_tainted = tainted_regs.find(reg_0) != tainted_regs.end();
      bool reg_1_tainted = tainted_regs.find(reg_1) != tainted_regs.end();

      if (reg_0_tainted && !reg_1_tainted) {
        tainted_regs.erase(reg_0);
        tainted_regs.insert(reg_1);
        result = true;
      } else if (reg_1_tainted && !reg_0_tainted) {
        tainted_regs.erase(reg_1);
        tainted_regs.insert(reg_0);
        result = true;
      }
    }
  }

  return result;
}

/** Special cases for tainting / untainting PC */
static bool handle_branches(void *drcontext, instr_t *instr) {

  bool is_ret = instr_is_return(instr);
  bool is_direct = instr_is_ubr(instr) || instr_is_cbr(instr) || instr_is_call_direct(instr);
  bool is_indirect = instr_is_mbr(instr);
  bool is_call = instr_is_call(instr);

  if (!is_ret && !is_direct && !is_indirect && !is_call) {
    return false;
  }

  reg_id_t reg_pc = reg_to_full_width64(DR_REG_NULL);
  reg_id_t reg_stack = reg_to_full_width64(DR_REG_ESP);
  bool pc_tainted = tainted_regs.find(reg_pc) != tainted_regs.end();

  bool result = false;
  int src_count = instr_num_srcs(instr);
  int dst_count = instr_num_dsts(instr);

  // call
  if (is_call) {
    if (pc_tainted) {
      // make saved return address tainted
      for (int i = 0; i < dst_count; i++) {
        opnd_t opnd = instr_get_dst(instr, i);
        if (opnd_is_memory_reference(opnd)) {
          taint(drcontext, opnd);
          break;
        }
      }
    }
  }

  // direct branch or call
  if (is_direct) {
    if (pc_tainted) {
      // untaint pc
      tainted_regs.erase(reg_pc);
    }
  }

  // indirect branch or call
  if (is_indirect) {
    for (int i = 0; i < src_count; i++) {
      opnd_t opnd = instr_get_src(instr, i);

      if (opnd_is_reg(opnd)) {
        reg_id_t reg = reg_to_full_width64(opnd_get_reg(opnd));
        if (reg != reg_stack && tainted_regs.find(reg) != tainted_regs.end()) {
          // taint pc
          tainted_regs.insert(reg_pc);
        }
      }
    }
  }

  /* TODO: check that this taints PC if the tainted address is saved (by the if(is_call)) and
   * restored */
  // ret
  if (is_ret) {
    bool tainted = false;
    for (int i = 0; i < src_count; i++) {
      opnd_t opnd = instr_get_src(instr, i);
      if (is_tainted(drcontext, opnd)) {
        tainted = true;
        break;
      }
    }

    if (tainted) {
      // taint pc
      tainted_regs.insert(reg_pc);
    } else {
      // untaint pc
      tainted_regs.erase(reg_pc);
    }
  }

  return true;
}

/** Dispatch to instruction-specific taint handling for things that don't fit the general
    model of tainted operand -> tainted result */
static bool handle_specific(void *drcontext, instr_t *instr) {
  int opcode = instr_get_opcode(instr);
  bool result = false;

  // indirect call
  if (handle_branches(drcontext, instr)) {
    return true;
  }

  switch (opcode) {
  case OP_push:
  case OP_pop:
    handle_push_pop(drcontext, instr);
    return true;
  case OP_xor:
    result = handle_xor(drcontext, instr);
    return result;
  case OP_xchg:
    result = handle_xchg(drcontext, instr);
    return result;
  default:
    return false;
  }
}

/** Called on each instruction. Spreads taint from sources to destinations,
    wipes tainted destinations with untainted sources. */
static void propagate_taint(app_pc pc) {
  // Store instruction trace
  if (pc > module_start && pc < module_end) {
    last_insns[last_insn_idx] = pc;
    last_insn_idx++;
    last_insn_idx %= LAST_COUNT;
  }

  if (tainted_mems.size() == 0 && tainted_regs.size() == 0) {
    return;
  }

  void *drcontext = dr_get_current_drcontext();
  instr_t instr;
  instr_init(drcontext, &instr);
  decode(drcontext, pc, &instr);

  // Save the count of times we've called this function (if it's a call)
  if (instr_is_call(&instr)) {
    opnd_t target = instr_get_target(&instr);
    if (opnd_is_memory_reference(target)) {
      dr_mcontext_t mc = {sizeof(mc), DR_MC_ALL};
      dr_get_mcontext(drcontext, &mc);
      app_pc addr = opnd_compute_address(target, &mc);

      if (pc > module_start && pc < module_end) {
        last_calls[last_call_idx] = addr;
        last_call_idx++;
        last_call_idx %= LAST_COUNT;
      }
    }
  }

  // if(tainted_mems.size() > 0) {
  //     char buf[100];
  //     instr_disassemble_to_buffer(drcontext, &instr, buf, 100);
  //     dr_printf("%s\n", buf);
  // }

  /* Handle specific instructions */
  if (handle_specific(drcontext, &instr)) {
    instr_free(drcontext, &instr);
    return;
  }

  /* Check if sources are tainted */
  int src_count = instr_num_srcs(&instr);
  bool tainted = false;

  for (int i = 0; i < src_count && !tainted; i++) {
    opnd_t opnd = instr_get_src(&instr, i);
    tainted |= is_tainted(drcontext, opnd);
  }

  /* If tainted sources, taint destinations */
  int dst_count = instr_num_dsts(&instr);
  for (int i = 0; i < dst_count && tainted; i++) {
    opnd_t opnd = instr_get_dst(&instr, i);
    taint(drcontext, opnd);
  }

  /* If not tainted sources, untaint destinations*/
  bool untainted = false;
  for (int i = 0; i < dst_count && !tainted; i++) {
    opnd_t opnd = instr_get_dst(&instr, i);
    untainted |= untaint(drcontext, opnd);
  }

  // if(tainted | untainted) {
  //     int opcode = instr_get_opcode(&instr);
  // }

  instr_free(drcontext, &instr);
}

/** Called upon basic block insertion with each individual instruction as an argument.
    Inserts a clean call to propagate_taint before every instruction */
static dr_emit_flags_t on_bb_instrument(void *drcontext, void *tag, instrlist_t *bb, instr_t *instr,
                                        bool for_trace, bool translating, void *user_data) {
  if (!instr_is_app(instr))
    return DR_EMIT_DEFAULT;

  /* Clean call propagate taint on each instruction. Should be side-effect free
      http://dynamorio.org/docs/dr__ir__utils_8h.html#ae7b7bd1e750b8a24ebf401fb6a6d6d5e */
  // TODO(ww): Replace this with instruction injection for performance?
  dr_insert_clean_call(drcontext, bb, instr, propagate_taint, false, 1,
                       OPND_CREATE_INTPTR(instr_get_app_pc(instr)));

  return DR_EMIT_DEFAULT;
}

static void on_thread_init(void *drcontext) {
  SL2_DR_DEBUG("tracer#on_thread_init\n");
}

static void on_thread_exit(void *drcontext) {
  SL2_DR_DEBUG("tracer#on_thread_exit\n");
}

/** Clean up registered callbacks before exiting */
static void on_dr_exit(void) {
  json j;
  j["success"] = crashed;
  j["run_id"] = run_id_s;

  SL2_DR_DEBUG("tracer#on_dr_exit: cleaning up and exiting.\n");

  if (!crashed) {
    SL2_DR_DEBUG("tracer#on_dr_exit: target did NOT crash on replay!\n");

    j["message"] = "replay did not cause a crash";
  } else {
    j["message"] = "replay caused a crash";
  }

  SL2_LOG_JSONL(j);

  if (!op_no_taint.get_value()) {
    if (!drmgr_unregister_bb_insertion_event(on_bb_instrument)) {
      DR_ASSERT(false);
    }
  }

  if (!drmgr_unregister_thread_init_event(on_thread_init) ||
      !drmgr_unregister_thread_exit_event(on_thread_exit) || drreg_exit() != DRREG_SUCCESS) {
    DR_ASSERT(false);
  }

  sl2_conn_close(&sl2_conn);

  drmgr_exit();
}

/** Debug functionality. If you need to use it, add the relevant print statements */
static void dump_regs(void *drcontext, app_pc exception_address) {
  reg_id_t regs[16] = {
      DR_REG_RAX, DR_REG_RBX, DR_REG_RCX, DR_REG_RDX, DR_REG_RSP, DR_REG_RBP,
      DR_REG_RSI, DR_REG_RDI, DR_REG_R8,  DR_REG_R9,  DR_REG_R10, DR_REG_R11,
      DR_REG_R12, DR_REG_R13, DR_REG_R14, DR_REG_R15,
  };

  std::set<reg_id_t>::iterator reg_it;
  for (reg_it = tainted_regs.begin(); reg_it != tainted_regs.end(); reg_it++) {
    // TODO(ww): Implement.
  }

  std::set<app_pc>::iterator mem_it;
  for (mem_it = tainted_mems.begin(); mem_it != tainted_mems.end(); mem_it++) {
    // TODO(ww): Implement.
  }

  for (int i = 0; i < 16; i++) {
    bool tainted = tainted_regs.find(regs[i]) != tainted_regs.end();
    dr_mcontext_t mc = {sizeof(mc), DR_MC_ALL};
    dr_get_mcontext(drcontext, &mc);
    if (tainted) {
      // TODO(ww): Implement.
    } else {
      // TODO(ww): Implement.
    }
  }

  bool tainted = tainted_regs.find(DR_REG_NULL) != tainted_regs.end();
  if (tainted) {
    // TODO(ww): Implement.
  } else {
    // TODO(ww): Implement.
  }
}

/** Get crash info as JSON for dumping to stderr */
std::string dump_json(void *drcontext, uint8_t score, std::string reason, dr_exception_t *excpt,
                      std::string disassembly, bool pc_tainted, bool stack_tainted, bool is_ret,
                      bool is_indirect, bool is_direct, bool is_call, bool mem_write, bool mem_read,
                      bool tainted_src, bool tainted_dst) {
  DWORD exception_code = excpt->record->ExceptionCode;
  app_pc exception_address = (app_pc)excpt->record->ExceptionAddress;

  json j;

  j["score"] = score;
  j["reason"] = reason;
  j["exception"] = client.exception_to_string(exception_code);
  j["location"] = (uint64_t)exception_address;
  j["instruction"] = disassembly;
  j["pc_tainted"] = pc_tainted;
  j["stack_tainted"] = stack_tainted;
  j["is_ret"] = is_ret;
  j["is_indirect"] = is_indirect;
  j["is_direct"] = is_direct;
  j["is_call"] = is_call;
  j["mem_write"] = mem_write;
  j["mem_read"] = mem_read;
  j["tainted_src"] = tainted_src;
  j["tainted_dst"] = tainted_dst;

  j["regs"] = json::array();
  reg_id_t regs[16] = {
      DR_REG_RAX, DR_REG_RBX, DR_REG_RCX, DR_REG_RDX, DR_REG_RSP, DR_REG_RBP,
      DR_REG_RSI, DR_REG_RDI, DR_REG_R8,  DR_REG_R9,  DR_REG_R10, DR_REG_R11,
      DR_REG_R12, DR_REG_R13, DR_REG_R14, DR_REG_R15,
  };

  for (int i = 0; i < 16; i++) {
    bool tainted = tainted_regs.find(regs[i]) != tainted_regs.end();
    json reg = {{"reg", get_register_name(regs[i])},
                {"value", reg_get_value(regs[i], excpt->mcontext)},
                {"tainted", tainted}};
    j["regs"].push_back(reg);
  }

  bool tainted = tainted_regs.find(DR_REG_NULL) != tainted_regs.end();
  json rip = {{"reg", "rip"}, {"value", (uint64_t)exception_address}, {"tainted", tainted}};
  j["regs"].push_back(rip);

  j["last_calls"] = json::array();
  for (int i = 0; i < LAST_COUNT; i++) {
    int idx = last_call_idx + i;
    idx %= LAST_COUNT;
    j["last_calls"].push_back((uint64_t)last_calls[idx]);
  }

  j["last_insns"] = json::array();
  for (int i = 0; i < LAST_COUNT; i++) {
    int idx = last_insn_idx + i;
    idx %= LAST_COUNT;
    j["last_insns"].push_back((uint64_t)last_insns[idx]);
  }

  j["tainted_addrs"] = json::array();
  if (tainted_mems.size() > 0) {
    std::set<app_pc>::iterator mit = tainted_mems.begin();
    uint64_t start = (uint64_t)*mit;
    uint64_t size = 1;

    mit++;
    for (; mit != tainted_mems.end(); mit++) {
      uint64_t curr = (uint64_t)*mit;
      if (curr > (start + size)) {
        json addr = {{"start", start}, {"size", size}};
        j["tainted_addrs"].push_back(addr);

        start = curr;
        size = 0;
      }
      size++;
    }

    json addr = {{"start", start}, {"size", size}};
    j["tainted_addrs"].push_back(addr);
  }

  return j.dump();
}

/** Get Run ID and dump crash info into JSON file in the run folder. */
static void dump_crash(void *drcontext, dr_exception_t *excpt, std::string reason, uint8_t score,
                       std::string disassembly, bool pc_tainted, bool stack_tainted, bool is_ret,
                       bool is_indirect, bool is_direct, bool is_call, bool mem_write,
                       bool mem_read, bool tainted_src, bool tainted_dst) {
  sl2_crash_paths crash_paths = {0};
  std::string crash_json =
      dump_json(drcontext, score, reason, excpt, disassembly, pc_tainted, stack_tainted, is_ret,
                is_indirect, is_direct, is_call, mem_write, mem_read, tainted_src, tainted_dst);

  if (replay) {
    sl2_conn_request_crash_paths(&sl2_conn, dr_get_process_id(), &crash_paths);

    HANDLE dump_file = CreateFile(crash_paths.crash_path, GENERIC_WRITE, NULL, NULL, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, NULL);

    if (dump_file == INVALID_HANDLE_VALUE) {
      SL2_DR_DEBUG("tracer#dump_crash: could not open the crash file (crash_path=%S) (GLE=%d)\n",
                   crash_paths.crash_path, GetLastError());
      dr_abort();
    }

    DWORD txsize;
    if (!WriteFile(dump_file, crash_json.c_str(), (DWORD)crash_json.length(), &txsize, NULL)) {
      SL2_DR_DEBUG("tracer#dump_crash: could not write to the crash file (GLE=%d)\n",
                   GetLastError());
      dr_abort();
    }

    CloseHandle(dump_file);

    HANDLE hDumpFile = CreateFile(crash_paths.mem_dump_path, GENERIC_WRITE, NULL, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hDumpFile == INVALID_HANDLE_VALUE) {
      SL2_DR_DEBUG("tracer#dump_crash: could not open the dump file (GLE=%d)\n", GetLastError());
    }

    EXCEPTION_POINTERS exception_pointers = {0};
    MINIDUMP_EXCEPTION_INFORMATION mdump_info = {0};

    exception_pointers.ExceptionRecord = &(trace_exception_ctx.record);
    exception_pointers.ContextRecord = &(trace_exception_ctx.thread_ctx);

    mdump_info.ThreadId = trace_exception_ctx.thread_id;
    mdump_info.ExceptionPointers = &exception_pointers;
    mdump_info.ClientPointers = true;

    // NOTE(ww): Switching back to the application's state is necessary, as we don't want
    // parts of the instrumentation showing up in our memory dump.
    dr_switch_to_app_state(drcontext);

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile, MiniDumpWithFullMemory,
                      &mdump_info, NULL, NULL);

    dr_switch_to_dr_state(drcontext);

    CloseHandle(hDumpFile);
  }

  dr_exit_process(1);
}

/** Scoring function. Checks exception code, then checks taint state in order to calculate the
 * severity score */
static bool on_exception(void *drcontext, dr_exception_t *excpt) {
  crashed = true;
  DWORD exception_code = excpt->record->ExceptionCode;

  dr_switch_to_app_state(drcontext);
  trace_exception_ctx.thread_id = GetCurrentThreadId();
  dr_mcontext_to_context(&(trace_exception_ctx.thread_ctx), excpt->mcontext);
  dr_switch_to_dr_state(drcontext);

  // Make our own copy of the exception record.
  memcpy(&(trace_exception_ctx.record), excpt->record, sizeof(EXCEPTION_RECORD));

  reg_id_t reg_pc = reg_to_full_width64(DR_REG_NULL);
  reg_id_t reg_stack = reg_to_full_width64(DR_REG_ESP);
  bool pc_tainted = tainted_regs.find(reg_pc) != tainted_regs.end();
  bool stack_tainted = tainted_regs.find(reg_stack) != tainted_regs.end();

  // catch-all result
  app_pc exception_address = (app_pc)(excpt->record->ExceptionAddress);
  std::string reason = "unknown";
  uint8_t score = 50;
  std::string disassembly = "";

  // TODO(ww): Can we use dr_memory_is_readable here?
  if (IsBadReadPtr(exception_address, 1)) {
    if (pc_tainted) {
      reason = "oob execution tainted pc";
      score = 100;
    } else {
      reason = "oob execution";
      score = 50;
    }
    dump_crash(drcontext, excpt, reason, score, disassembly, pc_tainted, stack_tainted, false,
               false, false, false, false, false, false, false);
  }

  instr_t instr;
  // TODO: this isn't instr_freed because of all the early returns
  // it shouldn't hurt though
  instr_init(drcontext, &instr);
  decode(drcontext, exception_address, &instr);
  char buf[100];
  instr_disassemble_to_buffer(drcontext, &instr, buf, 100);

  disassembly = buf;

  // get crashing instruction
  bool is_ret = instr_is_return(&instr);
  bool is_direct = instr_is_ubr(&instr) || instr_is_cbr(&instr) || instr_is_call_direct(&instr);
  bool is_indirect = instr_is_mbr(&instr);
  bool is_call = instr_is_call(&instr); // this might be covered in other flags

  bool mem_write = instr_writes_memory(&instr);
  bool mem_read = instr_reads_memory(&instr);
  bool tainted_src = false;
  bool tainted_dst = false;

  // check exception code - illegal instructions are bad
  if (exception_code == EXCEPTION_ILLEGAL_INSTRUCTION) {
    if (pc_tainted) {
      reason = "illegal instruction tainted pc";
      score = 100;
    } else {
      reason = "illegal instruction";
      score = 50;
    }
    dump_crash(drcontext, excpt, reason, score, disassembly, pc_tainted, stack_tainted, false,
               false, false, false, false, false, false, false);
  }

  // Divide by zero is probably not too bad
  if (exception_code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
    reason = "divide by zero";
    score = 50;
    dump_crash(drcontext, excpt, reason, score, disassembly, pc_tainted, stack_tainted, is_ret,
               is_indirect, is_direct, is_call, mem_write, mem_read, tainted_src, tainted_dst);
  }

  // Breakpoints
  // Could indicate we're executing non-instructions, but probably just indicates a debugger.
  if (exception_code == EXCEPTION_BREAKPOINT) {
    reason = "breakpoint";
    score = 25;
    dump_crash(drcontext, excpt, reason, score, disassembly, pc_tainted, stack_tainted, is_ret,
               is_indirect, is_direct, is_call, mem_write, mem_read, tainted_src, tainted_dst);
  }

  // check branch
  if (is_direct || is_indirect || is_call) {
    if (pc_tainted) {
      reason = "branching tainted pc";
      score = 75;
    } else {
      reason = "branching";
      score = 25;
    }
    dump_crash(drcontext, excpt, reason, score, disassembly, pc_tainted, stack_tainted, is_ret,
               is_indirect, is_direct, is_call, mem_write, mem_read, tainted_src, tainted_dst);
  }

  // check ret
  if (is_ret) {
    if (pc_tainted || stack_tainted) {
      score = 100;
      reason = "return with taint";
    } else {
      reason = "return";
      score = 75;
    }

    dump_crash(drcontext, excpt, reason, score, disassembly, pc_tainted, stack_tainted, is_ret,
               is_indirect, is_direct, is_call, mem_write, mem_read, tainted_src, tainted_dst);
  }

  int src_count = instr_num_srcs(&instr);
  int dst_count = instr_num_dsts(&instr);

  for (int i = 0; i < src_count; i++) {
    opnd_t opnd = instr_get_src(&instr, i);
    tainted_src |= is_tainted(drcontext, opnd);
  }

  for (int i = 0; i < dst_count; i++) {
    opnd_t opnd = instr_get_dst(&instr, i);
    tainted_dst |= is_tainted(drcontext, opnd);
  }

  // Check if the crash resulted from an invalid memory write
  // usually EXCEPTION_ACCESS_VIOLATION
  if (mem_write) {
    // If what we're writing or where we're writing it to are potentially attacker controlled,
    // that's worse than if it's just a normal invalid write
    if (tainted_src || tainted_dst) {
      reason = "tainted write";
      score = 75;
    } else {
      reason = "write";
      score = 50;
    }

    dump_crash(drcontext, excpt, reason, score, disassembly, pc_tainted, stack_tainted, is_ret,
               is_indirect, is_direct, is_call, mem_write, mem_read, tainted_src, tainted_dst);
  }

  // ditto, but for invalid reads
  if (mem_read) {
    // TODO - do we need to think about tainted destination addresses?
    if (tainted_src) {
      reason = "tainted read";
      score = 75;
    } else {
      reason = "read";
      score = 25;
    }

    dump_crash(drcontext, excpt, reason, score, disassembly, pc_tainted, stack_tainted, is_ret,
               is_indirect, is_direct, is_call, mem_write, mem_read, tainted_src, tainted_dst);
  }

  dump_crash(drcontext, excpt, reason, score, disassembly, pc_tainted, stack_tainted, is_ret,
             is_indirect, is_direct, is_call, mem_write, mem_read, tainted_src, tainted_dst);

  return true;
}

/**
 * Transparent wrapper around SL2Client.wrap_pre_IsProcessorFeaturePresent
 */
static void wrap_pre_IsProcessorFeaturePresent(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_IsProcessorFeaturePresent(wrapcxt, user_data);
}

/**
 * Transparent wrapper around SL2Client.wrap_post_IsProcessorFeaturePresent
 */
static void wrap_post_IsProcessorFeaturePresent(void *wrapcxt, void *user_data) {
  client.wrap_post_IsProcessorFeaturePresent(wrapcxt, user_data);
}

/**
 * Transparent wrapper around SL2Client.wrap_pre_UnhandledExceptionFilter
 */
static void wrap_pre_UnhandledExceptionFilter(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_UnhandledExceptionFilter(wrapcxt, user_data, on_exception);
}

/**
 * Transparent wrapper around SL2Client.wrap_pre_VerifierStopMessage
 */
static void wrap_pre_VerifierStopMessage(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_VerifierStopMessage(wrapcxt, user_data, on_exception);
}

/*
*
  Large block of pre-function callbacks that collect metadata about the target call
*
*/

/**
 * Transparent wrapper around SL2Client.wrap_pre_ReadEventLog
 */
static void wrap_pre_ReadEventLog(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_ReadEventLog(wrapcxt, user_data);
}

/**
 * Transparent wrapper around SL2Client.wrap_pre_RegQueryValueEx
 */
static void wrap_pre_RegQueryValueEx(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_RegQueryValueEx(wrapcxt, user_data);
}

/**
 * Transparent wrapper around SL2Client.wrap_pre_WinHttpWebSocketReceive
 */
static void wrap_pre_WinHttpWebSocketReceive(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_WinHttpWebSocketReceive(wrapcxt, user_data);
}

/**
 * Transparent wrapper around SL2Client.wrap_pre_InternetReadFile
 */
static void wrap_pre_InternetReadFile(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_InternetReadFile(wrapcxt, user_data);
}

/**
 * Transparent wrapper around SL2Client.wrap_pre_WinHttpReadData
 */
static void wrap_pre_WinHttpReadData(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_WinHttpReadData(wrapcxt, user_data);
}

/**
 * Transparent wrapper around SL2Client.wrap_pre_recv
 */
static void wrap_pre_recv(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_recv(wrapcxt, user_data);
}

/**
 * Transparent wrapper around SL2Client.wrap_pre_ReadFile
 */
static void wrap_pre_ReadFile(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_ReadFile(wrapcxt, user_data);
}

/**
 * Transparent wrapper around SL2Client.wrap_pre_fread_s
 */
static void wrap_pre_fread_s(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_fread_s(wrapcxt, user_data);
}

/**
 * Transparent wrapper around SL2Client.wrap_pre_fread
 */
static void wrap_pre_fread(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_fread(wrapcxt, user_data);
}

/**
 * Transparent wrapper around SL2Client.wrap_pre__read
 */
static void wrap_pre__read(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre__read(wrapcxt, user_data);
}

/**
 * Transparent wrapper around SL2Client.wrap_pre_MapViewOfFile
 */
static void wrap_pre_MapViewOfFile(void *wrapcxt, OUT void **user_data) {
  client.wrap_pre_MapViewOfFile(wrapcxt, user_data);
}

/** Called after each targeted function to replay mutation and mark bytes as tainted */
static void wrap_post_Generic(void *wrapcxt, void *user_data) {
  void *drcontext = NULL;

  if (!client.is_sane_post_hook(wrapcxt, user_data, &drcontext)) {
    goto cleanup;
  }

  SL2_DR_DEBUG("<in wrap_post_Generic>\n");

  client_read_info *info = (client_read_info *)user_data;

  // Identify whether this is the function we want to target
  bool targeted = client.is_function_targeted(info);
  client.increment_call_count(info->function);

  // Mark the targeted memory as tainted
  if (targeted) {
    taint_mem((app_pc)info->lpBuffer, info->nNumberOfBytesToRead);
  }

  // Talk to the server, get the stored mutation from the fuzzing run, and write it into memory.
  if (replay && targeted) {
    dr_mutex_lock(mutatex);

    if (no_mutate) {
      SL2_DR_DEBUG("user requested replay WITHOUT mutation!\n");
    } else {
      sl2_conn_request_replay(&sl2_conn, mutate_count, info->nNumberOfBytesToRead, info->lpBuffer);
    }

    mutate_count++;

    dr_mutex_unlock(mutatex);
  }

cleanup:

  if (info->argHash) {
    dr_thread_free(drcontext, info->argHash, SL2_HASH_LEN + 1);
  }

  dr_thread_free(drcontext, info, sizeof(client_read_info));
}

/**
 * Replays mutation and marks bytes as tainted. MapViewOfFile can't use the generic callback.
 */
static void wrap_post_MapViewOfFile(void *wrapcxt, void *user_data) {
  void *drcontext = NULL;
  bool interesting_call = true;

  if (!client.is_sane_post_hook(wrapcxt, user_data, &drcontext)) {
    goto cleanup;
  }

  SL2_DR_DEBUG("<in wrap_post_MapViewOfFile>\n");

  client_read_info *info = (client_read_info *)user_data;
  info->lpBuffer = drwrap_get_retval(wrapcxt);
  MEMORY_BASIC_INFORMATION memory_info = {0};

  if (!info->nNumberOfBytesToRead) {
    dr_virtual_query((byte *)info->lpBuffer, &memory_info, sizeof(memory_info));

    info->nNumberOfBytesToRead = memory_info.RegionSize;
  }

  hash_context hash_ctx = {0};
  hash_ctx.readSize = info->nNumberOfBytesToRead;

  // NOTE(ww): The wizard should weed these failures out for us; if it happens
  // here, there's not much we can do.
  if (!GetMappedFileName(GetCurrentProcess(), info->lpBuffer, hash_ctx.fileName, MAX_PATH)) {
    SL2_DR_DEBUG(
        "Couldn't get filename for memory map (size=%lu) (GLE=%d)! Assuming uninteresting.\n",
        info->nNumberOfBytesToRead, GetLastError());
    interesting_call = false;
  }

  // Create the argHash, now that we have the correct source and nNumberOfBytesToRead.
  client.hash_args(info->argHash, &hash_ctx);

  bool targeted = client.is_function_targeted(info);
  client.increment_call_count(info->function);

  if (targeted) {
    taint_mem((app_pc)info->lpBuffer, info->nNumberOfBytesToRead);
  }

  // Talk to the server, get the stored mutation from the fuzzing run, and write it into memory.
  if (interesting_call && replay && targeted) {
    dr_mutex_lock(mutatex);

    if (no_mutate) {
      SL2_DR_DEBUG("user requested replay WITHOUT mutation!\n");
    } else {
      sl2_conn_request_replay(&sl2_conn, mutate_count, info->nNumberOfBytesToRead, info->lpBuffer);
    }

    mutate_count++;

    dr_mutex_unlock(mutatex);
  }

#pragma warning(suppress : 4533)
cleanup:

  dr_thread_free(drcontext, info->argHash, SL2_HASH_LEN + 1);
  dr_thread_free(drcontext, info, sizeof(client_read_info));
}

/** Register function pre/post callbacks in each module */
static void on_module_load(void *drcontext, const module_data_t *mod, bool loaded) {
  if (!strcmp(dr_get_application_name(), dr_module_preferred_name(mod))) {
    baseAddr = (size_t)mod->start;
  }

  const char *mod_name = dr_module_preferred_name(mod);
  app_pc towrap;

  sl2_pre_proto_map pre_hooks;
  SL2_PRE_HOOK1(pre_hooks, ReadFile);
  SL2_PRE_HOOK1(pre_hooks, InternetReadFile);
  SL2_PRE_HOOK2(pre_hooks, ReadEventLogA, ReadEventLog);
  SL2_PRE_HOOK2(pre_hooks, ReadEventLogW, ReadEventLog);
  SL2_PRE_HOOK1(pre_hooks, WinHttpWebSocketReceive);
  SL2_PRE_HOOK1(pre_hooks, WinHttpReadData);
  SL2_PRE_HOOK1(pre_hooks, recv);
  SL2_PRE_HOOK1(pre_hooks, fread_s);
  SL2_PRE_HOOK1(pre_hooks, fread);
  SL2_PRE_HOOK1(pre_hooks, _read);
  SL2_PRE_HOOK1(pre_hooks, MapViewOfFile);

  if (op_registry.get_value()) {
    SL2_PRE_HOOK2(pre_hooks, RegQueryValueExW, RegQueryValueEx);
    SL2_PRE_HOOK2(pre_hooks, RegQueryValueExA, RegQueryValueEx);
  }

  sl2_post_proto_map post_hooks;
  SL2_POST_HOOK2(post_hooks, ReadFile, Generic);
  SL2_POST_HOOK2(post_hooks, InternetReadFile, Generic);
  SL2_POST_HOOK2(post_hooks, ReadEventLogA, Generic);
  SL2_POST_HOOK2(post_hooks, ReadEventLogW, Generic);

  if (op_registry.get_value()) {
    SL2_POST_HOOK2(post_hooks, RegQueryValueExW, Generic);
    SL2_POST_HOOK2(post_hooks, RegQueryValueExA, Generic);
  }

  SL2_POST_HOOK2(post_hooks, WinHttpWebSocketReceive, Generic);
  SL2_POST_HOOK2(post_hooks, WinHttpReadData, Generic);
  SL2_POST_HOOK2(post_hooks, recv, Generic);
  SL2_POST_HOOK2(post_hooks, fread_s, Generic);
  SL2_POST_HOOK2(post_hooks, fread, Generic);
  SL2_POST_HOOK2(post_hooks, _read, Generic);
  SL2_POST_HOOK1(post_hooks, MapViewOfFile);

  // Wrap IsProcessorFeaturePresent and UnhandledExceptionFilter to prevent
  // __fastfail from circumventing our exception tracking. See the comment
  // above wrap_pre_IsProcessorFeaturePresent for more information.
  if (STREQI(mod_name, "KERNELBASE.DLL")) {
    SL2_DR_DEBUG("loading __fastfail mitigations\n");

    towrap = (app_pc)dr_get_proc_address(mod->handle, "IsProcessorFeaturePresent");
    drwrap_wrap(towrap, wrap_pre_IsProcessorFeaturePresent, wrap_post_IsProcessorFeaturePresent);

    towrap = (app_pc)dr_get_proc_address(mod->handle, "UnhandledExceptionFilter");
    drwrap_wrap(towrap, wrap_pre_UnhandledExceptionFilter, NULL);
  }

  // Wrap VerifierStopMessage and VerifierStopMessageEx, which are apparently
  // used in AppVerifier to register heap corruptions.
  //
  // NOTE(ww): I haven't seen these in the wild, but WinAFL wraps
  // VerifierStopMessage and VerifierStopMessageEx is probably
  // just a newer version of the former.
  if (STREQ(mod_name, "VERIFIER.DLL")) {
    SL2_DR_DEBUG("loading Application Verifier mitigations\n");

    towrap = (app_pc)dr_get_proc_address(mod->handle, "VerifierStopMessage");
    drwrap_wrap(towrap, wrap_pre_VerifierStopMessage, NULL);

    towrap = (app_pc)dr_get_proc_address(mod->handle, "VerifierStopMessageEx");
    drwrap_wrap(towrap, wrap_pre_VerifierStopMessage, NULL);
  }

  // TODO(ww): Wrap DllDebugObjectRpcHook.
  if (STREQ(mod_name, "OLE32.DLL")) {
    SL2_DR_DEBUG("OLE32.DLL loaded, but we don't have an DllDebugObjectRpcHook mitigation yet!\n");
  }

  /* assume our target executable is an exe */
  if (strstr(mod_name, ".exe") != NULL) {
    module_start = mod->start; // TODO evaluate us of dr_get_application_name above
    module_end = module_start + mod->module_internal_size;
  }

  void(__cdecl * pre_hook)(void *, void **);
  void(__cdecl * post_hook)(void *, void *);

  sl2_pre_proto_map::iterator it;
  for (it = pre_hooks.begin(); it != pre_hooks.end(); it++) {
    char *function_name = it->first;
    bool hook = false;

    if (!client.function_is_in_expected_module(function_name, mod_name)) {
      continue;
    }

    // Look for function matching the target specified on the command line
    for (targetFunction t : client.parsedJson) {
      if (t.selected && STREQ(t.functionName.c_str(), function_name)) {
        hook = true;
      } else if (t.selected && (STREQ(function_name, "RegQueryValueExW") ||
                                STREQ(function_name, "RegQueryValueExA"))) {
        if (!STREQ(t.functionName.c_str(), "RegQueryValueEx")) {
          hook = false;
        }
      }
    }

    if (!hook) {
      continue;
    }

    pre_hook = it->second;
    post_hook = post_hooks[function_name];

    // find target function in module
    towrap = (app_pc)dr_get_proc_address(mod->handle, function_name);

    // if the function was found, wrap it
    if (towrap != NULL) {
      dr_flush_region(towrap, 0x1000);
      bool ok = drwrap_wrap(towrap, pre_hook, post_hook);
      // bool ok = false;
      if (ok) {
        SL2_DR_DEBUG("<wrapped %s @ 0x%p>\n", function_name, towrap);
      } else {
        SL2_DR_DEBUG("<FAILED to wrap %s @ 0x%p: already wrapped?>\n", function_name, towrap);
      }
    }
  }
}

/**
 * Initializes tracer
 */
DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
  // parse client options
  std::string parse_err;
  int last_idx = 0;
  if (!droption_parser_t::parse_argv(DROPTION_SCOPE_CLIENT, argc, argv, &parse_err, &last_idx)) {
    SL2_DR_DEBUG("tracer#main: usage error: %s", parse_err.c_str());
    dr_abort();
  }

  // target is mandatory
  std::string target = op_target.get_value();
  if (target == "") {
    SL2_DR_DEBUG("tracer#main: ERROR: arg -t (target) required");
    dr_abort();
  }

  if (!client.loadTargets(target)) {
    SL2_DR_DEBUG("Failed to load targets!\n");
    dr_abort();
  }

  if (sl2_conn_open(&sl2_conn) != SL2Response::OK) {
    SL2_DR_DEBUG("ERROR: Couldn't open a connection to the server!\n");
    dr_abort();
  }

  dr_enable_console_printing();

  drreg_options_t ops = {sizeof(ops), 3, false};
  dr_set_client_name("Tracer", "https://github.com/trailofbits/sienna-locomotive");

  if (!drmgr_init() || !drwrap_init() || drreg_init(&ops) != DRREG_SUCCESS) {
    DR_ASSERT(false);
  }

  run_id_s = op_replay.get_value();
  UUID run_id;

  if (run_id_s.length() > 0) {
    replay = true;
  }

  no_mutate = op_no_mutate.get_value();

  sl2_string_to_uuid(run_id_s.c_str(), &run_id);
  sl2_conn_assign_run_id(&sl2_conn, run_id);

  sl2_conn_register_pid(&sl2_conn, dr_get_process_id(), true);

  mutatex = dr_mutex_create();
  dr_register_exit_event(on_dr_exit);

  // If taint tracing is enabled, register the propagate_taint callback
  if (!op_no_taint.get_value()) {
    // http://dynamorio.org/docs/group__drmgr.html#ga83a5fc96944e10bd7356e0c492c93966
    if (!drmgr_register_bb_instrumentation_event(NULL, on_bb_instrument, NULL)) {
      DR_ASSERT(false);
    }
  }

  if (!drmgr_register_module_load_event(on_module_load) ||
      !drmgr_register_thread_init_event(on_thread_init) ||
      !drmgr_register_thread_exit_event(on_thread_exit) ||
      !drmgr_register_exception_event(on_exception)) {
    DR_ASSERT(false);
  }

  dr_log(NULL, DR_LOG_ALL, 1, "Client 'tracer' initializing\n");
}
