/*
 * Copyright (c) 2019-2020, University of Southampton and Contributors.
 * All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <iostream>
#include <string>
#include <systemc-ams>
#include <systemc>
#include "utilities/Config.hpp"

// Load switch with voltage detector and override input.
// Consumes ext.dc uA  internally
SCA_TDF_MODULE(VoltageDetectorWithOverride) {
  // Consume enable
  // Consume input voltage and output current
  sca_tdf::sc_in<bool> forceOn{"forceOn"};
  sca_tdf::sc_in<double> i_out{"i_out"};
  sca_tdf::sca_in<double> v_in{"v_in"};

  // Produce output voltage and input current
  sca_tdf::sc_out<double> v_out{"v_out"};
  sca_tdf::sca_out<double> i_in{"i_in"};
  sca_tdf::sca_de::sca_out<sc_dt::sc_logic> v_warn{"v_warn"};

  void set_attributes(){};

  void initialize(){};

  void processing() {
    double crnt_v_in = v_in.read();
    if (forceOn.read() || (crnt_v_in > m_vOn) ||
        ((crnt_v_in > m_vOff) && m_isOn)) {
      i_in.write(i_out.read() + m_icc);
      v_out.write(crnt_v_in);
      m_isOn = true;
    } else {
      i_in.write(0.0 + m_icc);
      v_out.write(0.0);
      m_isOn = false;
    }

    // Issue voltage warning
    v_warn.write(sc_dt::sc_logic(crnt_v_in < m_vWarn));
  }

  void ac_processing(){};

  SCA_CTOR(VoltageDetectorWithOverride) {
    m_vOn = Config::get().getDouble("SVSVon");
    m_vOff = Config::get().getDouble("SVSVoff");
    m_icc = Config::get().getDouble("ext.dc");
    m_vWarn = Config::get().getDouble("VoltageWarning");
  };

 private:
  double m_vOn;    // On-threshold [V]
  double m_vOff;   // Off-threshold [V]
  double m_vWarn;  // Voltage warning threshold [V]
  double m_icc;    // Current draw of external circuitry
  bool m_isOn;
};

SCA_TDF_MODULE(CapacitorIdeal) {
  // Consume input and output current
  sca_tdf::sca_in<double> i_in{"i_in"};
  sca_tdf::sca_in<double> i_out{"i_out"};

  // Produce output voltage
  sca_tdf::sca_out<double> v{"v"};

  void set_attributes() { v.set_delay(1); };

  void initialize() {
    v.initialize(m_crntVoltage);
    m_timestep = get_timestep().to_seconds();
  };

  void processing() {
    m_crntVoltage += m_timestep * (i_in.read() - i_out.read()) / m_capacitance;
    if (m_crntVoltage <= 0) {
        m_crntVoltage = 0;
    }
    v.write(m_crntVoltage);
  }

  void ac_processing(){};

  SCA_CTOR(CapacitorIdeal) {
    m_capacitance = Config::get().getDouble("CapacitorValue");
    m_crntVoltage = Config::get().getDouble("CapacitorInitialVoltage");
  };

 private:
  double m_capacitance;
  double m_crntVoltage;
  double m_timestep;
};

SCA_TDF_MODULE(ConstantCurrentSupplyTDF) {
  // Consume voltage
  sca_tdf::sca_in<double> v;

  // Produce contant current
  sca_tdf::sca_out<double> i;

  void set_attributes() { set_timestep(m_timestep); }

  void initialize(){};

  void processing() {
    if ((v.read() + m_maxStepSize) < m_voltageLimit) {
      i.write(m_currentSetpoint);
    } else {
      i.write(0.0);
    }
  }

  void ac_processing(){};

  SCA_CTOR(ConstantCurrentSupplyTDF) {
    m_currentSetpoint = Config::get().getDouble("SupplyCurrentLimit");
    m_voltageLimit = Config::get().getDouble("SupplyVoltageLimit");
    m_timestep = sc_core::sc_time::from_seconds(
        Config::get().getDouble("PowerModelTimestep"));
    m_maxStepSize =
        m_timestep.to_seconds() *
        (m_currentSetpoint / Config::get().getDouble("CapacitorValue"));
  };

 private:
  double m_currentSetpoint;     // [Ampere]
  double m_voltageLimit;        // [Volt]
  double m_maxStepSize;         // [Volt] Handy to avoid overshoot
  sc_core::sc_time m_timestep;  // Evaluation timestep
};

// SCA_TDF_MODULE(VoltageTraceReplayTDF) {
//   // Consume voltage
//   sca_tdf::sca_in<double> v;

//   // Produce contant current
//   sca_tdf::sca_out<double> i;

//   void set_attributes() { set_timestep(m_timestep); }

//   void initialize(){};

//   void processing() {
//     // check if the end of the trace is reached
//     if (m_traceIndex >= m_voltageTrace.size()) {
//       m_traceIndex = 0;
//     }

//     double vcap = v.read();
//     if ((vcap + m_maxStepSize) < m_voltageLimit) {
//       i.write(deriveCurrentFromVoltage(m_voltageTrace[m_traceIndex], vcap));
//       m_traceIndex++;
//     } else {
//       i.write(0.0);
//     }
//   }

//   void ac_processing(){};

//   SCA_CTOR(VoltageTraceReplayTDF) {
//     m_traceFile = Config::get().getString("VoltageTraceFile");
//     m_currentSetpoint = Config::get().getDouble("SupplyCurrentLimit");
//     m_voltageLimit = Config::get().getDouble("SupplyVoltageLimit");
//     m_timestep = sc_core::sc_time::from_seconds(
//         Config::get().getDouble("PowerModelTimestep"));
//     m_maxStepSize =
//         m_timestep.to_seconds() *
//         (m_currentSetpoint / Config::get().getDouble("CapacitorValue"));
//     m_loadResistance = Config::get().getDouble("LoadResistance");

//     readTraceFile(m_traceFile);
//     m_traceIndex = 0;
//   };

//  private:

//   void readTraceFile(std::string path)
//   {
//     std::ifstream file(path);
//     if (!file.is_open()) {
//       throw std::runtime_error("Failed to open trace file: " + path);
//     }

//     std::string line;
//     while (std::getline(file, line)) {
//       try
//       {
//         m_voltageTrace.push_back(std::stod(line));
//       }
//       catch (const std::invalid_argument& e)
//       {
//         std::cerr << "Invalid argument while parsing trace: " << e.what() << '\n';
//       }
//     }
//   }
  
//   double deriveCurrentFromVoltage(double voltage, double vcap)
//   {
//     return voltage / m_loadResistance;
//   }

//   std::string m_traceFile;      // trace file path
//   std::vector<double> m_voltageTrace; // voltage trace
//   uint32_t m_traceIndex;        // current index in the trace
//   double m_currentSetpoint;     // [Ampere]
//   double m_voltageLimit;        // [Volt]
//   double m_maxStepSize;         // [Volt] Handy to avoid overshoot
//   double m_loadResistance;      // [Ohm]
//   sc_core::sc_time m_timestep;  // Evaluation timestep'
// };

SCA_TDF_MODULE(VoltageTraceReplayTDF) {
  // Consume voltage
  sca_tdf::sca_in<double> v;

  // Produce constant current
  sca_tdf::sca_out<double> i;

  void set_attributes() { set_timestep(m_timestep); }

  void initialize() {
    m_timeElapsed = 0.0; // Initialize the elapsed time
  }

  void processing() {
    // Update elapsed time
    m_timeElapsed += m_timestep.to_seconds();

    // Check if 1 ms has passed
    if (m_timeElapsed >= 1e-3) {  // 1 ms = 1e-3 seconds
      // Move to the next voltage trace value
      m_traceIndex++;

      // Reset elapsed time
      m_timeElapsed = 0.0;
    }

    // check if the end of the trace is reached
    if (m_traceIndex >= m_voltageTrace.size()) {
      m_traceIndex = 0;
    }

    double vcap = v.read();
    if ((vcap + m_maxStepSize) < m_voltageLimit) {
      i.write(deriveCurrentFromVoltage(m_voltageTrace[m_traceIndex], vcap));
    } else {
      i.write(0.0);
    }
  }

  void ac_processing() {}

  SCA_CTOR(VoltageTraceReplayTDF) {
    m_traceFile = Config::get().getString("VoltageTraceFile");
    m_currentSetpoint = Config::get().getDouble("SupplyCurrentLimit");
    m_voltageLimit = Config::get().getDouble("SupplyVoltageLimit");
    m_timestep = sc_core::sc_time::from_seconds(
        Config::get().getDouble("PowerModelTimestep"));
    m_maxStepSize =
        m_timestep.to_seconds() *
        (m_currentSetpoint / Config::get().getDouble("CapacitorValue"));
    m_loadResistance = Config::get().getDouble("LoadResistance");

    readTraceFile(m_traceFile);
    m_traceIndex = 0;
    m_timeElapsed = 0.0;  // Initialize the time elapsed
  };

private:

  void readTraceFile(std::string path) {
    std::ifstream file(path);
    if (!file.is_open()) {
      throw std::runtime_error("Failed to open trace file: " + path);
    }

    std::string line;
    while (std::getline(file, line)) {
      try {
        m_voltageTrace.push_back(std::stod(line));
      } catch (const std::invalid_argument& e) {
        std::cerr << "Invalid argument while parsing trace: " << e.what() << '\n';
      }
    }
  }

  double deriveCurrentFromVoltage(double voltage, double vcap) {
    return voltage / m_loadResistance;
  }

  std::string m_traceFile;          // trace file path
  std::vector<double> m_voltageTrace; // voltage trace
  uint32_t m_traceIndex;            // current index in the trace
  double m_currentSetpoint;         // [Ampere]
  double m_voltageLimit;            // [Volt]
  double m_maxStepSize;             // [Volt] Handy to avoid overshoot
  double m_loadResistance;          // [Ohm]
  sc_core::sc_time m_timestep;      // Evaluation timestep
  double m_timeElapsed;             // Tracks time to ensure each trace value is held for 1 ms
};


SC_MODULE(ExternalCircuitry) {
  sc_core::sc_in<bool> keepAlive{"keepAlive"};
  sc_core::sc_in<double> i_out{"i_out"};

  // TDF output converter ports
  sc_core::sc_out<double> vcc{"vcc"};
  sc_core::sc_out_resolved v_warn{"v_warn"};

  // Modules
  VoltageTraceReplayTDF supply{"supply"};
  CapacitorIdeal c{"c"};
  VoltageDetectorWithOverride svs{"svs"};

  SC_CTOR(ExternalCircuitry) {
    supply.i(i_supply);
    supply.v(v_cap);

    c.i_in(i_supply);
    c.v(v_cap);
    c.i_out(i_in_svs);

    svs.i_out(i_out);
    svs.i_in(i_in_svs);
    svs.v_in(v_cap);
    svs.v_out(vcc);
    svs.v_warn(v_warn);
    svs.forceOn(keepAlive);
  }

  // Signals
  sca_tdf::sca_signal<double> i_in_svs{"i_in_svs"};
  sca_tdf::sca_signal<double> i_supply{"i_supply"};
  sca_tdf::sca_signal<double> v_cap{"v_cap"};
};
