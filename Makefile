app: dist
	yarn tauri build

dist: yarn
	yarn build

yarn:
	yarn

.PHONY: yarn
