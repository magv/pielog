all: pielog-stubs/__init__.pyi test README.md

build:
	python3 -c "import setuptools; setuptools.setup()" build_ext --inplace

pielog-stubs/__init__.pyi: build
	python3 mod2md.py pielog --pyi > $@

test: build
	python3 -m pytest

README.md: build
	sed '/## Python API/,$$d' README.md >README.md.tmp
	printf '## Python API\n\n' >>README.md.tmp
	python3 mod2md.py pielog >>README.md.tmp
	mv README.md.tmp README.md

demo:
	$(MAKE) -C demos

wheel: pielog-stubs/__init__.pyi
	python3 -m build --wheel

dist: pielog-stubs/__init__.pyi
	python3 -m build

clean:
	rm -rf build dist *.egg-info pielog/__pycache__ tests/__pycache__
	rm -f pielog*.so pielog*.pyd pielog.cpp pielog/_version.h README.md.tmp
	$(MAKE) -C demos clean

.PHONY: all build test wheel dist demo clean
