#include "ooo_cpu.h"
#include "set.h"
#include <iterator>
#include <map>
#include <set>
#include "uncore.h"
#include <fstream>

// out-of-order core
O3_CPU ooo_cpu[NUM_CPUS];
uint64_t current_core_cycle[NUM_CPUS], stall_cycle[NUM_CPUS];
uint32_t SCHEDULING_LATENCY = 0, EXEC_LATENCY = 0, DECODE_LATENCY = 0;
uint8_t TRACE_ENDS_STOP = 0;
int asid_index = 0;
extern uint32_t sum_mshr_stall;
ofstream file2("mshr_stall.txt");

int reg_instruction_pointer = REG_INSTRUCTION_POINTER, reg_flags = REG_FLAGS,
    reg_stack_pointer = REG_STACK_POINTER;

void O3_CPU::initialize_core() {}

void O3_CPU::read_from_trace() {
  // actual processors do not work like this but for easier implementation,
  // we read instruction traces and virtually add them in the ROB
  // note that these traces are not yet translated and fetched

  uint8_t continue_reading = 1;
  uint32_t num_reads = 0;
  instrs_to_read_this_cycle = FETCH_WIDTH;

  // first, read PIN trace
  while (continue_reading) {

    size_t instr_size =
        knob_cloudsuite ? sizeof(cloudsuite_instr) : sizeof(input_instr);

    if (knob_cloudsuite) {

      if (!fread(&current_cloudsuite_instr, instr_size, 1, trace_file)) {
        // reached end of file for this trace
        // cout << "*** Reached end of trace for Core: " << cpu << " Repeating
        // trace: " << trace_string << endl;
        // close the trace file and re-open it
        pclose(trace_file);
        trace_file = popen(gunzip_command, "r");
        if (trace_file == NULL) {
          cerr << endl
               << "*** CANNOT REOPEN TRACE FILE: " << trace_string << " ***"
               << endl;
          assert(0);
        }
      } else { // successfully read the trace

        // copy the instruction into the performance model's instruction format
        ooo_model_instr arch_instr;
        int num_reg_ops = 0, num_mem_ops = 0;

        arch_instr.instr_id = instr_unique_id;
        arch_instr.ip = current_cloudsuite_instr.ip;
        arch_instr.is_branch = current_cloudsuite_instr.is_branch;
        arch_instr.branch_taken = current_cloudsuite_instr.branch_taken;

        arch_instr.asid[0] = current_cloudsuite_instr.asid[0];
        arch_instr.asid[1] = current_cloudsuite_instr.asid[1];

        for (uint32_t i = 0; i < MAX_INSTR_DESTINATIONS; i++) {
          arch_instr.destination_registers[i] =
              current_cloudsuite_instr.destination_registers[i];
          arch_instr.destination_memory[i] =
              current_cloudsuite_instr.destination_memory[i];
          arch_instr.destination_virtual_address[i] =
              current_cloudsuite_instr.destination_memory[i];

          if (arch_instr.destination_registers[i])
            num_reg_ops++;
          if (arch_instr.destination_memory[i]) {
            num_mem_ops++;

            // update STA, this structure is required to execute store
            // instructios properly without deadlock
            if (num_mem_ops > 0) {
#ifdef SANITY_CHECK
              if (STA[STA_tail] < UINT64_MAX) {
                if (STA_head != STA_tail)
                  assert(0);
              }
#endif
              STA[STA_tail] = instr_unique_id;
              STA_tail++;

              if (STA_tail == STA_SIZE)
                STA_tail = 0;
            }
          }
        }

        for (int i = 0; i < NUM_INSTR_SOURCES; i++) {
          arch_instr.source_registers[i] =
              current_cloudsuite_instr.source_registers[i];
          arch_instr.source_memory[i] =
              current_cloudsuite_instr.source_memory[i];
          arch_instr.source_virtual_address[i] =
              current_cloudsuite_instr.source_memory[i];

          if (arch_instr.source_registers[i])
            num_reg_ops++;
          if (arch_instr.source_memory[i])
            num_mem_ops++;
        }

        arch_instr.num_reg_ops = num_reg_ops;
        arch_instr.num_mem_ops = num_mem_ops;
        if (num_mem_ops > 0)
          arch_instr.is_memory = 1;

        // add this instruction to the IFETCH_BUFFER
        if (IFETCH_BUFFER.occupancy < IFETCH_BUFFER.SIZE) {
          uint32_t ifetch_buffer_index = add_to_ifetch_buffer(&arch_instr);
          num_reads++;
          // handle branch prediction

          if (IFETCH_BUFFER.entry[ifetch_buffer_index].is_branch) {
            DP(if (warmup_complete[cpu]) {
              cout << "[BRANCH] instr_id: " << instr_unique_id << " ip: " << hex
                   << arch_instr.ip << dec
                   << " taken: " << +arch_instr.branch_taken << endl;
            });

            num_branch++;
            // handle branch prediction & branch predictor update

            uint8_t branch_prediction =
                predict_branch(IFETCH_BUFFER.entry[ifetch_buffer_index].ip);
            if (IFETCH_BUFFER.entry[ifetch_buffer_index].branch_taken !=
                branch_prediction) {
              branch_mispredictions++;
              total_rob_occupancy_at_branch_mispredict += ROB.occupancy;
              if (warmup_complete[cpu]) {
                fetch_stall = 1;
                instrs_to_read_this_cycle = 0;
                IFETCH_BUFFER.entry[ifetch_buffer_index].branch_mispredicted =
                    1;
              }
            } else {
              // correct prediction
              if (branch_prediction == 1) {
                // if correctly predicted taken, then we can't fetch anymore
                // instructions this cycle
                instrs_to_read_this_cycle = 0;
              }
            }

            last_branch_result(
                IFETCH_BUFFER.entry[ifetch_buffer_index].ip,
                IFETCH_BUFFER.entry[ifetch_buffer_index].branch_taken);
          }

          if ((num_reads >= instrs_to_read_this_cycle) ||
              (IFETCH_BUFFER.occupancy == IFETCH_BUFFER.SIZE))
            continue_reading = 0;
        }
        instr_unique_id++;
      }
    } else {
      input_instr trace_read_instr;
      if (!fread(&trace_read_instr, instr_size, 1, trace_file)) {
        // reached end of file for this trace
        cout << "*** Reached end of trace for Core: " << cpu
             << " Repeating trace: " << trace_string << endl;

        // close the trace file and re-open it
        pclose(trace_file);
        trace_file = popen(gunzip_command, "r");
        if (trace_file == NULL) {
          cerr << endl
               << "*** CANNOT REOPEN TRACE FILE: " << trace_string << " ***"
               << endl;
          assert(0);
        }
      } else { // successfully read the trace

        if (instr_unique_id == 0) {
          current_instr = next_instr = trace_read_instr;
        } else {
          current_instr = next_instr;
          next_instr = trace_read_instr;
        }

        // copy the instruction into the performance model's instruction format
        ooo_model_instr arch_instr;
        int num_reg_ops = 0, num_mem_ops = 0;

        arch_instr.instr_id = instr_unique_id;
        arch_instr.ip = current_instr.ip;
        arch_instr.is_branch = current_instr.is_branch;
        arch_instr.branch_taken = current_instr.branch_taken;

        arch_instr.asid[0] = cpu;
        arch_instr.asid[1] = cpu;

        bool reads_sp = false;
        bool writes_sp = false;
        bool reads_flags = false;
        bool reads_ip = false;
        bool writes_ip = false;
        bool reads_other = false;

        for (uint32_t i = 0; i < MAX_INSTR_DESTINATIONS; i++) {
          arch_instr.destination_registers[i] =
              current_instr.destination_registers[i];
          arch_instr.destination_memory[i] =
              current_instr.destination_memory[i];
          arch_instr.destination_virtual_address[i] =
              current_instr.destination_memory[i];

          if (arch_instr.destination_registers[i] == reg_stack_pointer)
            writes_sp = true;
          else if (arch_instr.destination_registers[i] ==
                   reg_instruction_pointer)
            writes_ip = true;

          if (arch_instr.destination_registers[i])
            num_reg_ops++;
          if (arch_instr.destination_memory[i]) {
            num_mem_ops++;
            // update STA, this structure is required to execute store
            // instructios properly without deadlock
            if (num_mem_ops > 0) {
#ifdef SANITY_CHECK
              if (STA[STA_tail] < UINT64_MAX) {
                if (STA_head != STA_tail)
                  assert(0);
              }
#endif
              STA[STA_tail] = instr_unique_id;
              STA_tail++;

              if (STA_tail == STA_SIZE)
                STA_tail = 0;
            }
          }
        }

        for (int i = 0; i < NUM_INSTR_SOURCES; i++) {
          arch_instr.source_registers[i] = current_instr.source_registers[i];
          arch_instr.source_memory[i] = current_instr.source_memory[i];
          arch_instr.source_virtual_address[i] = current_instr.source_memory[i];

          if (arch_instr.source_registers[i] == reg_stack_pointer)
            reads_sp = true;
          else if (arch_instr.source_registers[i] == reg_flags)
            reads_flags = true;
          else if (arch_instr.source_registers[i] == reg_instruction_pointer)
            reads_ip = true;
          else if (arch_instr.source_registers[i] != 0)
            reads_other = true;

          if (arch_instr.source_registers[i])
            num_reg_ops++;
          if (arch_instr.source_memory[i])
            num_mem_ops++;
        }

        arch_instr.num_reg_ops = num_reg_ops;
        arch_instr.num_mem_ops = num_mem_ops;
        if (num_mem_ops > 0)
          arch_instr.is_memory = 1;

        // determine what kind of branch this is, if any
        if (!reads_sp && !reads_flags && writes_ip && !reads_other) {
          // direct jump
          arch_instr.is_branch = 1;
          arch_instr.branch_taken = 1;
          arch_instr.branch_type = BRANCH_DIRECT_JUMP;
        } else if (!reads_sp && !reads_flags && writes_ip && reads_other) {
          // indirect branch
          arch_instr.is_branch = 1;
          arch_instr.branch_taken = 1;
          arch_instr.branch_type = BRANCH_INDIRECT;
        } else if (!reads_sp && reads_ip && !writes_sp && writes_ip &&
                   reads_flags && !reads_other) {
          // conditional branch
          arch_instr.is_branch = 1;
          arch_instr.branch_taken =
              arch_instr.branch_taken; // don't change this
          arch_instr.branch_type = BRANCH_CONDITIONAL;
        } else if (reads_sp && reads_ip && writes_sp && writes_ip &&
                   !reads_flags && !reads_other) {
          // direct call
          arch_instr.is_branch = 1;
          arch_instr.branch_taken = 1;
          arch_instr.branch_type = BRANCH_DIRECT_CALL;

        } else if (reads_sp && reads_ip && writes_sp && writes_ip &&
                   !reads_flags && reads_other) {
          // indirect call
          arch_instr.is_branch = 1;
          arch_instr.branch_taken = 1;
          arch_instr.branch_type = BRANCH_INDIRECT_CALL;
        } else if (reads_sp && !reads_ip && writes_sp && writes_ip) {
          // return
          arch_instr.is_branch = 1;
          arch_instr.branch_taken = 1;
          arch_instr.branch_type = BRANCH_RETURN;
        } else if (writes_ip) {
          // some other branch type that doesn't fit the above categories
          arch_instr.is_branch = 1;
          arch_instr.branch_taken =
              arch_instr.branch_taken; // don't change this
          arch_instr.branch_type = BRANCH_OTHER;
        }

        total_branch_types[arch_instr.branch_type]++;

        if ((arch_instr.is_branch == 1) && (arch_instr.branch_taken == 1)) {
          arch_instr.branch_target = next_instr.ip;
        }

        // add this instruction to the IFETCH_BUFFER
        if (IFETCH_BUFFER.occupancy < IFETCH_BUFFER.SIZE) {
          uint32_t ifetch_buffer_index = add_to_ifetch_buffer(&arch_instr);
          num_reads++;

          // handle branch prediction

          if (IFETCH_BUFFER.entry[ifetch_buffer_index].is_branch) {

            DP(if (warmup_complete[cpu]) {
              cout << "[BRANCH] instr_id: " << instr_unique_id << " ip: " << hex
                   << arch_instr.ip << dec
                   << " taken: " << +arch_instr.branch_taken << endl;
            });
            num_branch++;
            // handle branch prediction & branch predictor update
            uint8_t branch_prediction =
                predict_branch(IFETCH_BUFFER.entry[ifetch_buffer_index].ip);
            uint64_t predicted_branch_target =
                IFETCH_BUFFER.entry[ifetch_buffer_index].branch_target;
            if (branch_prediction == 0) {
              predicted_branch_target = 0;
            }
            // call code prefetcher every time the branch predictor is used
            l1i_prefetcher_branch_operate(
                IFETCH_BUFFER.entry[ifetch_buffer_index].ip,
                IFETCH_BUFFER.entry[ifetch_buffer_index].branch_type,
                predicted_branch_target);

#ifndef PERFECT_BTB
            if (arch_instr.branch_taken) {
              uint32_t btb_set = BTB.get_set(arch_instr.ip);
              int btb_way = BTB.get_way(arch_instr.ip, btb_set);
              if (btb_way == BTB_WAY) {
                BTB.sim_miss[cpu][arch_instr.branch_type - 1]++;
                BTB.sim_access[cpu][arch_instr.branch_type - 1]++;
                if (warmup_complete[cpu]) {
                  fetch_stall = 1;
                  instrs_to_read_this_cycle = 0;
                  IFETCH_BUFFER.entry[ifetch_buffer_index].btb_miss = 1;
                }
              } else {
                if (BTB.block[btb_set][btb_way].data ==
                    IFETCH_BUFFER.entry[ifetch_buffer_index].branch_target) {
                  BTB.sim_hit[cpu][arch_instr.branch_type - 1]++;
                  BTB.sim_access[cpu][arch_instr.branch_type - 1]++;
                  (BTB.*BTB.update_replacement_state)(cpu, btb_set, btb_way,
                                                      arch_instr.ip,
                                                      arch_instr.ip, 0, 0, 1);
                } else {
                  BTB.sim_miss[cpu][arch_instr.branch_type - 1]++;
                  BTB.sim_access[cpu][arch_instr.branch_type - 1]++;
                  if (warmup_complete[cpu]) {
                    fetch_stall = 1;
                    instrs_to_read_this_cycle = 0;
                    IFETCH_BUFFER.entry[ifetch_buffer_index].btb_miss = 1;
                  }
                }
              }
            } else {
              uint32_t btb_set = BTB.get_set(arch_instr.ip);
              int btb_way = BTB.get_way(arch_instr.ip, btb_set);

              if (btb_way != BTB_WAY) {
                BTB.sim_hit[cpu][arch_instr.branch_type - 1]++;
                BTB.sim_access[cpu][arch_instr.branch_type - 1]++;
                (BTB.*BTB.update_replacement_state)(cpu, btb_set, btb_way,
                                                    arch_instr.ip,
                                                    arch_instr.ip, 0, 0, 1);
              }
            }

#endif

            if (IFETCH_BUFFER.entry[ifetch_buffer_index].branch_taken !=
                branch_prediction) {
              branch_mispredictions++;
              total_rob_occupancy_at_branch_mispredict += ROB.occupancy;
              if (warmup_complete[cpu]) {
                fetch_stall = 1;
                instrs_to_read_this_cycle = 0;
                IFETCH_BUFFER.entry[ifetch_buffer_index].branch_mispredicted =
                    1;
              }
            } else {
              // correct prediction

              if (branch_prediction == 1) {
                // if correctly predicted taken, then we can't fetch anymore
                // instructions this cycle
                instrs_to_read_this_cycle = 0;
              }
            }

            last_branch_result(
                IFETCH_BUFFER.entry[ifetch_buffer_index].ip,
                IFETCH_BUFFER.entry[ifetch_buffer_index].branch_taken);
          }

          if ((num_reads >= instrs_to_read_this_cycle) ||
              (IFETCH_BUFFER.occupancy == IFETCH_BUFFER.SIZE))
            continue_reading = 0;
        }
        instr_unique_id++;
      }
    }
  }
}

uint32_t O3_CPU::add_to_rob(ooo_model_instr *arch_instr) {

  uint32_t index = ROB.tail;

  // sanity check
  if (ROB.entry[index].instr_id != 0) {
    cerr << "[ROB_ERROR] " << __func__ << " is not empty index: " << index;
    cerr << " instr_id: " << ROB.entry[index].instr_id << endl;
    assert(0);
  }

  ROB.entry[index] = *arch_instr;
  ROB.entry[index].event_cycle = current_core_cycle[cpu];

  ROB.occupancy++;
  ROB.tail++;
  if (ROB.tail >= ROB.SIZE)
    ROB.tail = 0;

    // DP ( if (warmup_complete[cpu]) {
    // cout << "[ROB] " <<  __func__ << " instr_id: " <<
    // ROB.entry[index].instr_id; cout << " ip: " << hex << ROB.entry[index].ip
    // << dec; cout << " head: " << ROB.head << " tail: " << ROB.tail << "
    // occupancy: " << ROB.occupancy; cout << " event: " <<
    // ROB.entry[index].event_cycle << " current: " << current_core_cycle[cpu] <<
    // endl; });

#ifdef SANITY_CHECK
  if (ROB.entry[index].ip == 0) {
    cerr << "[ROB_ERROR] " << __func__ << " ip is zero index: " << index;
    cerr << " instr_id: " << ROB.entry[index].instr_id
         << " ip: " << ROB.entry[index].ip << endl;
    assert(0);
  }
#endif

  return index;
}

uint32_t O3_CPU::add_to_ifetch_buffer(ooo_model_instr *arch_instr) {
  uint32_t index = IFETCH_BUFFER.tail;

  if (IFETCH_BUFFER.entry[index].instr_id != 0) {
    cerr << "[IFETCH_BUFFER_ERROR] " << __func__
         << " is not empty index: " << index;
    cerr << " instr_id: " << IFETCH_BUFFER.entry[index].instr_id << endl;
    assert(0);
  }

  IFETCH_BUFFER.entry[index] = *arch_instr;
  IFETCH_BUFFER.entry[index].event_cycle = current_core_cycle[cpu];

  IFETCH_BUFFER.entry[index].instruction_pa = 0;
  IFETCH_BUFFER.entry[index].translated = 0;
  IFETCH_BUFFER.entry[index].fetched = 0;

  IFETCH_BUFFER.occupancy++;
  IFETCH_BUFFER.tail++;

  if (IFETCH_BUFFER.tail >= IFETCH_BUFFER.SIZE) {
    IFETCH_BUFFER.tail = 0;
  }

  return index;
}

uint32_t O3_CPU::add_to_decode_buffer(ooo_model_instr *arch_instr) {
  uint32_t index = DECODE_BUFFER.tail;

  if (DECODE_BUFFER.entry[index].instr_id != 0) {
    cerr << "[DECODE_BUFFER_ERROR] " << __func__
         << " is not empty index: " << index;
    cerr << " instr_id: " << IFETCH_BUFFER.entry[index].instr_id << endl;
    assert(0);
  }

  DECODE_BUFFER.entry[index] = *arch_instr;
  DECODE_BUFFER.entry[index].event_cycle = current_core_cycle[cpu];

  DECODE_BUFFER.occupancy++;
  DECODE_BUFFER.tail++;
  if (DECODE_BUFFER.tail >= DECODE_BUFFER.SIZE) {
    DECODE_BUFFER.tail = 0;
  }

  return index;
}

uint32_t O3_CPU::check_rob(uint64_t instr_id) {
  if ((ROB.head == ROB.tail) && ROB.occupancy == 0)
    return ROB.SIZE;

  if (ROB.head < ROB.tail) {
    for (uint32_t i = ROB.head; i < ROB.tail; i++) {
      if (ROB.entry[i].instr_id == instr_id) {
        // DP ( if (warmup_complete[ROB.cpu]) {
        // cout << "[ROB] " << __func__ << " same instr_id: " <<
        // ROB.entry[i].instr_id; cout << " rob_index: " << i << endl; });
        return i;
      }
    }
  } else {
    for (uint32_t i = ROB.head; i < ROB.SIZE; i++) {
      if (ROB.entry[i].instr_id == instr_id) {
        // DP ( if (warmup_complete[cpu]) {
        // cout << "[ROB] " << __func__ << " same instr_id: " <<
        // ROB.entry[i].instr_id; cout << " rob_index: " << i << endl; });
        return i;
      }
    }
    for (uint32_t i = 0; i < ROB.tail; i++) {
      if (ROB.entry[i].instr_id == instr_id) {
        // DP ( if (warmup_complete[cpu]) {
        // cout << "[ROB] " << __func__ << " same instr_id: " <<
        // ROB.entry[i].instr_id; cout << " rob_index: " << i << endl; });
        return i;
      }
    }
  }

  cerr << "[ROB_ERROR] " << __func__ << " does not have any matching index! "
       << " head=" << ROB.head << " tail=" << ROB.tail << endl;
  cerr << " instr_id: " << instr_id << endl;
  assert(0);

  return ROB.SIZE;
}

void O3_CPU::fetch_instruction() {
  // TODO: can we model wrong path execusion?

  // if we had a branch mispredict, turn fetching back on after the branch
  // mispredict penalty
  if ((fetch_stall == 1) && (current_core_cycle[cpu] >= fetch_resume_cycle) &&
      (fetch_resume_cycle != 0)) {
    fetch_stall = 0;
    fetch_resume_cycle = 0;
  }

  if (IFETCH_BUFFER.occupancy == 0) {
    return;
  }

  // Neelu: From IFETCH buffer, requests will be sent to L1-I and it will take
  // care of sending the translation request to ITLB as L1 caches are VIPT in
  // this version.

  uint32_t index = IFETCH_BUFFER.head;
  for (uint32_t i = 0; i < IFETCH_BUFFER.SIZE; i++) {
    if (IFETCH_BUFFER.entry[index].ip == 0) {
      break;
    }

    if ((IFETCH_BUFFER.entry[index].fetched == 0)) {
      // add it to the L1-I's read queue
      PACKET fetch_packet;
      fetch_packet.instruction = 1;
      fetch_packet.is_data = 0;
      fetch_packet.fill_level = FILL_L1;
      fetch_packet.fill_l1i = 1;
      fetch_packet.cpu = cpu;
      fetch_packet.address = IFETCH_BUFFER.entry[index].ip >> 6;
      fetch_packet.full_addr = IFETCH_BUFFER.entry[index].ip;
      fetch_packet.full_virtual_address = IFETCH_BUFFER.entry[index].ip;
      fetch_packet.instr_id = 0;
      fetch_packet.rob_index = 0;
      fetch_packet.producer = 0;
      fetch_packet.ip = IFETCH_BUFFER.entry[index].ip;
      fetch_packet.type = LOAD;
      fetch_packet.asid[0] = 0;
      fetch_packet.asid[1] = 0;
      fetch_packet.event_cycle = current_core_cycle[cpu];

      int rq_index = L1I.add_rq(&fetch_packet);

      if (rq_index != -2) {
        // mark all instructions from this cache line as having been fetched
        for (uint32_t j = 0; j < IFETCH_BUFFER.SIZE; j++) {
          if (((IFETCH_BUFFER.entry[j].ip) >> 6) ==
              ((IFETCH_BUFFER.entry[index].ip) >> 6)) {
            IFETCH_BUFFER.entry[j].translated = COMPLETED;
            IFETCH_BUFFER.entry[j].fetched = INFLIGHT;
          }
        }
      }
    }

    index++;
    if (index >= IFETCH_BUFFER.SIZE) {
      index = 0;
    }

    if (index == IFETCH_BUFFER.head) {
      break;
    }
  }

  // send to decode stage

  bool decode_full = false;
  for (uint32_t i = 0; i < DECODE_WIDTH; i++) {
    if (decode_full) {
      break;
    }

    if (IFETCH_BUFFER.entry[IFETCH_BUFFER.head].ip == 0) {
      break;
    }

    if ((IFETCH_BUFFER.entry[IFETCH_BUFFER.head].translated == COMPLETED) &&
        (IFETCH_BUFFER.entry[IFETCH_BUFFER.head].fetched == COMPLETED)) {
      if (DECODE_BUFFER.occupancy < DECODE_BUFFER.SIZE) {
        uint32_t decode_index =
            add_to_decode_buffer(&IFETCH_BUFFER.entry[IFETCH_BUFFER.head]);
        DECODE_BUFFER.entry[decode_index].event_cycle = 0;

        ooo_model_instr empty_entry;
        IFETCH_BUFFER.entry[IFETCH_BUFFER.head] = empty_entry;
        IFETCH_BUFFER.head++;
        if (IFETCH_BUFFER.head >= IFETCH_BUFFER.SIZE) {
          IFETCH_BUFFER.head = 0;
        }
        IFETCH_BUFFER.occupancy--;
      } else {
        decode_full = true;
      }
    }

    index++;
    if (index >= IFETCH_BUFFER.SIZE) {
      index = 0;
    }
  }
} // Ending of fetch_instruction()

void O3_CPU::fill_btb(uint64_t trigger, uint64_t target) {
  uint32_t btb_set = BTB.get_set(trigger);
  int btb_way = BTB.get_way(trigger, btb_set);

  if (btb_way == BTB_WAY) {
    btb_way = (BTB.*BTB.find_victim)(cpu, 0, btb_set, BTB.block[btb_set],
                                     trigger, trigger, 0);
    (BTB.*BTB.update_replacement_state)(cpu, btb_set, btb_way, trigger, trigger,
                                        BTB.block[btb_set][btb_way].address, 0,
                                        0);
    BLOCK &entry = BTB.block[btb_set][btb_way];
    if (entry.valid == 0)
      entry.valid = 1;
    entry.dirty = 0;
    entry.tag = trigger;
    entry.address = trigger;
    entry.full_addr = trigger;
    entry.data = target;
    entry.ip = trigger;
    entry.cpu = cpu;
  } else {
    BTB.block[btb_set][btb_way].data = target;
  }
}

void O3_CPU::decode_and_dispatch() {
  // dispatch DECODE_WIDTH instructions that have decoded into the ROB
  uint32_t count_dispatches = 0;
  for (uint32_t i = 0; i < DECODE_BUFFER.SIZE; i++) {
    if (DECODE_BUFFER.entry[DECODE_BUFFER.head].ip == 0) {
      break;
    }

    if (((!warmup_complete[cpu]) && (ROB.occupancy < ROB.SIZE)) ||
        ((DECODE_BUFFER.entry[DECODE_BUFFER.head].event_cycle != 0) &&
         (DECODE_BUFFER.entry[DECODE_BUFFER.head].event_cycle <
          current_core_cycle[cpu]) &&
         (ROB.occupancy < ROB.SIZE))) {
      if (DECODE_BUFFER.entry[DECODE_BUFFER.head].btb_miss == 1 &&
          DECODE_BUFFER.entry[DECODE_BUFFER.head].branch_mispredicted == 0) {
        uint8_t branch_type =
            DECODE_BUFFER.entry[DECODE_BUFFER.head].branch_type;
        if (branch_type == BRANCH_DIRECT_JUMP ||
            branch_type == BRANCH_DIRECT_CALL ||
            (branch_type == BRANCH_CONDITIONAL)) {
          if (warmup_complete[cpu]) {
            fetch_resume_cycle =
                current_core_cycle[cpu] + 1; // Resume fetch from next cycle.
          }
          DECODE_BUFFER.entry[DECODE_BUFFER.head].btb_miss = 0;
          fill_btb(DECODE_BUFFER.entry[DECODE_BUFFER.head].ip,
                   DECODE_BUFFER.entry[DECODE_BUFFER.head].branch_target);
        }
      }

      // move this instruction to the ROB if there's space
      uint32_t rob_index = add_to_rob(&DECODE_BUFFER.entry[DECODE_BUFFER.head]);
      ROB.entry[rob_index].event_cycle = current_core_cycle[cpu];

      ooo_model_instr empty_entry;
      DECODE_BUFFER.entry[DECODE_BUFFER.head] = empty_entry;

      DECODE_BUFFER.head++;
      if (DECODE_BUFFER.head >= DECODE_BUFFER.SIZE) {
        DECODE_BUFFER.head = 0;
      }
      DECODE_BUFFER.occupancy--;

      count_dispatches++;
      if (count_dispatches >= DECODE_WIDTH) {
        break;
      }
    } else {
        break;
    }
  }

  // make new instructions pay decode penalty if they miss in the decoded
  // instruction cache
  uint32_t decode_index = DECODE_BUFFER.head;
  uint32_t count_decodes = 0;
  for (uint32_t i = 0; i < DECODE_BUFFER.SIZE; i++) {
    if (DECODE_BUFFER.entry[DECODE_BUFFER.head].ip == 0) {
      break;
    }

    if (DECODE_BUFFER.entry[decode_index].event_cycle == 0) {
      // apply decode latency
      DECODE_BUFFER.entry[decode_index].event_cycle =
          current_core_cycle[cpu] + DECODE_LATENCY;
      count_decodes++;
    }

    if (decode_index == DECODE_BUFFER.tail) {
      break;
    }
    decode_index++;
    if (decode_index >= DECODE_BUFFER.SIZE) {
      decode_index = 0;
    }

    if (count_decodes >= DECODE_WIDTH) {
      break;
    }
  }
} // Ending of decode_and_dispatch()

int O3_CPU::prefetch_code_line(uint64_t pf_v_addr) {
  if (pf_v_addr == 0) {
    cerr << "Cannot prefetch code line 0x0 !!!" << endl;
    assert(0);
  }

  L1I.pf_requested++;

  if (L1I.PQ.occupancy < L1I.PQ.SIZE) {

    PACKET pf_packet;
    pf_packet.instruction = 1; // this is a code prefetch
    pf_packet.is_data = 0;
    pf_packet.fill_level = FILL_L1;
    pf_packet.fill_l1i = 1;
    pf_packet.pf_origin_level = FILL_L1;
    pf_packet.cpu = cpu;
    // Neelu: assigning virtual addresses.

    pf_packet.address = pf_v_addr >> LOG2_BLOCK_SIZE;
    pf_packet.full_addr = pf_v_addr;

    // Neelu: Marking translated = 0
    pf_packet.translated = 0;
    pf_packet.full_physical_address = 0;

    pf_packet.ip = pf_v_addr;
    pf_packet.type = PREFETCH;
    pf_packet.event_cycle = current_core_cycle[cpu];

    L1I.add_pq(&pf_packet);
    L1I.pf_issued++;

    return 1;
  }

  return 0;
}

// TODO: When should we update ROB.schedule_event_cycle?
// I. Instruction is fetched
// II. Instruction is completed
// III. Instruction is retired
void O3_CPU::schedule_instruction() {
  if ((ROB.head == ROB.tail) && ROB.occupancy == 0)
    return;

  // execution is out-of-order but we have an in-order scheduling algorithm to
  // detect all RAW dependencies
  uint32_t limit = ROB.next_fetch[1];
  num_searched = 0;
  if (ROB.head < limit) {
    for (uint32_t i = ROB.head; i < limit; i++) {
      if ((ROB.entry[i].fetched != COMPLETED) ||
          (ROB.entry[i].event_cycle > current_core_cycle[cpu]) ||
          (num_searched >= SCHEDULER_SIZE))
        return;

      if (ROB.entry[i].scheduled == 0)
        do_scheduling(i);

      num_searched++;
    }
  } else {
    for (uint32_t i = ROB.head; i < ROB.SIZE; i++) {
      if ((ROB.entry[i].fetched != COMPLETED) ||
          (ROB.entry[i].event_cycle > current_core_cycle[cpu]) ||
          (num_searched >= SCHEDULER_SIZE))
        return;

      if (ROB.entry[i].scheduled == 0)
        do_scheduling(i);

      num_searched++;
    }
    for (uint32_t i = 0; i < limit; i++) {
      if ((ROB.entry[i].fetched != COMPLETED) ||
          (ROB.entry[i].event_cycle > current_core_cycle[cpu]) ||
          (num_searched >= SCHEDULER_SIZE))
        return;

      if (ROB.entry[i].scheduled == 0)
        do_scheduling(i);

      num_searched++;
    }
  }
}

void O3_CPU::do_scheduling(uint32_t rob_index) {
  ROB.entry[rob_index].reg_ready =
      1; // reg_ready will be reset to 0 if there is RAW dependency

  reg_dependency(rob_index);
  ROB.next_schedule = (rob_index == (ROB.SIZE - 1)) ? 0 : (rob_index + 1);

  if (ROB.entry[rob_index].is_memory)
    ROB.entry[rob_index].scheduled = INFLIGHT;
  else {
    ROB.entry[rob_index].scheduled = COMPLETED;

    // ADD LATENCY
    if (ROB.entry[rob_index].event_cycle < current_core_cycle[cpu])
      ROB.entry[rob_index].event_cycle =
          current_core_cycle[cpu] + SCHEDULING_LATENCY;
    else
      ROB.entry[rob_index].event_cycle += SCHEDULING_LATENCY;

    if (ROB.entry[rob_index].reg_ready) {

#ifdef SANITY_CHECK
      if (RTE1[RTE1_tail] < ROB_SIZE)
        assert(0);
#endif
      // remember this rob_index in the Ready-To-Execute array 1
      RTE1[RTE1_tail] = rob_index;

      // DP (if (warmup_complete[cpu]) {
      // cout << "[RTE1] " << __func__ << " instr_id: " <<
      // ROB.entry[rob_index].instr_id << " rob_index: " << rob_index << " is
      // added to RTE1"; cout << " head: " << RTE1_head << " tail: " << RTE1_tail
      // << endl; });

      RTE1_tail++;
      if (RTE1_tail == ROB_SIZE)
        RTE1_tail = 0;
    }
  }
}

void O3_CPU::reg_dependency(uint32_t rob_index) {

  // check RAW dependency
  int prior = rob_index - 1;
  if (prior < 0)
    prior = ROB.SIZE - 1;

  if (rob_index != ROB.head) {
    if ((int)ROB.head <= prior) {
      for (int i = prior; i >= (int)ROB.head; i--)
        if (ROB.entry[i].executed != COMPLETED) {
          for (uint32_t j = 0; j < NUM_INSTR_SOURCES; j++) {
            if (ROB.entry[rob_index].source_registers[j] &&
                (ROB.entry[rob_index].reg_RAW_checked[j] == 0))
              reg_RAW_dependency(i, rob_index, j);
          }
        }
    } else {
      for (int i = prior; i >= 0; i--)
        if (ROB.entry[i].executed != COMPLETED) {
          for (uint32_t j = 0; j < NUM_INSTR_SOURCES; j++) {
            if (ROB.entry[rob_index].source_registers[j] &&
                (ROB.entry[rob_index].reg_RAW_checked[j] == 0))
              reg_RAW_dependency(i, rob_index, j);
          }
        }
      for (int i = ROB.SIZE - 1; i >= (int)ROB.head; i--)
        if (ROB.entry[i].executed != COMPLETED) {
          for (uint32_t j = 0; j < NUM_INSTR_SOURCES; j++) {
            if (ROB.entry[rob_index].source_registers[j] &&
                (ROB.entry[rob_index].reg_RAW_checked[j] == 0))
              reg_RAW_dependency(i, rob_index, j);
          }
        }
    }
  }
}

void O3_CPU::reg_RAW_dependency(uint32_t prior, uint32_t current,
                                uint32_t source_index) {
  for (uint32_t i = 0; i < MAX_INSTR_DESTINATIONS; i++) {
    if (ROB.entry[prior].destination_registers[i] == 0)
      continue;

    if (ROB.entry[prior].destination_registers[i] ==
        ROB.entry[current].source_registers[source_index]) {

      // we need to mark this dependency in the ROB since the producer might not
      // be added in the store queue yet
      ROB.entry[prior].registers_instrs_depend_on_me.insert(
          current); // this load cannot be executed until the prior store gets
                    // executed
      ROB.entry[prior].registers_index_depend_on_me[source_index].insert(
          current); // this load cannot be executed until the prior store gets
                    // executed
      ROB.entry[prior].reg_RAW_producer = 1;

      ROB.entry[current].reg_ready = 0;
      ROB.entry[current].producer_id = ROB.entry[prior].instr_id;
      ROB.entry[current].num_reg_dependent++;
      ROB.entry[current].reg_RAW_checked[source_index] = 1;

      // DP (if(warmup_complete[cpu]) {
      // cout << "[ROB] " << __func__ << " instr_id: " <<
      // ROB.entry[current].instr_id << " is_memory: " <<
      // +ROB.entry[current].is_memory; cout << " RAW reg_index: " <<
      // +ROB.entry[current].source_registers[source_index]; cout << "
      // producer_id: " << ROB.entry[prior].instr_id << endl; });

      return;
    }
  }
}

void O3_CPU::execute_instruction() {
  if ((ROB.head == ROB.tail) && ROB.occupancy == 0)
    return;

  // out-of-order execution for non-memory instructions
  // memory instructions are handled by memory_instruction()
  uint32_t exec_issued = 0, num_iteration = 0;

  while (exec_issued < EXEC_WIDTH) {
    if (RTE0[RTE0_head] < ROB_SIZE) {
      uint32_t exec_index = RTE0[RTE0_head];
      if (ROB.entry[exec_index].event_cycle <= current_core_cycle[cpu]) {
        do_execution(exec_index);

        RTE0[RTE0_head] = ROB_SIZE;
        RTE0_head++;
        if (RTE0_head == ROB_SIZE)
          RTE0_head = 0;
        exec_issued++;
      }
    } else {
      break;
    }

    num_iteration++;
    if (num_iteration == (ROB_SIZE - 1))
      break;
  }

  num_iteration = 0;
  while (exec_issued < EXEC_WIDTH) {
    if (RTE1[RTE1_head] < ROB_SIZE) {
      uint32_t exec_index = RTE1[RTE1_head];
      if (ROB.entry[exec_index].event_cycle <= current_core_cycle[cpu]) {
        do_execution(exec_index);

        RTE1[RTE1_head] = ROB_SIZE;
        RTE1_head++;
        if (RTE1_head == ROB_SIZE)
          RTE1_head = 0;
        exec_issued++;
      }
    } else {
      break;
    }

    num_iteration++;
    if (num_iteration == (ROB_SIZE - 1))
      break;
  }
}

void O3_CPU::do_execution(uint32_t rob_index) {

  ROB.entry[rob_index].executed = INFLIGHT;

  // ADD LATENCY
  if (ROB.entry[rob_index].event_cycle < current_core_cycle[cpu])
    ROB.entry[rob_index].event_cycle = current_core_cycle[cpu] + EXEC_LATENCY;
  else
    ROB.entry[rob_index].event_cycle += EXEC_LATENCY;

  inflight_reg_executions++;
}

void O3_CPU::schedule_memory_instruction() {
  if ((ROB.head == ROB.tail) && ROB.occupancy == 0)
    return;

  // execution is out-of-order but we have an in-order scheduling algorithm to
  // detect all RAW dependencies
  uint32_t limit = ROB.next_schedule;
  num_searched = 0;
  if (ROB.head < limit) {
    for (uint32_t i = ROB.head; i < limit; i++) {

      if (ROB.entry[i].is_memory == 0)
        continue;

      if ((ROB.entry[i].fetched != COMPLETED) ||
          (ROB.entry[i].event_cycle > current_core_cycle[cpu]) ||
          (num_searched >= SCHEDULER_SIZE))
        break;

      if (ROB.entry[i].is_memory && ROB.entry[i].reg_ready &&
          (ROB.entry[i].scheduled == INFLIGHT))
        do_memory_scheduling(i);
    }
  } else {
    for (uint32_t i = ROB.head; i < ROB.SIZE; i++) {

      if (ROB.entry[i].is_memory == 0)
        continue;

      if ((ROB.entry[i].fetched != COMPLETED) ||
          (ROB.entry[i].event_cycle > current_core_cycle[cpu]) ||
          (num_searched >= SCHEDULER_SIZE))
        break;

      if (ROB.entry[i].is_memory && ROB.entry[i].reg_ready &&
          (ROB.entry[i].scheduled == INFLIGHT))
        do_memory_scheduling(i);
    }
    for (uint32_t i = 0; i < limit; i++) {

      if (ROB.entry[i].is_memory == 0)
        continue;

      if ((ROB.entry[i].fetched != COMPLETED) ||
          (ROB.entry[i].event_cycle > current_core_cycle[cpu]) ||
          (num_searched >= SCHEDULER_SIZE))
        break;

      if (ROB.entry[i].is_memory && ROB.entry[i].reg_ready &&
          (ROB.entry[i].scheduled == INFLIGHT))
        do_memory_scheduling(i);
    }
  }
}

void O3_CPU::execute_memory_instruction() {
  operate_lsq();
  operate_cache();
}

void O3_CPU::do_memory_scheduling(uint32_t rob_index) {
  uint32_t not_available = check_and_add_lsq(rob_index);
  if (not_available == 0) {
    ROB.entry[rob_index].scheduled = COMPLETED;
    if (ROB.entry[rob_index].executed ==
        0) // it could be already set to COMPLETED due to store-to-load
           // forwarding
      ROB.entry[rob_index].executed = INFLIGHT;

    // DP (if (warmup_complete[cpu]) {
    // cout << "[ROB] " << __func__ << " instr_id: " <<
    // ROB.entry[rob_index].instr_id << " rob_index: " << rob_index; cout << "
    // scheduled all num_mem_ops: " << ROB.entry[rob_index].num_mem_ops << endl;
    // });
  }

  num_searched++;
}

uint32_t O3_CPU::check_and_add_lsq(uint32_t rob_index) {
  uint32_t num_mem_ops = 0, num_added = 0;

  // load
  for (uint32_t i = 0; i < NUM_INSTR_SOURCES; i++) {
    if (ROB.entry[rob_index].source_memory[i]) {
      num_mem_ops++;
      if (ROB.entry[rob_index].source_added[i])
        num_added++;
      else if (LQ.occupancy < LQ.SIZE) {
        add_load_queue(rob_index, i);
        num_added++;
      } else {
        // DP(if(warmup_complete[cpu]) {
        // cout << "[LQ] " << __func__ << " instr_id: " <<
        // ROB.entry[rob_index].instr_id; cout << " cannot be added in the load
        // queue occupancy: " << LQ.occupancy << " cycle: " <<
        // current_core_cycle[cpu] << endl; });
      }
    }
  }

  // store
  for (uint32_t i = 0; i < MAX_INSTR_DESTINATIONS; i++) {
    if (ROB.entry[rob_index].destination_memory[i]) {
      num_mem_ops++;
      if (ROB.entry[rob_index].destination_added[i])
        num_added++;
      else if (SQ.occupancy < SQ.SIZE) {
        if (STA[STA_head] == ROB.entry[rob_index].instr_id) {
          add_store_queue(rob_index, i);
          num_added++;
        }
      } else {
        // DP(if(warmup_complete[cpu]) {
        // cout << "[SQ] " << __func__ << " instr_id: " <<
        // ROB.entry[rob_index].instr_id; cout << " cannot be added in the store
        // queue occupancy: " << SQ.occupancy << " cycle: " <<
        // current_core_cycle[cpu] << endl; });
      }
    }
  }

  if (num_added == num_mem_ops)
    return 0;

  uint32_t not_available = num_mem_ops - num_added;
  if (not_available > num_mem_ops) {
    cerr << "instr_id: " << ROB.entry[rob_index].instr_id << endl;
    assert(0);
  }

  return not_available;
}

void O3_CPU::add_load_queue(uint32_t rob_index, uint32_t data_index) {
  // search for an empty slot
  uint32_t lq_index = LQ.SIZE;
  for (uint32_t i = 0; i < LQ.SIZE; i++) {
    if (LQ.entry[i].virtual_address == 0) {
      lq_index = i;
      break;
    }
  }

  // sanity check
  if (lq_index == LQ.SIZE) {
    cerr << "instr_id: " << ROB.entry[rob_index].instr_id
         << " no empty slot in the load queue!!!" << endl;
    assert(0);
  }

  sim_load_gen++;

  // add it to the load queue
  ROB.entry[rob_index].lq_index[data_index] = lq_index;
  LQ.entry[lq_index].instr_id = ROB.entry[rob_index].instr_id;
  LQ.entry[lq_index].virtual_address =
      ROB.entry[rob_index].source_memory[data_index];
  LQ.entry[lq_index].ip = ROB.entry[rob_index].ip;
  LQ.entry[lq_index].data_index = data_index;
  LQ.entry[lq_index].rob_index = rob_index;
  LQ.entry[lq_index].asid[0] = ROB.entry[rob_index].asid[0];
  LQ.entry[lq_index].asid[1] = ROB.entry[rob_index].asid[1];
  LQ.entry[lq_index].event_cycle = current_core_cycle[cpu] + SCHEDULING_LATENCY;
  LQ.occupancy++;

  // check RAW dependency
  int prior = rob_index - 1;
  if (prior < 0)
    prior = ROB.SIZE - 1;

  if (rob_index != ROB.head) {
    if ((int)ROB.head <= prior) {
      for (int i = prior; i >= (int)ROB.head; i--) {
        if (LQ.entry[lq_index].producer_id != UINT64_MAX)
          break;

        mem_RAW_dependency(i, rob_index, data_index, lq_index);
      }
    } else {
      for (int i = prior; i >= 0; i--) {
        if (LQ.entry[lq_index].producer_id != UINT64_MAX)
          break;

        mem_RAW_dependency(i, rob_index, data_index, lq_index);
      }
      for (int i = ROB.SIZE - 1; i >= (int)ROB.head; i--) {
        if (LQ.entry[lq_index].producer_id != UINT64_MAX)
          break;

        mem_RAW_dependency(i, rob_index, data_index, lq_index);
      }
    }
  }

  // check
  // 1) if store-to-load forwarding is possible
  // 2) if there is WAR that are not correctly executed
  uint32_t forwarding_index = SQ.SIZE;
  for (uint32_t i = 0; i < SQ.SIZE; i++) {

    // skip empty slot
    if (SQ.entry[i].virtual_address == 0)
      continue;

    // forwarding should be done by the SQ entry that holds the same producer_id
    // from RAW dependency check
    if (SQ.entry[i].virtual_address ==
        LQ.entry[lq_index].virtual_address) { // store-to-load forwarding check

      // forwarding store is in the SQ
      if ((rob_index != ROB.head) &&
          (LQ.entry[lq_index].producer_id == SQ.entry[i].instr_id)) { // RAW
        forwarding_index = i;
        break; // should be break
      }

      if ((LQ.entry[lq_index].producer_id == UINT64_MAX) &&
          (LQ.entry[lq_index].instr_id <= SQ.entry[i].instr_id)) { // WAR
        // a load is about to be added in the load queue and we found a store
        // that is "logically later in the program order but already executed"
        // => this is not correctly executed WAR due to out-of-order execution,
        // this case is possible, for example 1) application is load intensive
        // and load queue is full 2) we have loads that can't be added in the
        // load queue 3) subsequent stores logically behind in the program order
        // are added in the store queue first

        // thanks to the store buffer, data is not written back to the memory
        // system until retirement also due to in-order retirement, this
        // "already executed store" cannot be retired until we finish the prior
        // load instruction if we detect WAR when a load is added in the load
        // queue, just let the load instruction to access the memory system no
        // need to mark any dependency because this is actually WAR not RAW

        // do not forward data from the store queue since this is WAR
        // just read correct data from data cache

        LQ.entry[lq_index].physical_address = 0;
        LQ.entry[lq_index].translated = 0;
        LQ.entry[lq_index].fetched = 0;

        // DP(if(warmup_complete[cpu]) {
        // cout << "[LQ] " << __func__ << " instr_id: " <<
        // LQ.entry[lq_index].instr_id << " reset fetched: " <<
        // +LQ.entry[lq_index].fetched; cout << " to obey WAR store instr_id: "
        // << SQ.entry[i].instr_id << " cycle: " << current_core_cycle[cpu] <<
        // endl; });
      }
    }
  }

  if (forwarding_index != SQ.SIZE) { // we have a store-to-load forwarding

    if ((SQ.entry[forwarding_index].fetched == COMPLETED) &&
        (SQ.entry[forwarding_index].event_cycle <= current_core_cycle[cpu])) {

      //@Vishal: count RAW forwarding
      sim_RAW_hits++;

      //@Vishal: VIPT, translation is not required, Just mark the entry as
      //fetched
      LQ.entry[lq_index].translated = COMPLETED;
      LQ.entry[lq_index].fetched = COMPLETED;

      uint32_t fwr_rob_index = LQ.entry[lq_index].rob_index;
      ROB.entry[fwr_rob_index].num_mem_ops--;
      ROB.entry[fwr_rob_index].event_cycle = current_core_cycle[cpu];
      if (ROB.entry[fwr_rob_index].num_mem_ops < 0) {
        cerr << "instr_id: " << ROB.entry[fwr_rob_index].instr_id << endl;
        assert(0);
      }
      if (ROB.entry[fwr_rob_index].num_mem_ops == 0)
        inflight_mem_executions++;

      // DP(if(warmup_complete[cpu]) {
      // cout << "[LQ] " << __func__ << " instr_id: " <<
      // LQ.entry[lq_index].instr_id << hex; cout << " full_addr: " <<
      // LQ.entry[lq_index].physical_address << dec << " is forwarded by store
      // instr_id: "; cout << SQ.entry[forwarding_index].instr_id << "
      // remain_num_ops: " << ROB.entry[fwr_rob_index].num_mem_ops << " cycle: "
      // << current_core_cycle[cpu] << endl; });

      release_load_queue(lq_index);
    } else
      ; // store is not executed yet, forwarding will be handled by
        // execute_store()
  }

  // succesfully added to the load queue
  ROB.entry[rob_index].source_added[data_index] = 1;

  if (LQ.entry[lq_index].virtual_address &&
      (LQ.entry[lq_index].producer_id ==
       UINT64_MAX)) { // not released and no forwarding
    RTL0[RTL0_tail] = lq_index;
    RTL0_tail++;
    if (RTL0_tail == LQ_SIZE)
      RTL0_tail = 0;

    // DP (if (warmup_complete[cpu]) {
    // cout << "[RTL0] " << __func__ << " instr_id: " <<
    // LQ.entry[lq_index].instr_id << " rob_index: " <<
    // LQ.entry[lq_index].rob_index << " is added to RTL0"; cout << " head: " <<
    // RTL0_head << " tail: " << RTL0_tail << endl; });
  }

  // DP(if(warmup_complete[cpu]) {
  // cout << "[LQ] " << __func__ << " instr_id: " <<
  // LQ.entry[lq_index].instr_id; cout << " is added in the LQ address: " << hex
  // << LQ.entry[lq_index].virtual_address << dec << " translated: " <<
  // +LQ.entry[lq_index].translated; cout << " fetched: " <<
  // +LQ.entry[lq_index].fetched << " index: " << lq_index << " occupancy: " <<
  // LQ.occupancy << " cycle: " << current_core_cycle[cpu] << endl; });
}

void O3_CPU::mem_RAW_dependency(uint32_t prior, uint32_t current,
                                uint32_t data_index, uint32_t lq_index) {
  for (uint32_t i = 0; i < MAX_INSTR_DESTINATIONS; i++) {
    if (ROB.entry[prior].destination_memory[i] == 0)
      continue;

    if (ROB.entry[prior].destination_memory[i] ==
        ROB.entry[current]
            .source_memory[data_index]) { //  store-to-load forwarding check

      // we need to mark this dependency in the ROB since the producer might not
      // be added in the store queue yet
      ROB.entry[prior].memory_instrs_depend_on_me.insert(
          current); // this load cannot be executed until the prior store gets
                    // executed
      ROB.entry[prior].is_producer = 1;
      LQ.entry[lq_index].producer_id = ROB.entry[prior].instr_id;
      LQ.entry[lq_index].translated = INFLIGHT;

      // DP (if(warmup_complete[cpu]) {
      // cout << "[LQ] " << __func__ << " RAW producer instr_id: " <<
      // ROB.entry[prior].instr_id << " consumer_id: " <<
      // ROB.entry[current].instr_id << " lq_index: " << lq_index; cout << hex <<
      // " address: " << ROB.entry[prior].destination_memory[i] << dec << endl;
      // });

      return;
    }
  }
}

void O3_CPU::add_store_queue(uint32_t rob_index, uint32_t data_index) {
  uint32_t sq_index = SQ.tail;
#ifdef SANITY_CHECK
  if (SQ.entry[sq_index].virtual_address)
    assert(0);
#endif

  sim_store_gen++;

  // add it to the store queue
  ROB.entry[rob_index].sq_index[data_index] = sq_index;
  SQ.entry[sq_index].instr_id = ROB.entry[rob_index].instr_id;
  SQ.entry[sq_index].virtual_address =
      ROB.entry[rob_index].destination_memory[data_index];
  SQ.entry[sq_index].ip = ROB.entry[rob_index].ip;
  SQ.entry[sq_index].data_index = data_index;
  SQ.entry[sq_index].rob_index = rob_index;
  SQ.entry[sq_index].asid[0] = ROB.entry[rob_index].asid[0];
  SQ.entry[sq_index].asid[1] = ROB.entry[rob_index].asid[1];
  SQ.entry[sq_index].event_cycle = current_core_cycle[cpu] + SCHEDULING_LATENCY;

  SQ.occupancy++;
  SQ.tail++;
  if (SQ.tail == SQ.SIZE)
    SQ.tail = 0;

  // succesfully added to the store queue
  ROB.entry[rob_index].destination_added[data_index] = 1;

  STA[STA_head] = UINT64_MAX;
  STA_head++;
  if (STA_head == STA_SIZE)
    STA_head = 0;

  RTS0[RTS0_tail] = sq_index;
  RTS0_tail++;
  if (RTS0_tail == SQ_SIZE)
    RTS0_tail = 0;

  // DP(if(warmup_complete[cpu]) {
  // cout << "[SQ] " << __func__ << " instr_id: " <<
  // SQ.entry[sq_index].instr_id; cout << " is added in the SQ translated: " <<
  // +SQ.entry[sq_index].translated << " fetched: " <<
  // +SQ.entry[sq_index].fetched << " is_producer: " <<
  // +ROB.entry[rob_index].is_producer; cout << " cycle: " <<
  // current_core_cycle[cpu] << endl; });
}

void O3_CPU::operate_lsq() {
  // handle store
  uint32_t store_issued = 0, num_iteration = 0;

  //@Vishal: VIPT Execute store without sending translation request to DTLB.
  while (store_issued < SQ_WIDTH) {
    if (RTS0[RTS0_head] < SQ_SIZE) {
      uint32_t sq_index = RTS0[RTS0_head];
      if (SQ.entry[sq_index].event_cycle <= current_core_cycle[cpu]) {
        execute_store(SQ.entry[sq_index].rob_index, sq_index,
                      SQ.entry[sq_index].data_index);

        RTS0[RTS0_head] = SQ_SIZE;
        RTS0_head++;
        if (RTS0_head == SQ_SIZE)
          RTS0_head = 0;

        store_issued++;
      }
    } else {
      break;
    }

    num_iteration++;
    if (num_iteration == (SQ_SIZE - 1))
      break;
  }

  unsigned load_issued = 0;
  num_iteration = 0;

  //@Vishal: VIPT. Send request to L1D.

  while (load_issued < LQ_WIDTH) {
    if (RTL0[RTL0_head] < LQ_SIZE) {
      uint32_t lq_index = RTL0[RTL0_head];
      if (LQ.entry[lq_index].event_cycle <= current_core_cycle[cpu]) {

        int rq_index = execute_load(LQ.entry[lq_index].rob_index, lq_index,
                                    LQ.entry[lq_index].data_index);

        if (rq_index != -2) {
          RTL0[RTL0_head] = LQ_SIZE;
          RTL0_head++;
          if (RTL0_head == LQ_SIZE)
            RTL0_head = 0;

          load_issued++;
        }
      }
    } else {
      break;
    }

    num_iteration++;
    if (num_iteration == (LQ_SIZE - 1))
      break;
  }
}

void O3_CPU::execute_store(uint32_t rob_index, uint32_t sq_index,
                           uint32_t data_index) {
  SQ.entry[sq_index].fetched = COMPLETED;
  SQ.entry[sq_index].event_cycle = current_core_cycle[cpu];

  ROB.entry[rob_index].num_mem_ops--;
  ROB.entry[rob_index].event_cycle = current_core_cycle[cpu];
  if (ROB.entry[rob_index].num_mem_ops < 0) {
    cerr << "instr_id: " << ROB.entry[rob_index].instr_id << endl;
    assert(0);
  }

  if (ROB.entry[rob_index].num_mem_ops == 0)
    inflight_mem_executions++;

  // DP (if (warmup_complete[cpu]) {
  // cout << "[SQ1] " << __func__ << " instr_id: " <<
  // SQ.entry[sq_index].instr_id << hex; cout << " full_address: " <<
  // SQ.entry[sq_index].physical_address << dec << " remain_mem_ops: " <<
  // ROB.entry[rob_index].num_mem_ops; cout << " event_cycle: " <<
  // SQ.entry[sq_index].event_cycle << endl; });

  // resolve RAW dependency after DTLB access
  // check if this store has dependent loads
  if (ROB.entry[rob_index].is_producer) {
    ITERATE_SET(dependent, ROB.entry[rob_index].memory_instrs_depend_on_me,
                ROB_SIZE) {
      // check if dependent loads are already added in the load queue
      for (uint32_t j = 0; j < NUM_INSTR_SOURCES;
           j++) { // which one is dependent?
        if (ROB.entry[dependent].source_memory[j] &&
            ROB.entry[dependent].source_added[j]) {
          if (ROB.entry[dependent].source_memory[j] ==
              SQ.entry[sq_index]
                  .virtual_address) { // this is required since a single
                                      // instruction can issue multiple loads

            //@Vishal: count RAW forwarding
            sim_RAW_hits++;

            // now we can resolve RAW dependency
            uint32_t lq_index = ROB.entry[dependent].lq_index[j];
#ifdef SANITY_CHECK
            if (lq_index >= LQ.SIZE)
              assert(0);
            if (LQ.entry[lq_index].producer_id != SQ.entry[sq_index].instr_id) {
              cerr << "[SQ2] " << __func__ << " lq_index: " << lq_index
                   << " producer_id: " << LQ.entry[lq_index].producer_id;
              cerr << " does not match to the store instr_id: "
                   << SQ.entry[sq_index].instr_id << endl;
              assert(0);
            }
#endif
            // update correspodning LQ entry
            // @Vishal: Dependent load can now get the data, translation is not
            // required
            LQ.entry[lq_index].translated = COMPLETED;
            LQ.entry[lq_index].fetched = COMPLETED;
            LQ.entry[lq_index].event_cycle = current_core_cycle[cpu];

            uint32_t fwr_rob_index = LQ.entry[lq_index].rob_index;
            ROB.entry[fwr_rob_index].num_mem_ops--;
            ROB.entry[fwr_rob_index].event_cycle = current_core_cycle[cpu];
#ifdef SANITY_CHECK
            if (ROB.entry[fwr_rob_index].num_mem_ops < 0) {
              cerr << "instr_id: " << ROB.entry[fwr_rob_index].instr_id << endl;
              assert(0);
            }
#endif
            if (ROB.entry[fwr_rob_index].num_mem_ops == 0)
              inflight_mem_executions++;

            // DP(if(warmup_complete[cpu]) {
            // cout << "[LQ3] " << __func__ << " instr_id: " <<
            // LQ.entry[lq_index].instr_id << hex; cout << " full_addr: " <<
            // LQ.entry[lq_index].physical_address << dec << " is forwarded by
            // store instr_id: "; cout << SQ.entry[sq_index].instr_id << "
            // remain_num_ops: " << ROB.entry[fwr_rob_index].num_mem_ops << "
            // cycle: " << current_core_cycle[cpu] << endl; });

            release_load_queue(lq_index);

            // clear dependency bit
            if (j == (NUM_INSTR_SOURCES - 1))
              ROB.entry[rob_index].memory_instrs_depend_on_me.insert(dependent);
          }
        }
      }
    }
  }
}

int O3_CPU::execute_load(uint32_t rob_index, uint32_t lq_index,
                         uint32_t data_index) {
  // add it to L1D
  PACKET data_packet;
  data_packet.fill_level = FILL_L1;
  data_packet.fill_l1d = 1;
  data_packet.cpu = cpu;
  data_packet.data_index = LQ.entry[lq_index].data_index;
  data_packet.lq_index = lq_index;

  //@Vishal: VIPT send virtual address instead of physical address
  data_packet.address = LQ.entry[lq_index].virtual_address >> LOG2_BLOCK_SIZE;
  data_packet.full_addr = LQ.entry[lq_index].virtual_address;
  data_packet.full_virtual_address = LQ.entry[lq_index].virtual_address;

  data_packet.instr_id = LQ.entry[lq_index].instr_id;
  data_packet.rob_index = LQ.entry[lq_index].rob_index;
  data_packet.ip = LQ.entry[lq_index].ip;

  data_packet.type = LOAD;
  data_packet.asid[0] = LQ.entry[lq_index].asid[0];
  data_packet.asid[1] = LQ.entry[lq_index].asid[1];
  data_packet.event_cycle = LQ.entry[lq_index].event_cycle;

  int rq_index = L1D.add_rq(&data_packet);

  if (rq_index == -2)
    return rq_index;
  else {
    LQ.entry[lq_index].fetched = INFLIGHT;

    sim_load_sent++;
  }

  return rq_index;
}

void O3_CPU::complete_execution(uint32_t rob_index) {
  if (ROB.entry[rob_index].is_memory == 0) {
    if ((ROB.entry[rob_index].executed == INFLIGHT) &&
        (ROB.entry[rob_index].event_cycle <= current_core_cycle[cpu])) {

      ROB.entry[rob_index].executed = COMPLETED;
      inflight_reg_executions--;
      completed_executions++;

      if (ROB.entry[rob_index].reg_RAW_producer)
        reg_RAW_release(rob_index);

      if (ROB.entry[rob_index].branch_mispredicted) {
        if (warmup_complete[cpu]) {
          fetch_resume_cycle = current_core_cycle[cpu] + 1;
        }
        if (ROB.entry[rob_index].branch_taken) {
          fill_btb(ROB.entry[rob_index].ip, ROB.entry[rob_index].branch_target);
        }
      }

      if (ROB.entry[rob_index].btb_miss &&
          ROB.entry[rob_index].branch_mispredicted == 0) {
        uint8_t branch_type = ROB.entry[rob_index].branch_type;
        assert(branch_type != BRANCH_DIRECT_JUMP &&
               branch_type != BRANCH_DIRECT_CALL &&
               branch_type != BRANCH_CONDITIONAL);
        if (warmup_complete[cpu]) {
          fetch_resume_cycle =
              current_core_cycle[cpu] + 1; // Resume fetch from next cycle.
        }
        fill_btb(ROB.entry[rob_index].ip, ROB.entry[rob_index].branch_target);
      }

      // DP(if(warmup_complete[cpu]) {
      // cout << "[ROB] " << __func__ << " instr_id: " <<
      // ROB.entry[rob_index].instr_id; cout << " branch_mispredicted: " <<
      // +ROB.entry[rob_index].branch_mispredicted << " fetch_stall: " <<
      // +fetch_stall; cout << " event: " << ROB.entry[rob_index].event_cycle <<
      // endl; });
    }
  } else {
    if (ROB.entry[rob_index].num_mem_ops == 0) {
      if ((ROB.entry[rob_index].executed == INFLIGHT) &&
          (ROB.entry[rob_index].event_cycle <= current_core_cycle[cpu])) {
        ROB.entry[rob_index].executed = COMPLETED;
        inflight_mem_executions--;
        completed_executions++;

        if (ROB.entry[rob_index].reg_RAW_producer)
          reg_RAW_release(rob_index);

        if (ROB.entry[rob_index].branch_mispredicted) {
          if (warmup_complete[cpu]) {
            fetch_resume_cycle = current_core_cycle[cpu] + 1;
          }
          if (ROB.entry[rob_index].branch_taken) {
            fill_btb(ROB.entry[rob_index].ip,
                     ROB.entry[rob_index].branch_target);
          }
        }

        if (ROB.entry[rob_index].btb_miss &&
            ROB.entry[rob_index].branch_mispredicted == 0) {
          uint8_t branch_type = ROB.entry[rob_index].branch_type;
          assert(branch_type != BRANCH_DIRECT_JUMP &&
                 branch_type != BRANCH_DIRECT_CALL &&
                 branch_type != BRANCH_CONDITIONAL);
          if (warmup_complete[cpu]) {
            fetch_resume_cycle =
                current_core_cycle[cpu] + 1; // Resume fetch from next cycle.
          }
          fill_btb(ROB.entry[rob_index].ip, ROB.entry[rob_index].branch_target);
        }

        // DP(if(warmup_complete[cpu]) {
        // cout << "[ROB] " << __func__ << " instr_id: " <<
        // ROB.entry[rob_index].instr_id; cout << " is_memory: " <<
        // +ROB.entry[rob_index].is_memory << " branch_mispredicted: " <<
        // +ROB.entry[rob_index].branch_mispredicted; cout << " fetch_stall: " <<
        // +fetch_stall << " event: " << ROB.entry[rob_index].event_cycle << "
        // current: " << current_core_cycle[cpu] << endl; });
      }
    }
  }
}

void O3_CPU::reg_RAW_release(uint32_t rob_index) {

  ITERATE_SET(i, ROB.entry[rob_index].registers_instrs_depend_on_me, ROB_SIZE) {
    for (uint32_t j = 0; j < NUM_INSTR_SOURCES; j++) {
      if (ROB.entry[rob_index].registers_index_depend_on_me[j].search(i)) {
        ROB.entry[i].num_reg_dependent--;

        if (ROB.entry[i].num_reg_dependent == 0) {
          ROB.entry[i].reg_ready = 1;
          if (ROB.entry[i].is_memory)
            ROB.entry[i].scheduled = INFLIGHT;
          else {
            ROB.entry[i].scheduled = COMPLETED;

#ifdef SANITY_CHECK
            if (RTE0[RTE0_tail] < ROB_SIZE)
              assert(0);
#endif
            // remember this rob_index in the Ready-To-Execute array 0
            RTE0[RTE0_tail] = i;

            // DP (if (warmup_complete[cpu]) {
            // cout << "[RTE0] " << __func__ << " instr_id: " <<
            // ROB.entry[i].instr_id << " rob_index: " << i << " is added to
            // RTE0"; cout << " head: " << RTE0_head << " tail: " << RTE0_tail <<
            // endl; });

            RTE0_tail++;
            if (RTE0_tail == ROB_SIZE)
              RTE0_tail = 0;
          }
        }

        // DP (if (warmup_complete[cpu]) {
        // cout << "[ROB] " << __func__ << " instr_id: " <<
        // ROB.entry[rob_index].instr_id << " releases instr_id: "; cout <<
        // ROB.entry[i].instr_id << " reg_index: " <<
        // +ROB.entry[i].source_registers[j] << " num_reg_dependent: " <<
        // ROB.entry[i].num_reg_dependent << " cycle: " <<
        // current_core_cycle[cpu] << endl; });
      }
    }
  }
}

void O3_CPU::operate_cache() {
  ITLB.operate();
  DTLB.operate();
  STLB.operate();
#ifdef INS_PAGE_TABLE_WALKER
  PTW.operate();
#endif
  L1I.operate();
  L1D.operate();
  L2C.operate();

  // also handle per-cycle prefetcher operation
  l1i_prefetcher_cycle_operate();
}

void O3_CPU::update_rob() {
  //@Vishal: VIPT ITLB processed entries will be handled by L1I cache.

  if (L1I.PROCESSED.occupancy &&
      (L1I.PROCESSED.entry[L1I.PROCESSED.head].event_cycle <=
       current_core_cycle[cpu]))
    complete_instr_fetch(&L1I.PROCESSED, 0);

  //@Vishal: VIPT DTLB processed entries will be handled by L1D cache

  if (L1D.PROCESSED.occupancy &&
      (L1D.PROCESSED.entry[L1D.PROCESSED.head].event_cycle <=
       current_core_cycle[cpu]))
    complete_data_fetch(&L1D.PROCESSED, 0);

  // update ROB entries with completed executions
  if ((inflight_reg_executions > 0) || (inflight_mem_executions > 0)) {
    if (ROB.head < ROB.tail) {
      for (uint32_t i = ROB.head; i < ROB.tail; i++)
        complete_execution(i);
    } else {
      for (uint32_t i = ROB.head; i < ROB.SIZE; i++)
        complete_execution(i);
      for (uint32_t i = 0; i < ROB.tail; i++)
        complete_execution(i);
    }
  }
}

void O3_CPU::complete_instr_fetch(PACKET_QUEUE *queue, uint8_t is_it_tlb) {
  //@Vishal: VIPT, TLB request should not be handled here
  assert(is_it_tlb == 0);

  // Neelu: IFETCH_BUFFER entries are marked translated and fetched. Since TLB
  // requests will not be handled here due to VIPT, no need to check for
  // is_it_tlb condition.

  uint32_t index = queue->head, rob_index = queue->entry[index].rob_index,
           num_fetched = 0;

  uint64_t complete_ip = queue->entry[index].ip;

  for (uint32_t j = 0; j < IFETCH_BUFFER.SIZE; j++) {
    if (((IFETCH_BUFFER.entry[j].ip) >> 6) == ((complete_ip) >> 6)) {
      IFETCH_BUFFER.entry[j].translated = COMPLETED;
      IFETCH_BUFFER.entry[j].fetched = COMPLETED;
    }
  }

  // remove this entry
  queue->remove_queue(&queue->entry[index]);

  return;
}

void O3_CPU::complete_data_fetch(PACKET_QUEUE *queue, uint8_t is_it_tlb) {

  //@Vishal: VIPT, TLB request should not be handled here
  assert(is_it_tlb == 0);

  uint32_t index = queue->head, rob_index = queue->entry[index].rob_index,
           sq_index = queue->entry[index].sq_index,
           lq_index = queue->entry[index].lq_index;

#ifdef SANITY_CHECK
  if (queue->entry[index].type != RFO) {
    // DP (if (warmup_complete[cpu]) {
    // cout << "queue->entry[index].full_addr = "<<
    // queue->entry[index].full_addr << endl; });
    if (rob_index != check_rob(queue->entry[index].instr_id)) {
      assert(0);
    }
  }
#endif

  // update ROB entry

  if (queue->entry[index].type == RFO)
    handle_merged_load(&queue->entry[index]);
  else {
#ifdef SANITY_CHECK
    if (queue->entry[index].store_merged)
      assert(0);
#endif
    LQ.entry[lq_index].fetched = COMPLETED;
    LQ.entry[lq_index].event_cycle = current_core_cycle[cpu];
    ROB.entry[rob_index].num_mem_ops--;
    ROB.entry[rob_index].event_cycle = queue->entry[index].event_cycle;

#ifdef SANITY_CHECK
    if (ROB.entry[rob_index].num_mem_ops < 0) {
      cerr << "instr_id: " << ROB.entry[rob_index].instr_id << endl;
      assert(0);
    }
#endif
    if (ROB.entry[rob_index].num_mem_ops == 0)
      inflight_mem_executions++;

    // DP (if (warmup_complete[cpu]) {
    // cout << "[ROB] " << __func__ << " load instr_id: " <<
    // LQ.entry[lq_index].instr_id; cout << " L1D_FETCH_DONE fetched: " <<
    // +LQ.entry[lq_index].fetched << hex << " address: " <<
    // (LQ.entry[lq_index].physical_address>>LOG2_BLOCK_SIZE); cout << "
    // full_addr: " << LQ.entry[lq_index].physical_address << dec << "
    // remain_mem_ops: " << ROB.entry[rob_index].num_mem_ops; cout << "
    // load_merged: " << +queue->entry[index].load_merged << " inflight_mem: " <<
    // inflight_mem_executions << endl; });

    release_load_queue(lq_index);
    handle_merged_load(&queue->entry[index]);
  }

  // remove this entry
  queue->remove_queue(&queue->entry[index]);
}

void O3_CPU::handle_merged_load(PACKET *provider) {
  ITERATE_SET(merged, provider->lq_index_depend_on_me, LQ.SIZE) {
    uint32_t merged_rob_index = LQ.entry[merged].rob_index;

    LQ.entry[merged].fetched = COMPLETED;
    LQ.entry[merged].event_cycle = current_core_cycle[cpu];

    ROB.entry[merged_rob_index].num_mem_ops--;
    ROB.entry[merged_rob_index].event_cycle = current_core_cycle[cpu];

#ifdef SANITY_CHECK
    if (ROB.entry[merged_rob_index].num_mem_ops < 0) {
      cerr << "instr_id: " << ROB.entry[merged_rob_index].instr_id
           << " rob_index: " << merged_rob_index << endl;
      assert(0);
    }
#endif
    if (ROB.entry[merged_rob_index].num_mem_ops == 0)
      inflight_mem_executions++;

    // DP (if (warmup_complete[cpu]) {
    // cout << "[ROB] " << __func__ << " load instr_id: " <<
    // LQ.entry[merged].instr_id; cout << " L1D_FETCH_DONE translation: " <<
    // +LQ.entry[merged].translated << hex << " address: " <<
    // (LQ.entry[merged].physical_address>>LOG2_BLOCK_SIZE); cout << " full_addr:
    // " << LQ.entry[merged].physical_address << dec << " by instr_id: " <<
    // +provider->instr_id; cout << " remain_mem_ops: " <<
    // ROB.entry[merged_rob_index].num_mem_ops << endl; });

    release_load_queue(merged);
  }
}

void O3_CPU::release_load_queue(uint32_t lq_index) {
  // release LQ entries
  // DP ( if (warmup_complete[cpu]) {
  // cout << "[LQ] " << __func__ << " instr_id: " << LQ.entry[lq_index].instr_id
  // << " releases lq_index: " << lq_index; cout << hex << " full_addr: " <<
  // LQ.entry[lq_index].physical_address << dec << endl; });

  LSQ_ENTRY empty_entry;
  LQ.entry[lq_index] = empty_entry;
  LQ.occupancy--;
}

void O3_CPU::retire_rob() {
  for (uint32_t n = 0; n < RETIRE_WIDTH; n++) {
    if (ROB.entry[ROB.head].ip == 0)
      return;

    // retire is in-order
    if (ROB.entry[ROB.head].executed != COMPLETED) {

      // DP ( if (warmup_complete[cpu]) {
      // cout << "[ROB] " << __func__ << " instr_id: " <<
      // ROB.entry[ROB.head].instr_id << " head: " << ROB.head << " is not
      // executed yet" << endl; });
     
#ifdef ROB_STALL_STATS 
      uint64_t curr_instr_id = ROB.entry[ROB.head].instr_id;

      total_stall.insert(curr_instr_id);

      for (uint32_t i=0; i< STLB.MSHR_SIZE; i++){
        if (STLB.MSHR.entry[i].instr_id == curr_instr_id){
          stlb_miss_stall.insert(curr_instr_id);
	}
      }

      for (uint32_t i=0; i< L1D.MSHR_SIZE; i++){
        if (L1D.MSHR.entry[i].instr_id == curr_instr_id){
	  if (L1D.MSHR.entry[i].type == LOAD_TRANSLATION){
            l1d_tr_miss_stall.insert(curr_instr_id);
	  } else if(L1D.MSHR.entry[i].type == LOAD) {
            l1d_load_miss_stall.insert(curr_instr_id);		  
	  }
	}
      }

      for (uint32_t i=0; i< L2C.MSHR_SIZE; i++){
        if (L2C.MSHR.entry[i].instr_id == curr_instr_id){
          if (L2C.MSHR.entry[i].type == LOAD_TRANSLATION){
            l2c_tr_miss_stall.insert(curr_instr_id);
          } else if(L2C.MSHR.entry[i].type == LOAD) {
            l2c_load_miss_stall.insert(curr_instr_id);
          }
        }
      }


      for (uint32_t i=0; i< uncore.LLC.MSHR_SIZE; i++){
        if (uncore.LLC.MSHR.entry[i].instr_id == curr_instr_id){
          if (uncore.LLC.MSHR.entry[i].type == LOAD_TRANSLATION){
            llc_tr_miss_stall.insert(curr_instr_id);
          } else if(uncore.LLC.MSHR.entry[i].type == LOAD) {
            llc_load_miss_stall.insert(curr_instr_id);
          }
        }
      }

#endif 

      return;
    }

    if (ROB.entry[ROB.head].event_cycle >  current_core_cycle[cpu]){
      return;
    }

    // statistic current IPC
    if (ROB.entry[ROB.head].instr_id % 100 == 0) {
      if (last_count_cycle != 0)
        num_cycles_per_kilo_instruction = current_core_cycle[cpu] - last_count_cycle;
      last_count_cycle = current_core_cycle[cpu];
    }


    // check store instruction
    uint32_t num_store = 0;
    for (uint32_t i = 0; i < MAX_INSTR_DESTINATIONS; i++) {
      if (ROB.entry[ROB.head].destination_memory[i])
        num_store++;
    }

    if (num_store) {
      if ((L1D.WQ.occupancy + num_store) <= L1D.WQ.SIZE) {
        for (uint32_t i = 0; i < MAX_INSTR_DESTINATIONS; i++) {
          if (ROB.entry[ROB.head].destination_memory[i]) {

            PACKET data_packet;
            uint32_t sq_index = ROB.entry[ROB.head].sq_index[i];

            // sq_index and rob_index are no longer available after retirement
            // but we pass this information to avoid segmentation fault
            data_packet.fill_level = FILL_L1;
            data_packet.fill_l1d = 1;
            data_packet.cpu = cpu;
            data_packet.data_index = SQ.entry[sq_index].data_index;
            data_packet.sq_index = sq_index;

            //@Vishal: VIPT, send virtual address

            data_packet.address =
                SQ.entry[sq_index].virtual_address >> LOG2_BLOCK_SIZE;
            data_packet.full_addr = SQ.entry[sq_index].virtual_address;
            data_packet.full_virtual_address =
                SQ.entry[sq_index].virtual_address;

            data_packet.instr_id = SQ.entry[sq_index].instr_id;
            data_packet.rob_index = SQ.entry[sq_index].rob_index;
            data_packet.ip = SQ.entry[sq_index].ip;
            data_packet.type = RFO;
            data_packet.asid[0] = SQ.entry[sq_index].asid[0];
            data_packet.asid[1] = SQ.entry[sq_index].asid[1];
            data_packet.event_cycle = current_core_cycle[cpu];

            L1D.add_wq(&data_packet);

            sim_store_sent++;
          }
        }
      } else {
        // DP ( if (warmup_complete[cpu]) {
        // cout << "[ROB] " << __func__ << " instr_id: " <<
        // ROB.entry[ROB.head].instr_id << " L1D WQ is full" << endl; });

        L1D.WQ.FULL++;
        L1D.STALL[RFO]++;

        return;
      }
    }

    // release SQ entries
    for (uint32_t i = 0; i < MAX_INSTR_DESTINATIONS; i++) {
      if (ROB.entry[ROB.head].sq_index[i] != UINT32_MAX) {
        uint32_t sq_index = ROB.entry[ROB.head].sq_index[i];

        // DP ( if (warmup_complete[cpu]) {
        // cout << "[SQ] " << __func__ << " instr_id: " <<
        // ROB.entry[ROB.head].instr_id << " releases sq_index: " << sq_index;
        // cout << hex << " address: " <<
        // (SQ.entry[sq_index].physical_address>>LOG2_BLOCK_SIZE); cout << "
        // full_addr: " << SQ.entry[sq_index].physical_address << dec << endl;
        // });

        LSQ_ENTRY empty_entry;
        SQ.entry[sq_index] = empty_entry;

        SQ.occupancy--;
        SQ.head++;
        if (SQ.head == SQ.SIZE)
          SQ.head = 0;
      }
    }

    // release ROB entry
    // DP ( if (warmup_complete[cpu]) {
    // cout << "[ROB] " << __func__ << " instr_id: " <<
    // ROB.entry[ROB.head].instr_id << " is retired" << endl; });

    ooo_model_instr empty_entry;
    ROB.entry[ROB.head] = empty_entry;

    ROB.head++;
    if (ROB.head == ROB.SIZE)
      ROB.head = 0;
    ROB.occupancy--;
    completed_executions--;
    num_retired++;

	if(num_retired % 1000000 == 0)
	{
		// file2 << num_retired << " sum mshr stall: " << sum_mshr_stall << endl;
    	file2 << num_retired << " " << sum_mshr_stall << " " << current_core_cycle[cpu] << " " << ROB.entry[ROB.head].executed << endl;;
		// cout << sum_mshr_stall << endl;
	}	
    
  }
}
