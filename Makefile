.PHONY: build check check-live check-sanitize install source-bundle clean
CC ?= cc
SPARKDASH_URL ?= http://127.0.0.1:5555

build:
	./scripts/build-app.sh
check:
	mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/dashboard_data.c tests/dashboard_test.c -lcurl -lm -o build/dashboard-test
	./build/dashboard-test
	python3 -m unittest discover -s tests -p '*_test.py'
check-live: check
	./build/dashboard-test --live "$(SPARKDASH_URL)"
check-sanitize:
	mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Isrc src/dashboard_data.c tests/dashboard_test.c -lcurl -lm -o build/dashboard-sanitize
	./build/dashboard-sanitize
install: build
	python3 scripts/install-usb.py
source-bundle:
	python3 scripts/source-bundle.py
clean:
	rm -rf build dist
