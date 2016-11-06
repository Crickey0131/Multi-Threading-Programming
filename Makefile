warmup2: warmup2.o my402list.o 
	gcc -o warmup2 -g -Wall warmup2.o my402list.o -lpthread -lm

warmup2.o: warmup2.c my402list.h packet.h
	gcc -g -c -Wall warmup2.c

my402list.o: my402list.c my402list.h
	gcc -g -c -Wall my402list.c

clean:
	rm -f *.o warmup2