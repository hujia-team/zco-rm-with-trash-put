


.PHONY: help gcc-v1 gcc-v2 install


gcc-v2:
	gcc -fPIC -shared -o libtrash_intercept_v2.so trash_intercept_v2.c -ldl
## export LD_PRELOAD=libtrash_intercept_v2.so

## make sh failed: command -v trash-put &> /dev/null
check:
	@if ! command -v trash-put >/dev/null 2>&1; then \
		echo "[checked] require install trash-cli"; \
		sudo apt update && sudo apt-get install -y trash-cli; \
	else \
		echo "[checked] trash-cli version: $$(trash-put --version)"; \
	fi


install-zshrc: check
	gcc -fPIC -shared -o libtrash_intercept_v2.so trash_intercept_v2.c -ldl
	mkdir -p ~/.local/lib/zco
	@if [ -f ~/.local/lib/zco/libtrash_intercept_v2.so ]; then \
		echo "[checked] libtrash_intercept_v2.so already installed, 请手动重置, 如下:"; \
		echo "" \
		echo "unset LD_PRELOAD"; \
		echo "cp libtrash_intercept_v2.so ~/.local/lib/zco/libtrash_intercept_v2.so"; \
	else \
		cp libtrash_intercept_v2.so ~/.local/lib/zco/libtrash_intercept_v2.so; \
		if [ -f ~/.zshrc ]; then \
			echo "\n === 检查 ~/.zshrc 配置 ===\n"; \
			if ! grep -q "LD_PRELOAD" ~/.zshrc; then \
				echo "[checked] require add LD_PRELOAD to ~/.zshrc"; \
				echo "export LD_PRELOAD=~/.local/lib/zco/libtrash_intercept_v2.so" >> ~/.zshrc; \
				echo "新配置: export LD_PRELOAD=~/.local/lib/zco/libtrash_intercept_v2.so "; \
			else \
				echo "[checked] LD_PRELOAD already set in ~/.zshrc"; \
				line=$$(grep "LD_PRELOAD" ~/.zshrc); \
				if [ "$$line" != "export LD_PRELOAD=~/.local/lib/zco/libtrash_intercept_v2.so" ]; then \
					echo "原配置: $$line "; \
					echo "请替换: export LD_PRELOAD=~/.local/lib/zco/libtrash_intercept_v2.so"; \
				else \
					echo "已配置: $$line "; \
				fi \
			fi \
		fi \
	fi
	@echo ""
	@echo "如需立即生效，请执行 export LD_PRELOAD=~/.local/lib/zco/libtrash_intercept_v2.so"
	@echo ""



