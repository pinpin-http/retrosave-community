FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /code

# AD-32 : le contexte de build est la racine du monorepo, d'où les chemins
# préfixés. C'est ce qui rend `VERSION` copiable par construction plutôt que par
# discipline — un COPY qui échoue casse le build bruyamment, là où un argument
# oublié rendrait « inconnue » en silence, et seulement en production.
COPY server/requirements.lock ./
RUN pip install --no-cache-dir --require-hashes --requirement requirements.lock

COPY VERSION /app/VERSION
COPY server/app ./app
COPY server/alembic.ini ./
COPY server/migrations ./migrations

# Décoration de build : le sha est absent du fichier par nature, donc passé en
# argument — avec un défaut honnête plutôt qu'un vide silencieux. `0.1.0+dev`
# dit ce qu'il est ; une version vide ne dit rien.
ARG GIT_SHA=dev
ENV GIT_SHA=${GIT_SHA}

EXPOSE 8000

CMD ["sh", "-c", "alembic upgrade head && uvicorn app.main:app --host 0.0.0.0 --port 8000"]
