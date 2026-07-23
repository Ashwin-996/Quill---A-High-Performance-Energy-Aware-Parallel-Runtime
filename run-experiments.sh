#!/bin/bash

# Define the search space
# N: How many threads to wake/sleep at once
N_VALUES=(1 2 4 8 16)
# Interval: Microseconds between evaluations (10ms, 50ms, 100ms, 250ms)
INTERVAL_VALUES=(10000 50000 100000 250000)

# Total logical cores on your specific AMD machine
# (Update this if nproc gives a different number)
export QUILL_WORKERS=64 

# Output file setup
OUTPUT_FILE="grid_search_results.csv"
echo "N,Interval(us),Time(sec),Energy(J),EDP" > $OUTPUT_FILE

echo "Starting Grid Search on HiPeC Server..."
echo "Workers set to: $QUILL_WORKERS"
echo "----------------------------------------"

# Ensure the project is compiled and up to date
make

for n in "${N_VALUES[@]}"; do
    for interval in "${INTERVAL_VALUES[@]}"; do
        
        echo "Running -> N: $n | Interval: $interval us"
        
        # Export the variables for the C++ runtime to catch
        export DCT_N=$n
        export DCT_INTERVAL=$interval
        
        # Run the benchmark and capture all stdout
        # Assuming your executable from the Makefile is 'streams_bench'
        RAW_OUTPUT=$(./streams_bench)
        
        # The profiler prints stats bounded by "====". 
        # We grab the second-to-last line containing the actual numbers.
        METRICS=$(echo "$RAW_OUTPUT" | grep -v "=" | grep -v "JPI" | grep -v "Calling" | tail -n 1)
        
        # Format the metrics by replacing tabs/spaces with commas
        CSV_METRICS=$(echo "$METRICS" | awk '{print $1","$2","$3}')
        
        # Append to the results file
        echo "$n,$interval,$CSV_METRICS" >> $OUTPUT_FILE
        
    done
done

echo "----------------------------------------"
echo "Grid Search Complete! Results saved to $OUTPUT_FILE"
