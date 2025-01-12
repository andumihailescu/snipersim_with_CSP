import sim
import os
import csv

CORE_STATE_LABELS = {
    0: 'RUNNING',
    1: 'INITIALIZING',
    2: 'STALLED',
    3: 'SLEEPING',
    4: 'WAKING_UP',
    5: 'IDLE',
    6: 'BROKEN'
}

def is_idle(state_id):
    return (state_id == 5)

class NbitPredictor:
    """Individual core analyzer - one instance per core"""
    def __init__(self, n_bits):
        self.n_bits = n_bits
        self.state = 0  # Initial state: 00 (Strongly Idle)
        self.max_state = (1 << n_bits) - 1  # Max value for N-bit counter (2^n - 1)

        # Log of predictions (for offline analysis):
        #   Each item: {
        #       'event_id': int,
        #       'core_id': int,
        #       'predicted_idle': bool or None,
        #       'actual_idle': bool
        #   }
        self.prediction_log = []

        # - how many predictions were made, how many correct, how many predicted idle/active
        self.num_predictions_made = 0
        self.num_predictions_correct = 0
        self.num_predictions_idle = 0
        self.num_predictions_active = 0

    def update(self, event):
        """Update the counter state based on the event (1 = Active, 0 = Idle)"""
        if event == 1:  # Active
            if self.state < self.max_state:
                self.state += 1  # Increment the counter (up to the max value)
        elif event == 0:  # Idle
            if self.state > 0:
                self.state -= 1  # Decrement the counter (down to 0)

    def predict_idle(self):
        """Return the predicted state: 1 for Active, 0 for Idle"""
        return False if self.state >= (self.max_state // 2) else True

    def log_prediction(self, event_id, core_id, predicted_idle, actual_idle):
        # Update counters only if a real prediction was made (not None)
        self.num_predictions_made += 1
        if predicted_idle == actual_idle:
            self.num_predictions_correct += 1
        if predicted_idle is True:
            self.num_predictions_idle += 1
        else:
            self.num_predictions_active += 1
            
        self.prediction_log.append({
            'event_id': event_id,
            'core_id': core_id,
            'predicted_idle': predicted_idle,
            'actual_idle': actual_idle
        })

class CoreStateAndBranchMonitor:
    def __init__(self):
        self.sampling_period = None
        self.results_folder = None

        # A global counter for all events, both periodic and branch
        self.global_event_id = 1

        # Logs for states and branches
        self.core_state_log = []  # (event_id, core_id, state_id, time_fs)
        self.branch_log = []      # (event_id, core_id, ip, actual_taken, predicted_taken, indirect, time_fs)

        # Track n-bit saturated counters
        self.n_bits = 2  # Default to 2-bit counter
        
        # Array of predictors for each core
        self.nbit_predictors = {}

    def setup(self, args):
        args = dict(enumerate((args or '').split(':')))
        self.sampling_period = int(args.get(0, None) or 1) # Default 1 us
        self.n_bits = int(args.get(1, None) or 2)  # Default 2-bit counter
        self.results_folder = sim.config.output_dir
        
        # Register periodic sampling
        self.periodic_hook = sim.util.Every(
            self.sampling_period * sim.util.Time.US,  # Convert to femtoseconds
            lambda time, time_delta: self.hook_periodic(time, time_delta)
        )

        # Register branch hook
        def branch_callback(ip, predicted, actual, indirect, core_id):
            self.hook_branch_predictor(core_id, ip, predicted, actual, indirect)
        self.branch_hook = sim.util.EveryBranch(branch_callback)
        
        # Initialize N-bit predictor for each core
        num_cores = sim.config.ncores
        for core_id in range(num_cores):
            self.nbit_predictors[core_id] = NbitPredictor(self.n_bits)

        print(f"[CORE_STATE_BRANCH_MONITOR] Setup complete.")
        print(f"[CORE_STATE_BRANCH_MONITOR] Sampling period [us]: {self.sampling_period}")
        print(f"[CORE_STATE_BRANCH_MONITOR] Registered n-bit saturated counter with {self.n_bits} bits")

    # These methods will be automatically called by Sniper's hook system
    def hook_periodic(self, time, time_delta=0):
        
        if time_delta == 0:
            return
        
        num_cores = sim.config.ncores
        for core_id in range(num_cores):
            event_id = self.global_event_id
            self.global_event_id += 1

            state_id = sim.dvfs.get_core_state(core_id)
            
            # Log the current core state
            self.core_state_log.append((event_id, core_id, state_id, time))

            # Update the N-bit predictor with the state of the core
            self.nbit_predictors[core_id].update(1 if not is_idle(state_id) else 0)

            predicted_idle = self.nbit_predictors[core_id].predict_idle()
            actual_idle = is_idle(state_id)
            self.nbit_predictors[core_id].log_prediction(
                event_id=event_id,
                core_id=core_id,
                predicted_idle=predicted_idle,
                actual_idle=actual_idle
            )

    def hook_branch_predictor(self, core_id, ip, predicted, actual, indirect):
        event_id = self.global_event_id
        self.global_event_id += 1

        time = sim.stats.time()
        self.branch_log.append(
            (event_id, core_id, ip, actual, predicted, indirect, time)
        )

    def hook_sim_end(self):
        # 1) Core state samples
        core_states_file = os.path.join(self.results_folder, "periodic_core_states.csv")
        with open(core_states_file, 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(["event_id", "core_id", "state_id", "time_fs"])
            for (event_id, core_id, state_id, time_fs) in self.core_state_log:
                writer.writerow([event_id, core_id, state_id, time_fs])

        # 2) Branch events
        branch_file = os.path.join(self.results_folder, "branch_events.csv")
        with open(branch_file, 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow([
                "event_id", "core_id", "ip",
                "actual_taken", "predicted_taken", "indirect", "time_fs"
            ])
            for (event_id, core_id, ip, actual, predicted, indirect, time_fs) in self.branch_log:
                writer.writerow([
                    event_id, core_id, hex(ip), actual, predicted, indirect, time_fs
                ])

        # 3) N-bit counter predictions for all cores
        predictions_file = os.path.join(self.results_folder, "nbit_counter_predictions.csv")
        with open(predictions_file, 'w', newline='') as csvfile:
            writer = csv.DictWriter(csvfile, fieldnames=[
                'event_id', 'core_id', 'predicted_idle', 'actual_idle'
            ])
            writer.writeheader()
            num_cores = sim.config.ncores
            for core_id in range(num_cores):
                for row in self.nbit_predictors[core_id].prediction_log:
                    writer.writerow(row)

        # 4) Final statistics (branches taken/not, prediction accuracy, etc.) for all cores
        stats_file = os.path.join(self.results_folder, "nbit_counter_statistics.csv")
        with open(stats_file, 'w', newline='') as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow(['Core_Id', 'Statistic', 'Value'])
            writer.writerow("")

            num_cores = sim.config.ncores
            for core_id in range(num_cores):

                # Prediction stats
                writer.writerow([f"Core {core_id}", 'Predictions_Made', self.nbit_predictors[core_id].num_predictions_made])
                writer.writerow([f"Core {core_id}", 'Predictions_Correct', self.nbit_predictors[core_id].num_predictions_correct])

                accuracy = 0.0
                if self.nbit_predictors[core_id].num_predictions_made > 0:
                    accuracy = (float(self.nbit_predictors[core_id].num_predictions_correct)
                                / float(self.nbit_predictors[core_id].num_predictions_made)) * 100.0
                writer.writerow([f"Core {core_id}", 'Accuracy_Percent', f"{accuracy:.2f}"])

                writer.writerow([f"Core {core_id}", 'Predictions_Idle', self.nbit_predictors[core_id].num_predictions_idle])
                writer.writerow([f"Core {core_id}", 'Predictions_Active', self.nbit_predictors[core_id].num_predictions_active])
                writer.writerow("")

        print("[CORE_STATE_BRANCH_MONITOR] Simulation ended. Data saved:")
        print(f"  - {core_states_file}")
        print(f"  - {branch_file}")
        print(f"  - {predictions_file}")
        print(f"  - {stats_file}")     

# Register the analyzer
analyzer = CoreStateAndBranchMonitor()
sim.util.register(analyzer)
