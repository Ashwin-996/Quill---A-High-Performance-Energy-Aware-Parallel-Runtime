import matplotlib.pyplot as plt
import csv

times = []
dops = []

# Read the CSV data we extracted
with open('dop_graph.csv', 'r') as file:
    reader = csv.reader(file)
    for row in reader:
        # row[0] is "DOP_PLOT_DATA", row[1] is time, row[2] is DOP
        times.append(int(row[1]))
        dops.append(int(row[2]))

# Create the plot
plt.figure(figsize=(10, 6))
plt.plot(times, dops, marker='o', linestyle='-', color='b')

# Format the graph
plt.title('Change in Degree of Parallelism (DOP) over Time')
plt.xlabel('Elapsed Time (milliseconds)')
plt.ylabel('Active Workers (DOP)')
plt.grid(True)
plt.ylim(0, 65) # Lock Y-axis between 0 and 64 cores

# Save the plot as an image for your report
plt.savefig('DOP_Chart.png')
print("Graph saved as DOP_Chart.png!")
