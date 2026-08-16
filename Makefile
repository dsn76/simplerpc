# git clone --recurse-submodules https://github.com/dsn76/simplerpc

all: gitupdate build


build:
	@mkdir -p ./build
	@cd build && cmake .. && make


gitupdate:
	@git submodule update --init --remote --recursive


clean:
	@rm -rf ./build

