# This hook runs inside each spawned Vivado synthesis/implementation process.
# launch_runs -jobs limits concurrent runs; it does not limit threads inside a
# run, so keep this explicit child-process cap in addition to the parent cap.
set_param general.maxThreads 4
