all:
	./build/auto build

install:
	./build/auto install

clean:
	rm -f src/*.o
	rm -f src/gpgewrap
	rm -f src/fetchaddr
	rm -f src/parsedate
	rm -f src/dotlock



