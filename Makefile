all: mod_accounting.so

mod_accounting.so: mod_accounting.c
	apxs2 -c -Wl,-s mod_accounting.c

clean:
	rm -rf mod_accounting.so mod_accounting.o mod_accounting.c~ mod_accounting.slo .libs
