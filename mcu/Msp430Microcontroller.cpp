/*
 * Copyright (c) 2019-2020, University of Southampton and Contributors.
 * All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include "include/fused.h"
#include "mcu/Cache.hpp"
#include "mcu/Microcontroller.hpp"
#include "mcu/Msp430Microcontroller.hpp"
#include "mcu/msp430fr5xx/Msp430Cpu.hpp"

extern "C" {
#include "mcu/msp430fr5xx/device_includes/msp430fr5994.h"
}

using namespace sc_core;

Msp430Microcontroller::Msp430Microcontroller(sc_module_name nm)
    : Microcontroller(nm), m_cpu("CPU", m_cycleTime, false, false), bus("bus") {
  /* ------ Memories ------ */
  cache = new Cache("cache", FRAM_START, 0xff7f, m_cycleTime);
  fram = new NonvolatileMemory("fram", FRAM_START, 0xff7f, m_cycleTime);
  vectors = new GenericMemory("vectors", 0xff80, 0xffff, m_cycleTime);
  sram = new VolatileMemory("sram", RAM_START, RAM_START + 0x2000 - 1,
                            m_cycleTime);

  /* ------ Peripherals ------ */
  std::vector<unsigned char> zeroRetval(0x800, 0);    // Read reg's as 0s
  std::vector<unsigned char> refgenRetVal(0x800, 0);  // Read most reg's as 0s
  refgenRetVal[OFS_REFCTL0 + 1] = static_cast<uint8_t>(REFGENRDY >> 8);
  refgenRetVal[OFS_REFCTL0] = static_cast<uint8_t>(REFGENRDY & 0xff);

  pmm = new PowerManagementModule("pmm", /*delay=*/m_cycleTime);
  adc = new Adc12("Adc", m_cycleTime);
  refgen = new DummyPeripheral("refgen", refgenRetVal, REF_A_BASE,
                               REF_A_BASE + 1, m_cycleTime);
  fram_ctl = new Frctl_a("FRAM_CTL_A", m_cycleTime);
  watchdog = new DummyPeripheral("watchdog", zeroRetval, WDT_A_BASE,
                                 WDT_A_BASE + 1, m_cycleTime);
  mon = new SimpleMonitor("mon", m_cycleTime);
  portJ = new DummyPeripheral("portJ", zeroRetval, PJ_BASE, PJ_BASE + 0x16,
                              m_cycleTime);
  portA = new DigitalIo("portA", PA_BASE, PA_BASE + 0x1f, m_cycleTime);
  portB = new DigitalIo("portB", PB_BASE, PB_BASE + 0x1f, m_cycleTime);
  portC = new DigitalIo("portC", PC_BASE, PC_BASE + 0x1f, m_cycleTime);
  portD = new DigitalIo("portD", PD_BASE, PD_BASE + 0x1f, m_cycleTime);
  cs = new ClockSystem("cs", CS_BASE, m_cycleTime);
  tima = new TimerA("tima", TA0_BASE, m_cycleTime);
  interruptArbiter = new InterruptArbiter<37>("interruptArbiter", false);
  mpy32 = new Mpy32("mpy32", MPY32_BASE, MPY32_BASE + 0x2e, m_cycleTime);
  eusci_b = new eUSCI_B("eUSCI_B", EUSCI_B0_BASE, EUSCI_B0_BASE + 0x2e, m_cycleTime);

  slaves.push_back(cache);
  slaves.push_back(fram_ctl);
  slaves.push_back(sram);
  slaves.push_back(vectors);
  slaves.push_back(adc);
  slaves.push_back(refgen);
  slaves.push_back(watchdog);
  slaves.push_back(portJ);
  slaves.push_back(portA);
  slaves.push_back(portB);
  slaves.push_back(portC);
  slaves.push_back(portD);
  slaves.push_back(pmm);
  slaves.push_back(cs);
  slaves.push_back(tima);
  slaves.push_back(mpy32);
  slaves.push_back(mon);
  slaves.push_back(eusci_b);

  // Sort slaves by address
  std::sort(slaves.begin(), slaves.end(), [](BusTarget *a, BusTarget *b) {
    return a->startAddress() < b->startAddress();
  });

  /* ------ Bind ------ */

  // IO
  for (int i = 0; i < 16; i++) {
    portA->pins[i].bind(ioPortA[i]);
    portB->pins[i].bind(ioPortB[i]);
    portC->pins[i].bind(ioPortC[i]);
    portD->pins[i].bind(ioPortD[i]);
  }

  // Clocks
  cs->aclk.bind(aclk);
  cs->smclk.bind(smclk);
  cs->mclk.bind(mclk);
  cs->vloclk.bind(vloclk);
  cs->modclk.bind(modclk);

  tima->aclk.bind(aclk);
  tima->smclk.bind(smclk);

  adc->modclk.bind(modclk);
  adc->aclk.bind(aclk);
  adc->mclk.bind(mclk);
  adc->smclk.bind(smclk);

  // Interrupts
  m_cpu.ira.bind(cpu_ira);
  m_cpu.irq.bind(cpu_irq);
  m_cpu.irqIdx.bind(cpu_irqIdx);
  m_cpu.iraConnected.bind(m_iraConnected);
  interruptArbiter->iraConnected.bind(m_iraConnected);
  interruptArbiter->irqOut.bind(cpu_irq);
  interruptArbiter->iraIn.bind(cpu_ira);
  interruptArbiter->idxOut.bind(cpu_irqIdx);

  tima->ira.bind(tima_ira);
  tima->irq.bind(tima_irq);
  interruptArbiter->irqIn[10].bind(tima_irq);
  interruptArbiter->iraOut[10].bind(tima_ira);

  pmm->ira.bind(pmm_ira);
  pmm->irq.bind(pmm_irq);
  interruptArbiter->irqIn[0].bind(pmm_irq);
  interruptArbiter->iraOut[0].bind(pmm_ira);

  adc->irq.bind(adc_irq);

  interruptArbiter->irqIn[9].bind(adc_irq);

  portA->irq[0].bind(port1_irq);
  portA->irq[1].bind(port2_irq);
  portB->irq[0].bind(port3_irq);
  portB->irq[1].bind(port4_irq);
  portC->irq[0].bind(port5_irq);
  portC->irq[1].bind(port6_irq);
  portD->irq[0].bind(port7_irq);
  portD->irq[1].bind(port8_irq);
  interruptArbiter->irqIn[16].bind(port1_irq);
  interruptArbiter->irqIn[19].bind(port2_irq);
  interruptArbiter->irqIn[22].bind(port3_irq);
  interruptArbiter->irqIn[23].bind(port4_irq);
  interruptArbiter->irqIn[28].bind(port5_irq);
  interruptArbiter->irqIn[29].bind(port6_irq);
  interruptArbiter->irqIn[35].bind(port7_irq);
  interruptArbiter->irqIn[36].bind(port8_irq);

  // Power
  pmm->staticPower.bind(staticPower);
  m_cpu.pwrOn.bind(nReset);
  for (const auto &s : slaves) {
    s->pwrOn.bind(nReset);
  }
  fram->pwrOn.bind(nReset);

  // Analog signals
  adc->vcc.bind(vcc);
  adc->vref.bind(vref);
  pmm->vcc.bind(vcc);

  // Write default const value for now.
  vref.write(2.0);

  // Miscellaneous
  fram_ctl->waitStates.bind(framWaitStates);
  fram->waitStates.bind(framWaitStates);

  // Bus
  bus.addInitiator();
  m_cpu.iSocket.bind(*bus.tSockets[0]);
  for (const auto &s : slaves) {
    auto port = bus.addTarget(*s);
    s->setBusSocket(port);
    bus.iSockets[port]->bind(s->tSocket);
  }

  // Fram and cache
  cache->iSocket.bind(fram->tSocket);
}

bool Msp430Microcontroller::dbgReadMem(uint8_t *out, size_t addr, size_t len) {
  tlm::tlm_generic_payload trans;

  trans.set_address(addr);
  trans.set_data_length(len);
  trans.set_data_ptr(out);
  trans.set_command(tlm::TLM_READ_COMMAND);
  unsigned int received = bus.transport_dbg(trans);

  return (received > 0);
}

bool Msp430Microcontroller::dbgWriteMem(uint8_t *src, size_t addr, size_t len) {
  tlm::tlm_generic_payload trans;

  trans.set_address(addr);
  trans.set_data_length(len);
  trans.set_data_ptr(src);
  trans.set_command(tlm::TLM_WRITE_COMMAND);
  unsigned int sent = bus.transport_dbg(trans);

  return (sent > 0);
}
