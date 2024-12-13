"""
Core State At Branch Event Data Exporter

A tool to collect and analyze core state patterns following branch events.
Exports detailed state transition data and statistical summaries to help understand
core behavior patterns and their relationship to branch instructions.



Core state values:
- RUNNING        0
- INITIALIZING   1
- STALLED        2
- SLEEPING       3
- WAKING_UP      4
- IDLE           5
- BROKEN         6
- NUM_STATES     7

Example run command:
paulrosu@WorkstationP1:~/experiments$ ~/snipersim_SCSP/run-sniper -v -n 4 -c gainestown --roi -s analysis_data_export --power -- ~/snipersim_SCSP/test/fft/fft -p 4

It will generate the following files, besides the usual ones:
- core_state_patterns.csv
- state_pattern_summary.csv

Next steps:
- Add a script to plot the data
- Investigate what observation window and sampling period are optimal
- Implement state corellation between cores

"""

import sim
import os
import csv

class CoreStateAtBranchEventAnalyzer:
    def __init__(self):
        self.results_folder = None
        self.state_patterns_file = None
        self.analysis_summary_file = None
        
        self.observation_window = 100   # microseconds
        self.sampling_period = 1       # microseconds
        self.active_records = {}       # key=(event_id, ip), value=record dict
        self.active_core_records = {}  # key=core_id, value=list of (event_id, ip)
        self.completed_records = []    # store completed records in memory
        self.total_branches = 0        # track total branches
        self.next_event_id = 1         # track the next available event ID

    def setup(self, args):
        self.results_folder = sim.config.output_dir
        self.state_patterns_file = os.path.join(self.results_folder, "core_state_patterns.csv")
        self.analysis_summary_file = os.path.join(self.results_folder, "state_pattern_summary.csv")

        for filepath in [self.state_patterns_file, self.analysis_summary_file]:
            if os.path.exists(filepath):
                os.remove(filepath)

        print(f"[CORE_ANALYZER] Initialized with observation_window={self.observation_window}us, sampling_period={self.sampling_period}us")

        sim.util.EveryBranch(self.record_branch_event)
        sim.util.Every(self.sampling_period * sim.util.Time.US, self.collect_state_sample)

    def record_branch_event(self, ip, predicted, actual, indirect, core_id):
        self.total_branches += 1
        current_time = sim.stats.time()
        current_instruction_count = sim.stats.icount()

        event_id = self.next_event_id
        self.next_event_id += 1

        if core_id not in self.active_core_records:
            self.active_core_records[core_id] = []

        record_key = (event_id, ip)
        event_record = {
            'start_time': current_time,
            'instruction_count': current_instruction_count,
            'states': [],
            'branch_taken': actual,
            'core_id': core_id,
            'event_id': event_id,
            'ip': ip
        }

        self.active_records[record_key] = event_record
        self.active_core_records[core_id].append(record_key)
        

    def collect_state_sample(self, time, time_delta):
        if time_delta == 0:
            return

        for core_id, record_keys in list(self.active_core_records.items()):
            if not record_keys:
                continue

            current_state = sim.dvfs.get_core_state(core_id)
            for record_key in record_keys[:]:
                record = self.active_records[record_key]
                elapsed_time = time - record['start_time']

                record['states'].append((elapsed_time, current_state))

                if elapsed_time > (self.observation_window * sim.util.Time.US):
                    # Move this record to completed_records
                    self.completed_records.append(record)
                    del self.active_records[record_key]
                    record_keys.remove(record_key)

    def generate_analysis_summary(self):
        # Analyze data directly from self.completed_records
        print("[CORE_ANALYZER] Generating analysis summary...")
        pattern_stats = {}
        total_records = 0

        for record in self.completed_records:
            total_records += 1
            ip = hex(record['ip'])
            branch_taken = record['branch_taken']
            states = [s for _, s in record['states']]

            idle_positions = [i for i, state in enumerate(states) if state == 5]

            if idle_positions:
                if ip not in pattern_stats:
                    pattern_stats[ip] = {
                        'count': 1,
                        'idle_positions': idle_positions,
                        'branch_taken_count': 1 if branch_taken else 0
                    }
                else:
                    pattern_stats[ip]['count'] += 1
                    pattern_stats[ip]['idle_positions'].extend(idle_positions)
                    if branch_taken:
                        pattern_stats[ip]['branch_taken_count'] += 1

        # Write pattern summary
        with open(self.analysis_summary_file, 'w') as f:
            f.write("Branch_IP,Count,Avg_Idle_Position,Idle_Time_Percent,Branch_Taken_Ratio\n")
            for ip, stats in pattern_stats.items():
                count = stats['count']
                idle_positions = stats['idle_positions']
                avg_position = sum(idle_positions) / len(idle_positions)
                # observation_window samples = observation_window / sampling_period
                # total samples per record = observation_window / sampling_period
                # we have observation_window microseconds and sampling_period=1us => 
                # total samples = observation_window
                total_samples_per_record = self.observation_window
                idle_percentage = (len(idle_positions) / (count * total_samples_per_record)) * 100
                branch_taken_ratio = stats['branch_taken_count'] / count
                f.write(f"{ip},{count},{avg_position:.2f},{idle_percentage:.2f},{branch_taken_ratio:.2f}\n")

        print(f"[CORE_ANALYZER] Analyzed {total_records} total records")
        print(f"[CORE_ANALYZER] Found {len(pattern_stats)} branches with IDLE states")

    def hook_sim_end(self):
        # Finalize any events still active at simulation end
        for record_key, record in self.active_records.items():
            self.completed_records.append(record)
        self.active_records.clear()

        # Write all completed records to the state_patterns_file
        with open(self.state_patterns_file, 'w') as f:
            f.write("Event_ID,Instruction_Count,Start_Time,Core_ID,Branch_IP,Branch_Taken,States\n")
            for record in self.completed_records:
                states_str = ','.join(str(s) for _, s in record['states'])
                f.write(f"{record['event_id']},{record['instruction_count']},{record['start_time']},{record['core_id']},{hex(record['ip'])},{record['branch_taken']},{states_str}\n")

        self.generate_analysis_summary()
        print(f"[CORE_ANALYZER] Total branches encountered: {self.total_branches}")

sim.util.register(CoreStateAtBranchEventAnalyzer())
