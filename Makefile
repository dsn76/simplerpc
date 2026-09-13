
all: build_all


build_all: build
	@cd build && cmake .. && make


build:
	@mkdir -p ./build


clean:
	@rm -rf ./build

