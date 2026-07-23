TARGETS := profiler
all: clean $(TARGETS) clean-obj

%: %.c
	gcc -O3 -o $@ profiler.c -lpfm -fopenmp

clean-obj:
	rm -rf *.o

clean:
	rm -rf *.o $(TARGETS)
