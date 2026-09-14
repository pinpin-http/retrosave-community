.PHONY: help up down token test test-server lint desktop android
help:
	@echo 'up / down : serveur de développement local'
	@echo 'token LABEL=nom : créer un compte et afficher son jeton'
	@echo 'test : vecteurs Python ; test-server : API sur des conteneurs jetables'
	@echo 'desktop / android : construire les clients de développement'
	@echo 'Déploiement public : docs/guides/SELF_HOSTING.md'
up:
	test -f .env || cp .env.example .env
	docker compose up -d --build
down:
	docker compose down
token:
	docker compose exec -T server python -m app.cli new-token --label "$(LABEL)"
test:
	uv run pytest tests -q
test-server:
	uv run python scripts/testing/test_server.py
lint:
	uv run ruff check server scripts/testing/test_server.py
	python3 scripts/testing/check_docs.py
	android/gradlew -p android ktlintCheck
desktop:
	cmake -S desktop -B desktop/build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build desktop/build/dev --parallel 4
	ctest --test-dir desktop/build/dev --output-on-failure
android:
	android/gradlew -p android ktlintCheck testDebugUnitTest assembleDebug
