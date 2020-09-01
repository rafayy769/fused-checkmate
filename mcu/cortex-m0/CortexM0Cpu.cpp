/*
 * Copyright (c) 2019-2020, University of Southampton and Contributors.
 * All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <spdlog/spdlog.h>
#include <chrono>
#include <systemc>
#include <thread>
#include <tlm>
#include "include/cm0-fused.h"
#include "mcu/Cm0Microcontroller.hpp"
#include "mcu/cortex-m0/CortexM0Cpu.hpp"
#include "ps/EventLog.hpp"
#include "utilities/Utilities.hpp"

using namespace sc_core;

struct CPU cpu;

CortexM0Cpu *m_ctx =
    nullptr;  //! Simulation context used for hooking callbacks into thumbulator

CortexM0Cpu::CortexM0Cpu(const sc_module_name nm) : sc_module(nm) {
  iSocket.bind(*this);
  m_ctx = this;

  // Register callbacks for reads & writes by emulator
  cpu_set_write_memory_cb(&CortexM0Cpu::write_cb);
  cpu_set_read_memory_cb(&CortexM0Cpu::read_cb);
  cpu_set_consume_cycles_cb(&CortexM0Cpu::consume_cycles_cb);
  cpu_set_exception_return_cb(&CortexM0Cpu::exception_return_cb);
  cpu_set_next_pipeline_instr_cb(&CortexM0Cpu::next_pipeline_instr_cb);

  // Register eventlog events
  m_idleCyclesEvent = EventLog::getInstance().registerEvent(
      std::string(this->name()) + " idle cycles");

  m_nInstructionsEventId = EventLog::getInstance().registerEvent(
      std::string(this->name()) + " n instructions");

  EventLog::getInstance().reportState(this->name(), "off");

  // Construct & init cpu
  memset(&cpu, 0, sizeof(struct CPU));
}

void CortexM0Cpu::end_of_elaboration() {
  SC_THREAD(process);
  SC_METHOD(powerOffChecks);
  sensitive << pwrOn.negedge_event();
  dont_initialize();
}

void CortexM0Cpu::process() {
  wait(SC_ZERO_TIME);  // Wait for start of simulation

  // Initialize CPU state
  cpu.debug = 1;

  // Execute the program
  while (true) {
    if (pwrOn.read() && m_run) {
      uint16_t insn;

      if ((cpu_get_pc() & 0x1) == 0) {
        spdlog::error("PC moved out of thumb mode: 0x{:08x}", cpu_get_pc());
        SC_REPORT_FATAL(this->name(), "PC moved out of thumb mode");
      }

      // Check for exceptions
      exceptionCheck();
      returningException.write(0);

      if (m_sleeping) {
        wait(sysTickIrq.value_changed_event() | nvicIrq.value_changed_event() |
             pwrOn.default_event());
      } else {
        // Handle breakpoints
        if (m_breakpoints.count((cpu_get_pc() & (~1u)) - 4) >
            0) {  // Hit breakpoint
          spdlog::info("@{:10s}: Breakpoint hit (0x{:08x})",
                       sc_core::sc_time_stamp().to_string(), cpu_get_pc());
          m_run = false;
          continue;
        }

        // Fetch next instruction
        simLoadInsn(cpu_get_pc(), &insn);
        m_instructionQueue.push_back(insn);

        // Decode & execute
        // (on real hw this is done in separate steps)
        insn = m_instructionQueue.front();
        m_instructionQueue.pop_front();

        if (insn == OPCODE_WFE || insn == OPCODE_WFI) {
          spdlog::info("{}: going to sleep", this->name());
          m_sleeping = true;
          EventLog::getInstance().reportState(this->name(), "sleep");
        } else {
          // Decode & execute
          takenBranch = 0;
          decode(insn);
          auto exCycles = exwbmem(insn);
          if (exCycles > 0) {
            // Extra cycles spent for special instructions.
            wait(clk->getPeriod() * exCycles);
          }
        }

        if (m_bubbles > 0) {
          m_bubbles--;
        }

        if (takenBranch) {
          flushPipeline();
        }

        // Advance PC if no jumps or exceptions
        if (!takenBranch) {
          cpu_set_pc(cpu_get_pc() + 0x2);
        }

        EventLog::getInstance().increment(m_nInstructionsEventId);

        if (m_doStep && (m_bubbles == 0)) {
          m_run = false;
          m_doStep = false;
        }
      }
    }

    if (!m_run) {
      waitForCommand();  // Stall simulation, waiting for gdb server interaction
    }

    if (m_run && (!pwrOn.read())) {
      EventLog::getInstance().reportState(this->name(), "off");
      wait(pwrOn.default_event());  // Wait for power
      EventLog::getInstance().reportState(this->name(), "on");
      reset();  // Reset CPU
    }
  }
}

void CortexM0Cpu::flushPipeline() {
  m_instructionQueue.clear();
  m_instructionQueue.push_back(OPCODE_NOP);
  m_instructionQueue.push_back(OPCODE_NOP);
  m_bubbles = 2;
}

void CortexM0Cpu::reset() {
  m_sleeping = false;
  takenBranch = false;

  // Initialize the special-purpose registers
  cpu.apsr = 0;        // No flags set
  cpu.ipsr = 0;        // No exception number
  cpu.espr = ESPR_T;   // Thumb mode
  cpu.primask = 0;     // No except priority boosting
  cpu.control = 0;     // Priv mode and main stack
  cpu.sp_main = 0;     // Stack pointer for exception handling
  cpu.sp_process = 0;  // Stack pointer for process

  // Clear the general purpose registers
  memset(cpu.gpr, 0, sizeof(cpu.gpr));

  // Set the reserved GPRs
  cpu.gpr[GPR_LR] = 0;

  // Load main stack pointer from the start of program memory
  uint8_t tmparr[4];
  uint32_t addr = ROM_START;
  readMem(addr, tmparr, 4);
  cpu_set_sp(0xFFFFFFFC & Utility::ttohl(Utility::packBytes(tmparr, 4)));
  cpu.sp_process = 0;

  // Set the program counter to the address of the reset exception vector
  addr += 4;
  readMem(addr, tmparr, 4);
  cpu_set_pc(Utility::ttohl(Utility::packBytes(tmparr, 4)));

  // No pending exceptions
  cpu.exceptmask = 0;

  cpu.debug = 1;
  cpu_mode_thread();

  // Initialize pipeline
  flushPipeline();
}

void CortexM0Cpu::exceptionCheck() {
  if (cpu_get_ipsr() != 0) {
    return;  // Not handling nested exceptions yet
  }
  // TODO check PRIMASK
  // TODO nested exception/preemption

  // Check if there is a pending exception
  uint32_t exceptionId = 0;
  if (sysTickIrq.read()) {
    exceptionId = 15;
  } else if (nvicIrq.read() != -1) {
    exceptionId = nvicIrq.read();
  }
  if (exceptionId != 0) {
    spdlog::info("{}: @{:s} handling exception with ID {}", this->name(),
                 sc_time_stamp().to_string(), exceptionId);
    m_sleeping = false;
    exceptionEnter(exceptionId);
  }
}

void CortexM0Cpu::exceptionEnter(const unsigned exceptionId) {
  // Save a snapshot of registers for checking correct irq handling
  std::copy(std::begin(cpu.gpr), std::end(cpu.gpr),
            std::begin(m_regsAtExceptEnter));
  m_regsAtExceptEnter[15] -= (4 - 2 * m_bubbles);  // Point to next valid instr.
  m_regsAtExceptEnter[16] = cpu_get_apsr();

  // Align stack frame to 8 bytes (to comply with AAPCS)
  // (SP is already aligned to 4 bytes)
  uint32_t frameAlign = (cpu_get_sp() & 0x4) != 0;
  cpu_set_sp((cpu_get_sp() - 0x20) & (~0x4));  // Pre-decrement SP
  uint32_t framePtr = cpu_get_sp();

  // Stack R0-R3, R12, R14, PC, xPSR
  uint8_t tmp[4];

  write32(framePtr, cpu_get_gpr(0));
  write32(framePtr + 4, cpu_get_gpr(1));
  write32(framePtr + 8, cpu_get_gpr(2));
  write32(framePtr + 12, cpu_get_gpr(3));
  write32(framePtr + 16, cpu_get_gpr(12));
  write32(framePtr + 20, cpu_get_lr());
  write32(framePtr + 24, cpu_get_pc() - (4 - 2 * m_bubbles));
  u32 psr = cpu_get_apsr();
  write32(framePtr + 28,
          ((psr & 0xFFFFFC00) | (frameAlign << 9) | (psr & 0x1FF)));

  // Encode the mode of the cpu at time of exception in LR value
  // (Set LR = EXC_RETURN)
  if (cpu_mode_is_handler()) {
    cpu_set_lr(0xFFFFFFF1);  // Nested exception
  } else if (cpu_stack_is_main()) {
    cpu_set_lr(0xFFFFFFF9);  // First exception, main stack
  } else {
    cpu_set_lr(0xFFFFFFFD);  // First exception, process stack
  }

  // Put the cpu in exception handling mode
  cpu_mode_handler();
  cpu_set_ipsr(exceptionId);
  cpu_stack_use_main();
  u32 handlerAddress = read32(ROM_START + 4 * exceptionId);

  activeException.write(exceptionId);
  cpu_set_pc(handlerAddress);
  flushPipeline();
}

void CortexM0Cpu::exceptionReturn(const uint32_t EXC_RETURN) {
  returningException.write(cpu_get_ipsr());

  // Return to the mode and stack that were active when the exception started
  // Error if handler mode and process stack, stops simulation
  switch (EXC_RETURN) {
    case 0xFFFFFFF1:  // Return to handler mode (nested interrupt)
      cpu_mode_handler();
      cpu_stack_use_main();
      break;
    case 0xFFFFFFF9:  // Return to thread mode using main stack
      cpu_mode_thread();
      cpu_stack_use_main();
      break;
    case 0xFFFFFFFD:  // Return to thread mode using process stack
      cpu_mode_thread();
      cpu_stack_use_process();
      break;
    default:
      spdlog::error("{}::exceptionReturn Invalid EXC_RETURN 0x{:0x}",
                    this->name(), EXC_RETURN);
      SC_REPORT_FATAL(this->name(), "Invalid EXC_RETURN");
      break;
  }

  cpu_set_ipsr(0);

  // Restore registers
  uint32_t framePtr = cpu_get_sp();
  cpu_set_gpr(0, read32(framePtr + 0 * 4));
  cpu_set_gpr(1, read32(framePtr + 1 * 4));
  cpu_set_gpr(2, read32(framePtr + 2 * 4));
  cpu_set_gpr(3, read32(framePtr + 3 * 4));
  cpu_set_gpr(12, read32(framePtr + 4 * 4));
  cpu_set_lr(read32(framePtr + 5 * 4));
  cpu_set_pc(read32(framePtr + 6 * 4));
  auto storedApsr = read32(framePtr + 7 * 4);
  cpu_set_apsr(storedApsr);

  cpu_set_sp((storedApsr & (1u << 9)) ? (framePtr + 0x20) | 0x4
                                      : framePtr + 0x20);

  // Set special-purpose registers
  cpu_set_apsr(cpu_get_apsr() & 0xF0000000);  // Clear invalid bits
  cpu_set_ipsr(0);                            // Ignore epsr
  takenBranch = 1;
  activeException.write(0);

  // Check correct state
  for (int i = 0; i < m_regsAtExceptEnter.size(); i++) {
    if (cpu.gpr[i] != m_regsAtExceptEnter[i]) {
      spdlog::error(
          "{}:exceptionReturn r{} has was not restored correctly, is "
          "0x{:08x}, should be 0x{:08x}",
          this->name(), i, cpu.gpr[i], m_regsAtExceptEnter[i]);
    }
  }
}

void CortexM0Cpu::read_cb(const uint32_t addr, uint8_t *const data,
                          const size_t bytelen) {
  m_ctx->readMem(addr, data, bytelen);
}

void CortexM0Cpu::write_cb(const uint32_t addr, uint8_t *const data,
                           const size_t bytelen) {
  m_ctx->writeMem(addr, data, bytelen);
}

void CortexM0Cpu::consume_cycles_cb(const size_t n) {
  sc_time delay = n * m_ctx->clk->getPeriod();
  m_ctx->wait(delay);
  EventLog::getInstance().increment(m_ctx->m_idleCyclesEvent);
}

void CortexM0Cpu::exception_return_cb(const uint32_t EXC_RETURN) {
  m_ctx->exceptionReturn(EXC_RETURN);
}

uint16_t CortexM0Cpu::next_pipeline_instr_cb() {
  return m_ctx->getNextPipelineInstr();
}

uint16_t CortexM0Cpu::getNextPipelineInstr() {
  uint16_t result = m_instructionQueue.front();
  m_instructionQueue.pop_front();
  return result;
}

void CortexM0Cpu::writeMem(const uint32_t addr, uint8_t *const data,
                           const size_t bytelen) {
  sc_time delay;
  tlm::tlm_generic_payload trans;

  delay = SC_ZERO_TIME;
  trans.set_address(addr);
  trans.set_data_length(bytelen);
  trans.set_data_ptr(data);
  trans.set_command(tlm::TLM_WRITE_COMMAND);
  iSocket->b_transport(trans, delay);

  if (trans.get_response_status() != tlm::TLM_OK_RESPONSE) {
    spdlog::error("{} Failed write to address 0x{:08x}.", this->name(), addr);
    sc_stop();
  }

  wait(delay);
}

void CortexM0Cpu::write32(const uint32_t addr, const uint32_t val) {
  uint8_t tmp[4];
  Utility::unpackBytes(tmp, Utility::htotl(val), 4);
  writeMem(addr, tmp, 4);
}

uint32_t CortexM0Cpu::read32(const uint32_t addr) {
  uint8_t tmp[4];
  readMem(addr, tmp, 4);
  return Utility::ttohl(Utility::packBytes(tmp, 4));
}

void CortexM0Cpu::readMem(const uint32_t addr, uint8_t *const data,
                          const size_t bytelen) {
  sc_time delay;
  tlm::tlm_generic_payload trans;

  delay = SC_ZERO_TIME;
  trans.set_address(addr);
  trans.set_data_length(bytelen);
  trans.set_data_ptr(data);
  trans.set_command(tlm::TLM_READ_COMMAND);
  iSocket->b_transport(trans, delay);
  if (trans.get_response_status() != tlm::TLM_OK_RESPONSE) {
    spdlog::error("{} Failed read from address 0x{:08x}.", this->name(), addr);
    sc_stop();
  }
  wait(delay);
}

uint32_t CortexM0Cpu::dbg_readReg(size_t addr) {
  if (addr == PC_REGNUM) {
    return (cpu_get_pc() & (~1u)) -
           4;  // Next instruction that will be executed.
  } else if (addr == CPSR_REGNUM) {
    return cpu.apsr;
  } else if (addr <= N_GPR) {
    return cpu_get_gpr(addr);
  } else {
    spdlog::warn("{}: dbg_readReg: invalid register number {}, returning 0",
                 this->name(), addr);
    return 0;
    // SC_REPORT_FATAL(this->name(), "Invalid register.");
  }
}

void CortexM0Cpu::dbg_writeReg(size_t addr, uint32_t data) {
  if (addr == PC_REGNUM) {
    cpu_set_pc((data + 4) | 1);  // Adjust for next instr to be fetched
  } else if (addr == CPSR_REGNUM) {
    spdlog::warn("writes to CPSR are ignored.");
  } else if (addr <= N_GPR) {
    cpu_set_gpr(addr, data);
  } else {
    spdlog::error("{}: dbg_writeReg: invalid register number {}", this->name(),
                  addr);
    SC_REPORT_FATAL(this->name(), "Invalid register.");
  }
}

void CortexM0Cpu::insertBreakpoint(unsigned addr) {
  m_breakpoints.insert(addr & (~1u));
}

void CortexM0Cpu::removeBreakpoint(unsigned addr) {
  m_breakpoints.erase(addr & (~1u));
}

void CortexM0Cpu::step(void) {
  m_doStep = true;
  m_run = true;
}

void CortexM0Cpu::stall(void) { m_run = false; }

void CortexM0Cpu::unstall(void) { m_run = true; }

bool CortexM0Cpu::isStalled(void) { return !m_run; }

void CortexM0Cpu::waitForCommand() {
  while (isStalled()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void CortexM0Cpu::powerOffChecks() {
  if (!m_sleeping) {
    spdlog::warn(
        "{} was active (not sleeping) at power-off, this could corrupt mcu "
        "state.",
        this->name());
  }
}

std::ostream &operator<<(std::ostream &os, const CortexM0Cpu &rhs) {
  // clang-format off
  os << "<CortexM0Cpu> " << rhs.name()
    << "\nPower: " << rhs.pwrOn.read()
    << "\nClock period: " << rhs.clk->getPeriod()
    << "\nactiveException: " << rhs.activeException.read()
    << "\nreturningException: " << rhs.returningException.read()
    << "\nNVIC irq: " << rhs.nvicIrq.read()
    << "\nSysTick irq: " << rhs.sysTickIrq.read()
    << "\nPipeline: [";
  for (const auto &i :  rhs.m_instructionQueue) {
    os << i << ", ";
  }
  os << "]\n";
  os << "\nCPU regs:";
  for (int i = 0; i <=12; i++) {
    os << fmt::format("\n\tR{:02d}: 0x{:08x}", i, cpu.gpr[i]);
  }
  os << fmt::format("\n\tSP:02d}: 0x{:08x}", cpu.gpr[13]);
  os << fmt::format("\n\tLR:02d}: 0x{:08x}", cpu.gpr[14]);
  os << fmt::format("\n\tPC[FETCH]:02d}: 0x{:08x}", cpu.gpr[15]);
  os << fmt::format("\n\tPC[EXECUTE]:02d}: 0x{:08x}", cpu.gpr[15] - 4);
  // clang-format on
  return os;
}
