# syntax=docker/dockerfile:1
FROM node:24-bookworm-slim AS frontend-deps
WORKDIR /src
RUN npm install --global pnpm@11.19.0
COPY package.json pnpm-lock.yaml pnpm-workspace.yaml ./
COPY apps/miniapp/package.json apps/miniapp/package.json
COPY packages/contracts/package.json packages/contracts/package.json
RUN pnpm install --frozen-lockfile

FROM frontend-deps AS frontend-build
ARG RUN_TESTS=true
COPY apps/miniapp ./apps/miniapp
COPY packages/contracts ./packages/contracts
COPY tsconfig*.json ./
RUN if [ "$RUN_TESTS" = "true" ]; then pnpm check; \
    elif [ "$RUN_TESTS" = "false" ]; then pnpm typecheck && pnpm build; \
    else exit 2; fi

FROM debian:trixie-slim AS cpp-deps
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked --mount=type=cache,target=/var/lib/apt/lists,sharing=locked rm -f /etc/apt/apt.conf.d/docker-clean && apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
    ca-certificates cmake g++ ninja-build pkg-config libdrogon-dev libjsoncpp-dev uuid-dev zlib1g-dev \
    nlohmann-json3-dev libssl-dev libpq-dev libcurl4-openssl-dev

WORKDIR /src

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked --mount=type=cache,target=/var/lib/apt/lists,sharing=locked apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends libsqlite3-dev libmariadb-dev libhiredis-dev libbrotli-dev libyaml-cpp-dev libc-ares-dev

FROM cpp-deps AS cpp-build
ARG RUN_TESTS=true
COPY apps/backend ./apps/backend
COPY data/taxonomy.example.json ./data/taxonomy.example.json
RUN cmake -S apps/backend -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --parallel 2 \
    && if [ "$RUN_TESTS" = "true" ]; then ctest --test-dir /build --output-on-failure; elif [ "$RUN_TESTS" != "false" ]; then exit 2; fi

FROM debian:trixie-slim AS runtime
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked --mount=type=cache,target=/var/lib/apt/lists,sharing=locked rm -f /etc/apt/apt.conf.d/docker-clean && apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
    ca-certificates curl libdrogon1t64 libpq5 libssl3t64 libcurl4t64 \
    && groupadd --gid 10001 app \
    && useradd --uid 10001 --gid app --no-create-home --home-dir /app --shell /usr/sbin/nologin app
WORKDIR /app
ENV PORT=8080 DATA_DIR=/app/data MIGRATIONS_DIR=/app/db/migrations STATIC_DIR=/app/miniapp
COPY --from=cpp-build /build/max-help /app/bin/max-help
COPY --from=cpp-build /build/queue-tests /app/bin/queue-tests
COPY --from=cpp-build /build/bot-workflow-tests /app/bin/bot-workflow-tests
COPY --from=cpp-build /build/matching-tests /app/bin/matching-tests
COPY --from=frontend-build /src/apps/miniapp/dist /app/miniapp
COPY data /app/data
COPY db/migrations /app/db/migrations
COPY infra/init-db.sh /app/infra/init-db.sh
USER 10001:10001
EXPOSE 8080
CMD ["/app/bin/max-help", "serve"]
