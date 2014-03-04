all:
	@cd build && ./build-gnu.sh && cd -

install:
	@build/install-gnu.sh

