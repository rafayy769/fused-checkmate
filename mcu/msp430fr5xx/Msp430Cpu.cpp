/*
 * Copyright (c) 2019-2020, University of Southampton and Contributors.
 * All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <spdlog/spdlog.h>
#include <stdint.h>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <systemc>
#include <thread>
#include <tlm>
#include <array>
#include "libs/make_unique.hpp"
#include "mcu/msp430fr5xx/Msp430Cpu.hpp"
#include "ps/ConstantCurrentState.hpp"
#include "ps/ConstantEnergyEvent.hpp"
#include "utilities/Config.hpp"
#include "utilities/Utilities.hpp"

extern "C" {
#include "mcu/msp430fr5xx/device_includes/msp430fr5994.h"
}

using namespace sc_core;

Msp430Cpu::Msp430Cpu(const sc_module_name name, const bool logOperation,
                     const bool logInstructions)
    : sc_module(name),
      m_doLogOperation(logOperation),
      m_doLogInstructions(logInstructions) {
  iSocket.bind(*this);

  SC_THREAD(process);

  std::string odir = Config::get().getString("OutputDirectory");
  if (logOperation) {
    m_opsLogFile.open(odir + "/cpu_op.log");
  }

  if (logInstructions) {
    m_instrLogFile.open(odir + "/cpu_instructions.log");
  }
}

void Msp430Cpu::end_of_elaboration() {
  // Register events & states
  const std::string ops[] = {
      "ADD",  "ADDC", "AND", "BIC", "BIS", "BIT",  "CALL", "CMP", "DADD",
      "JC",   "JZ",   "JGE", "JL",  "JMP", "JN",   "JNC",  "JNZ", "MOV",
      "PUSH", "RETI", "RRA", "RRC", "SUB", "SUBC", "SWPB", "SXT", "XOR"};

  for (const auto &op : ops) {
    powerModelPort->registerEvent(
        this->name(), std::make_unique<ConstantEnergyEvent>(this->name(), op));
  }

  m_formatIEventId = powerModelPort->registerEvent(
      this->name(),
      std::make_unique<ConstantEnergyEvent>(this->name(), "formatI"));
  m_formatIIEventId = powerModelPort->registerEvent(
      this->name(),
      std::make_unique<ConstantEnergyEvent>(this->name(), "formatII"));
  m_formatIIIEventId = powerModelPort->registerEvent(
      this->name(),
      std::make_unique<ConstantEnergyEvent>(this->name(), "formatIII"));
  m_pcIsDestinationEventId = powerModelPort->registerEvent(
      this->name(),
      std::make_unique<ConstantEnergyEvent>(this->name(), "pc-is-dest"));
  m_irqEventId = powerModelPort->registerEvent(
      this->name(), std::make_unique<ConstantEnergyEvent>(this->name(), "irq"));
  m_idleCyclesEventId = powerModelPort->registerEvent(
      this->name(),
      std::make_unique<ConstantEnergyEvent>(this->name(), "idle cycles"));

  m_offStateId = powerModelPort->registerState(
      this->name(),
      std::make_unique<ConstantCurrentState>(this->name(), "off"));
  m_onStateId = powerModelPort->registerState(
      this->name(), std::make_unique<ConstantCurrentState>(this->name(), "on"));
  m_sleepStateId = powerModelPort->registerState(
      this->name(),
      std::make_unique<ConstantCurrentState>(this->name(), "sleep"));
}

void Msp430Cpu::reset(void) {
  for (auto &r : m_cpuRegs) {
    r = 0;
  }
  setSr(CPUOFF);  // Don't execute anything until we get the power-up NMI
}

void Msp430Cpu::process() {
  wait(SC_ZERO_TIME);  // Wait for start of simulation

  while (true) {  // Run emulator

    if (pwrOn.read() && m_run) {
      // Handle interrupts
      if (irq.read()) {
        powerModelPort->reportEvent(m_irqEventId);
        processInterrupt();
      }

      // Handle breakpoints
      if (m_breakpoints.count(getPc()) > 0) {  // Hit breakpoint
        std::cout << "@" << std::setw(10) << sc_core::sc_time_stamp()
                  << ": Breakpoint hit (0x" << std::hex << getPc() << ")!\n";
        m_run = false;
        continue;
      }

      if (getSr() & CPUOFF) {
        // Low-power mode -- don't execute instructions
        if (!m_sleeping) {
          powerModelPort->reportState(m_sleepStateId);
          m_sleeping = true;
        }
        wait(mclk->getPeriod());
      } else {
        // Normal mode -- execute instructions
        if (m_sleeping) {
          powerModelPort->reportState(m_onStateId);
          m_sleeping = false;
        }
        uint16_t opcode = fetch();
        static const uint16_t INST_RETI = 0x1300;
        if (m_doLogOperation && opcode == INST_RETI) {
          m_opsLogFile << "@" << sc_time_stamp() << ": RETI\n";
        }

        uint8_t instructionFmt = (opcode & 0xe000) >> 13;
        if (instructionFmt == 0) {
          executeSingleOpInstruction(opcode);
          powerModelPort->reportEvent(m_formatIIEventId);
        } else if (instructionFmt == 1) {
          executeConditionalJump(opcode);
          powerModelPort->reportEvent(m_formatIIIEventId);
        } else {
          executeDoubleOpInstruction(opcode);
          powerModelPort->reportEvent(m_formatIEventId);
        }
        if (m_doStep) {  // end single step
          m_run = false;
          m_doStep = false;
        }
      }
    }

    if (!m_run) {
      waitForCommand();  // Stall simulation, waiting for gdb server interaction
    }

    if (m_run && (!pwrOn.read())) {
      powerModelPort->reportState(m_offStateId);
      wait(pwrOn.posedge_event());  // Wait for power
      m_sleeping = false;
      reset();  // Reset
    }
  }
}

void Msp430Cpu::processInterrupt() {
  uint16_t addr;

  if (irqIdx.read() == 0) {  // Reset vector (BOR/PUC)
    addr = 0xfffe;

    // Acknowledge interrupt source
    ira.write(true);
    waitCycles(2);
    ira.write(false);

    // Clear all bits of SR except SCG0
    setSr(getSr() & (1u << 6));

    // Load reset vector
    setPc(read16(addr));

    if (m_doLogOperation) {
      using namespace std;
      m_opsLogFile << "@" << setw(10) << sc_time_stamp() << ": IRQHANDLER 0x"
                   << hex << setw(4) << addr << "\n";
    }
    powerModelPort->reportState(m_onStateId);
    m_sleeping = false;
  } else if ((getSr() & GIE) || irqIdx.read() < 3) {  // GIE or NMI
    // Push pc to stack
    setSp(getSp() - 2);
    write16(getSp(), getPc());

    // Push SR to stack
    setSp(getSp() - 2);
    write16(getSp(), getSr());

    // Capture interrupt vector address
    addr = 0xfffe - (2 * irqIdx.read());

    // IRQ flag (source) resets if the selected peripheral's IRA is
    // connected
    ira.write(true);
    wait(2 * mclk->getPeriod());
    ira.write(false);

    // Clear all bits of SR except SCG0
    setSr(getSr() & (1u << 6));

    // Load content of interrupt vector to PC
    setPc(read16(addr));

    if (m_doLogOperation) {
      using namespace std;
      m_opsLogFile << "@" << setw(10) << sc_time_stamp() << ": IRQHANDLER 0x"
                   << hex << setw(4) << addr << "\n";
    }
  } else {
    // Do nothing -- interrupts are disabled, and this was not an NMI
    return;
  }
}

void Msp430Cpu::writeMem(const uint32_t addr, uint8_t *const data,
                         const size_t bytelen) {
  sc_time delay;
  tlm::tlm_generic_payload trans;

  if (busStall.read()) {
    wait(busStall.negedge_event());
  }

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

void Msp430Cpu::readMem(const uint32_t addr, uint8_t *const data,
                        const size_t bytelen) {
  sc_time delay;
  tlm::tlm_generic_payload trans;

  if (busStall.read()) {
    wait(busStall.negedge_event());
  }

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

void Msp430Cpu::dbg_writeReg(uint16_t addr, uint16_t val) {
  assert(addr <= N_GPR);
  switch (addr) {
    case PC_REGNUM:
      setPc(val);
      break;
    case SP_REGNUM:
      setSp(val);
      break;
    case SR_REGNUM:
      setSr(val);
      break;
    case CG_REGNUM:
      // Special purpose constant generator register.
      SC_REPORT_FATAL(this->name(), "Attempt to write to r3 (constant gen");
      break;
    default:
      setGpr(addr, val);
      break;
  }
}

uint16_t Msp430Cpu::dbg_readReg(unsigned addr) {
  uint16_t result = 0;
  assert(addr <= N_GPR);
  switch (addr) {
    case PC_REGNUM:
      result = getPc();
      break;
    case SP_REGNUM:
      result = getSp();
      break;
    case SR_REGNUM:
      result = getSr();
      break;
    case CG_REGNUM:
      result = 0;  // Special case: Constant generator, return 0
      break;
    default:
      result = getGpr(addr);
      break;
  }

  return result;
}

void Msp430Cpu::insertBreakpoint(unsigned addr) { m_breakpoints.insert(addr); }

void Msp430Cpu::removeBreakpoint(unsigned addr) { m_breakpoints.erase(addr); }

void Msp430Cpu::step(void) {
  m_doStep = true;
  m_run = true;
}

void Msp430Cpu::stall(void) { m_run = false; }

void Msp430Cpu::unstall(void) { m_run = true; }

bool Msp430Cpu::isStalled(void) { return !m_run; }

uint16_t Msp430Cpu::fetch() {
  assert(getPc() % 2 == 0);
  uint16_t retval = read16(getPc());
  setPc(getPc() + 2);
  return retval;
}

uint16_t Msp430Cpu::read16(size_t addr) {
  uint8_t tmp[2];
  readMem(addr, tmp, 2);
  return Utility::ttohs(Utility::packBytes(tmp, 2));
}

uint8_t Msp430Cpu::read8(size_t addr) {
  uint8_t tmp;
  readMem(addr, &tmp, 1);
  return tmp;
}

void Msp430Cpu::write16(size_t addr, uint16_t val) {
  uint8_t tmp[2];
  Utility::unpackBytes(tmp, Utility::htots(val), 2);
  writeMem(addr, tmp, 2);
}

void Msp430Cpu::write8(size_t addr, uint8_t val) { writeMem(addr, &val, 1); }

void Msp430Cpu::writeback(operand_t operand) {
  writeback(operand.addr, operand.val, operand.inMem, operand.byteNotWord);
}

void Msp430Cpu::writeback(size_t addr, uint16_t val, bool toMemory,
                          bool byteNotWord) {
  if (toMemory) {
    if (byteNotWord) {
      write8(addr, static_cast<uint8_t>(val));
    } else {
      write16(addr, val);
    }
  } else {
    if (byteNotWord) {
      setGpr(addr, static_cast<uint8_t>(val));
    } else {
      setGpr(addr, val);
    }
  }
}

void Msp430Cpu::loadOperand(operand_t &operand) {
  if (operand.inMem) {
    if (operand.byteNotWord) {
      operand.val = read8(operand.addr);
    } else {
      operand.val = read16(operand.addr);
    }
  } else {
    if (operand.byteNotWord) {
      operand.val = static_cast<uint8_t>(getGpr(operand.addr));
    } else {
      operand.val = getGpr(operand.addr);
    }
  }
}

bool Msp430Cpu::isNegative(uint16_t val, bool byteNotWord) {
  if (byteNotWord) {
    return (val & (1u << 7));
  } else {
    return (val & (1u << 15));
  }
}

bool Msp430Cpu::isZero(uint16_t val, bool byteNotWord) {
  if (byteNotWord) {
    return static_cast<uint8_t>(val) == 0;
  } else {
    return static_cast<uint16_t>(val) == 0;
  }
}

Msp430Cpu::operand_t Msp430Cpu::getDestinationOperand(uint16_t opcode) {
  uint8_t destRegNum = opcode & 0x000f;
  bool ad = (opcode & (1u << 7));
  operand_t operand;
  std::memset(&operand, 0, sizeof(operand_t));
  operand.byteNotWord = (opcode & (1u << 6));
  if (ad) {
    if (destRegNum == CG_REGNUM) {
      // Invalid instruction
      spdlog::error(
          "getDestinationOperand:: Invalid destination register "
          "3(CG) in opcode 0x{:04x}",
          opcode);
      SC_REPORT_FATAL(this->name(), "Invalid destination register.");
    } else if (destRegNum == PC_REGNUM) {  // Symbolic
      operand.addr = getPc();              // Store old value of PC first
      operand.addr += fetch();             // Fetch offset (increments PC)
    } else if (destRegNum == SR_REGNUM) {  // Absolute
      operand.addr = fetch();              // Fetch absolute address
    } else {                               // Indexed
      operand.addr = getGpr(destRegNum);   // Get base address
      operand.addr += fetch();             // Fetch offset
    }
    operand.inMem = true;
  } else {  // Register direct
    operand.inMem = false;
    operand.addr = destRegNum;
  }

  operand.addr = static_cast<uint16_t>(operand.addr);  // Wrap to 16 bit
  if ((opcode & 0xf000) != OP_MOV) {  // Load value (if not MOV instruction)
    loadOperand(operand);
  }

  return operand;
}

bool Msp430Cpu::isSourceConstant(uint8_t as, uint8_t regIdx) {
  return ((regIdx == 3) || ((regIdx == 2) && (as >= 2)));
}

uint16_t Msp430Cpu::getSourceConstant(uint8_t as, uint8_t regIdx) {
  uint16_t result = 0;
  if (regIdx == CG_REGNUM) {
    if (as == 3) {
      result = static_cast<uint16_t>(-1u);
    } else {
      result = as;
    }
  } else if (regIdx == SR_REGNUM) {
    if (as == 2) {
      result = 4;
    } else if (as == 3) {
      result = 8;
    } else {
      spdlog::error(
          "getSourceConstant: invalid as 0x{:01x} for constant generator.", as);
      SC_REPORT_FATAL(this->name(),
                      "Invalid source address mode for constant generator");
    }
  }
  return result;
}

Msp430Cpu::operand_t Msp430Cpu::getSourceOperand(uint16_t opcode) {
  uint8_t as = (opcode & 0x0030) >> 4;  // address mode
  operand_t operand;
  std::memset(&operand, 0, sizeof(operand_t));
  operand.byteNotWord = (opcode & (1u << 6));

  uint8_t srcRegNum;
  if ((opcode & 0xf000) == 0x1000) {  // Single operand instruction
    srcRegNum = opcode & 0x000f;
  } else {  // Double operand instruction
    srcRegNum = (opcode & 0x0f00) >> 8;
  }

  // Special case -- constants
  if (isSourceConstant(as, srcRegNum)) {
    operand.inMem = false;
    operand.addr = srcRegNum;
    operand.val = getSourceConstant(as, srcRegNum);
    return operand;
  }

  // Register/memory access
  switch (as) {
    case 0:  // Register direct
      operand.inMem = false;
      operand.addr = srcRegNum;
      break;
    case 1:  // Indexed / Symbolic / Absolute
      operand.inMem = true;
      if (srcRegNum == PC_REGNUM) {         // Symbolic
        operand.addr = getPc();             // base address is PC
        operand.addr += fetch();            // fetch offset
      } else if (srcRegNum == SR_REGNUM) {  // Absolute
        operand.addr = fetch();             // Fetch absolute address
      } else {                              // Indexed
        operand.addr = getGpr(srcRegNum);   // Base addres
        operand.addr += fetch();            // Offset
      }
      break;
    case 2:  // Register indirect (@Rn)
      operand.inMem = true;
      operand.addr = getGpr(srcRegNum);
      break;
    case 3:  // Indirect autoincrement / immediate
      operand.inMem = true;
      if (srcRegNum == PC_REGNUM) {  // Immediate
        operand.addr = getPc();
        setPc(getPc() + 2);
      } else {  // Indirect autoincrement
        operand.addr = getGpr(srcRegNum);
        // Autoincrement
        if ((srcRegNum == SP_REGNUM) | (!operand.byteNotWord)) {
          setGpr(srcRegNum, getGpr(srcRegNum) + 2);
        } else {
          setGpr(srcRegNum, getGpr(srcRegNum) + 1);
        }
        break;
      }
  }

  operand.addr = static_cast<uint16_t>(operand.addr);  // Wrap to 16-bit
  loadOperand(operand);

  return operand;
}

void Msp430Cpu::executeConditionalJump(uint16_t opcode) {
  int32_t jumpOffset = opcode & 0x03ff;
  if (jumpOffset & (1u << 9)) {  // negative
    jumpOffset = jumpOffset - 0x03ff - 1;
  }
  jumpOffset *= 2;

  uint8_t condition = (opcode & 0x1C00) >> 10;
  bool doJump = false;
  switch (condition) {
    case 0:  // JNE / JNZ
      doJump = (getZeroFlag() == false);
      break;
    case 1:  // JEQ / JZ
      doJump = (getZeroFlag() == true);
      break;
    case 2:  // JNC / JLO
      doJump = (getCarryFlag() == false);
      break;
    case 3:  // JC  / JHS
      doJump = (getCarryFlag() == true);
      break;
    case 4:  // JN
      doJump = (getNegativeFlag() == true);
      break;
    case 5:  // JGE
      doJump = ((getNegativeFlag() ^ getOverflowFlag()) == false);
      break;
    case 6:  // JL
      doJump = ((getNegativeFlag() ^ getOverflowFlag()) == true);
      break;
    case 7:  // JMP
      doJump = true;
      break;
  }
  if (doJump) {
    setPc(getPc() + jumpOffset);
  }
  waitCycles(1);
}

void Msp430Cpu::executeSingleOpInstruction(uint16_t opcode) {
  unsigned instrIdx = (opcode & 0x0380) >> 7;
  operand_t operand = getSourceOperand(opcode);
  bool byteNotWord = operand.byteNotWord;
  uint32_t result;

  switch (instrIdx) {
    case 0:  // RRC Rotate right through carry
      result = operand.val >> 1;
      if (byteNotWord && getCarryFlag()) {
        result |= (1u << 7);
      } else if (getCarryFlag()) {
        result |= (1u << 15);
      }

      setCarryFlag(operand.val & 1u);
      setOverflowFlag(false);
      setNegativeFlag(isNegative(result, byteNotWord));
      setZeroFlag(isZero(result, byteNotWord));

      operand.val = result;
      writeback(operand);
      break;

    case 1:  // SWPB Swap bytes
      operand.val =
          ((operand.val & 0x00ff) << 8) | ((operand.val & 0xff00) >> 8);
      assert(operand.byteNotWord == false);
      writeback(operand);
      break;

    case 2:  // RRA Rotate right arithmetic
      if (operand.byteNotWord) {
        result = (operand.val & (1u << 7)) | (operand.val >> 1);
      } else {
        result = (operand.val & (1u << 15)) | (operand.val >> 1);
      }

      // Flags
      setCarryFlag(operand.val & 1u);
      setOverflowFlag(false);
      setNegativeFlag(isNegative(result, byteNotWord));
      setZeroFlag(isZero(result, byteNotWord));

      // Writeback
      operand.val = result;
      writeback(operand);
      break;

    case 3:  // SXT Sign extend
      if (operand.val & (1u << 7)) {
        operand.val |= 0xff00;  // Set upper byte
      } else {
        operand.val &= 0x00ff;  // Clear upper byte
      }

      // Flags
      setCarryFlag(!isZero(operand.val, false));
      setOverflowFlag(false);
      setNegativeFlag(isNegative(operand.val, false));
      setZeroFlag(isZero(operand.val, false));

      operand.byteNotWord = false;
      operand.addr &= ~(1u);  // Align to word
      writeback(operand);
      break;

    case 4:  // PUSH
      if (!operand.inMem) {
        // PUSH takes 3 cycles if operand is a register
        waitCycles(1);
      }
      setSp(getSp() - 2);
      operand.addr = getSp();
      operand.inMem = true;
      operand.byteNotWord = false;
      writeback(operand);
      break;

    case 5:  // CALL
      // Push PC
      setSp(getSp() - 2);
      writeback(getSp(), getPc(), /*toMemory=*/true, /*byteNotWord=*/false);

      // Jump
      setPc(operand.val);

      if (!operand.inMem) {
        // 4 cycles if operand is a register
        waitCycles(2);
      } else {
        // 4/5/6 cycles if operand is in memory
        waitCycles(1);
        // absolute mode requires one more cycle
        if ((opcode & 0x000f) == SR_REGNUM) {
          waitCycles(1);
        }
      }
      break;

    case 6:  // RETI Return from interrupt
      // Pop SR from stack
      setSr(read16(getSp()));
      setSp(getSp() + 2);

      // Pop PC from stack
      setPc(read16(getSp()));
      setSp(getSp() + 2);

      waitCycles(1);
      break;

    case 7:  // INVALID
      spdlog::error("executeSingleOpInstruction: Invalid opcode 0x{:04x}.",
                    opcode);
      SC_REPORT_FATAL(this->name(), "Invalid instruction");
  }
}

bool Msp430Cpu::isCarry(uint32_t a, uint32_t b, bool c, bool byteNotWord) {
  uint32_t res;
  if (byteNotWord) {
    res = (a & 0xff) + (b & 0xff) + c;
    return res > 0xff;
  } else {
    res = (a & 0xffff) + (b & 0xffff) + c;
    return res > 0xffff;
  }
}

bool Msp430Cpu::isOverflow(uint32_t a, uint32_t b, bool c, bool byteNotWord) {
  uint32_t res = a + b + c;
  bool aNeg = isNegative(a, byteNotWord);
  bool bNeg = isNegative(b, byteNotWord);
  bool resNeg = isNegative(res, byteNotWord);
  return (resNeg && (!aNeg) && (!bNeg)) | ((!resNeg) && aNeg && bNeg);
}

void Msp430Cpu::executeDoubleOpInstruction(uint16_t opcode) {
  unsigned instrIdx = (opcode & 0xf000) >> 12;
  operand_t srcOp = getSourceOperand(opcode);
  operand_t dstOp = getDestinationOperand(opcode);
  bool byteNotWord = srcOp.byteNotWord;
  uint32_t result;

  // Special case when PC is destination
  if (dstOp.addr == PC_REGNUM) {
    uint8_t as = (opcode & 0x0030) >> 4;
    uint8_t srcRegNum = (opcode & 0x0f00) >> 8;
    if ((as == 3) && (srcRegNum == PC_REGNUM)) {
      waitCycles(1);
    } else {
      waitCycles(2);
    }
  }

  switch (instrIdx) {
    case 4:  // MOV : dst = src
      dstOp.val = srcOp.val;
      writeback(dstOp);
      break;
    case 5:  // ADD : dst = src + dst
      result = srcOp.val + dstOp.val;

      setZeroFlag(isZero(result, dstOp.byteNotWord));
      setNegativeFlag(isNegative(result, dstOp.byteNotWord));
      setCarryFlag(isCarry(srcOp.val, dstOp.val, 0, byteNotWord));
      setOverflowFlag(isOverflow(srcOp.val, dstOp.val, 0, byteNotWord));

      dstOp.val = result;
      writeback(dstOp);
      break;
    case 6:  // ADDC : dst = src + dst + c
      result = srcOp.val + dstOp.val + getCarryFlag();

      setZeroFlag(isZero(result, dstOp.byteNotWord));
      setNegativeFlag(isNegative(result, dstOp.byteNotWord));
      setOverflowFlag(
          isOverflow(srcOp.val, dstOp.val, getCarryFlag(), byteNotWord));
      setCarryFlag(isCarry(srcOp.val, dstOp.val, getCarryFlag(), byteNotWord));
      dstOp.val = result;
      writeback(dstOp);
      break;
    case 7:  // SUBC : dst = dst + ~src + c
      result = (~srcOp.val) + getCarryFlag() + dstOp.val;

      setZeroFlag(isZero(result, byteNotWord));
      setNegativeFlag(isNegative(result, byteNotWord));
      setOverflowFlag(
          isOverflow(dstOp.val, ~srcOp.val, getCarryFlag(), byteNotWord));
      setCarryFlag(isCarry(dstOp.val, ~srcOp.val, getCarryFlag(), byteNotWord));

      dstOp.val = result;
      writeback(dstOp);
      break;
    case 8:  // SUB : dst = dst + ~src + 1
      result = (~srcOp.val) + dstOp.val + 1;

      setZeroFlag(isZero(result, byteNotWord));
      setNegativeFlag(isNegative(result, byteNotWord));
      setOverflowFlag(isOverflow(dstOp.val, ~srcOp.val, 1, byteNotWord));
      setCarryFlag(isCarry(dstOp.val, ~srcOp.val, 1, byteNotWord));

      dstOp.val = result;
      writeback(dstOp);
      break;
    case 9:  // CMP
      result = (~srcOp.val) + dstOp.val + 1;

      setZeroFlag(isZero(result, byteNotWord));
      setNegativeFlag(isNegative(result, byteNotWord));
      setOverflowFlag(isOverflow(dstOp.val, ~srcOp.val, 1, byteNotWord));
      setCarryFlag(isCarry(dstOp.val, ~srcOp.val, 1, byteNotWord));
      break;
    case 10:  // DADD
      SC_REPORT_FATAL(this->name(), "DADD instruction is not implemented.");
      break;
    case 11:  // BIT
      result = srcOp.val & dstOp.val;

      setZeroFlag(isZero(result, byteNotWord));
      setNegativeFlag(isNegative(result, byteNotWord));
      setOverflowFlag(false);
      setCarryFlag(!getZeroFlag());
      break;
    case 12:  // BIC : dst &= ~src
      dstOp.val &= ~srcOp.val;
      writeback(dstOp);
      break;
    case 13:  // BIS : dst |= src (logical OR)
      dstOp.val |= srcOp.val;
      writeback(dstOp);
      break;
    case 14:  // XOR : dst ^= src
      result = dstOp.val ^ srcOp.val;

      setZeroFlag(isZero(result, byteNotWord));
      setNegativeFlag(isNegative(result, byteNotWord));
      setOverflowFlag(isNegative(srcOp.val, byteNotWord) &&
                      isNegative(dstOp.val, byteNotWord));
      setCarryFlag(!getZeroFlag());

      dstOp.val = result;
      writeback(dstOp);
      break;
    case 15:  // AND : dst &= src
      result = dstOp.val & srcOp.val;

      setZeroFlag(isZero(result, byteNotWord));
      setNegativeFlag(isNegative(result, byteNotWord));
      setOverflowFlag(false);
      setCarryFlag(!isZero(result, byteNotWord));

      dstOp.val = result;
      writeback(dstOp);
      break;
  }
}

void Msp430Cpu::waitForCommand() {
  while (isStalled()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::ostream &operator<<(std::ostream &os, const Msp430Cpu &rhs) {
  std::array<const std::string, 16> registernames = {
      {"r0 (pc)", "r1 (sp)", "r2 (sr)", "r3 (cg)", "r4", "r5", "r6", "r7", "r8",
       "r9", "r10", "r11", "r12", "r13", "r14", "r15"}};
  // clang-format off
  os << "<Msp430Cpu> " << rhs.name()
    << "\nm_run (active) " << rhs.m_run
    << "\nm_sleeping " << rhs.m_sleeping
    << "\nclock period " << rhs.mclk->getPeriod()
    << "\nirq " << rhs.irq.read()
    << "\nira " << rhs.ira.read()
    << "\nirqIdx " << rhs.irqIdx.read()
    << "\nbusStall " << rhs.busStall.read()
    << "\ncpu registers:";
  for (int i = 0; i < rhs.m_cpuRegs.size(); ++i) {
    os << fmt::format("\n\t{:s}: 0x{:04x}", registernames[i], rhs.m_cpuRegs[i]);
  }

  os << "\nBreakpoints:";
  for (const auto &b : rhs.m_breakpoints) {
    os << fmt::format("\n\t0x{:04x}", b);
  }
  os << "\n";
  // clang-format on
  return os;
}
