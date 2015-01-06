all:
	@cd build && ./build-gnu.sh && cd -

install:
	@build/install-gnu.sh

clean:
	rm -f src/*.o
	rm -f src/pgpewrap
	rm -f src/fetchaddr
	rm -f src/parsedate
	rm -f src/dotlock



