# Multi-stage build for the React SPA.
# Build context is the REPO ROOT so a future codegen step could reach
# docs/openapi.yaml; today every COPY is prefixed with `frontend/`.
# Final image is ~25MB (alpine + ~150KB JS bundle).

# ── builder ─────────────────────────────────────────────────────────────
FROM node:20-alpine AS builder

WORKDIR /app

# Manifest first for layer caching: package.json + lock change → reinstall;
# source change alone keeps node_modules cached.
COPY frontend/package.json frontend/package-lock.json* ./
RUN if [ -f package-lock.json ]; then npm ci --no-audit --no-fund; \
    else npm install --no-audit --no-fund; fi

# Build configuration files.
COPY frontend/tsconfig.json frontend/tsconfig.node.json frontend/vite.config.ts ./
COPY frontend/tailwind.config.js frontend/postcss.config.js ./
COPY frontend/eslint.config.js frontend/.prettierrc.json frontend/index.html ./

# Source.
COPY frontend/src ./src
COPY frontend/public ./public

RUN npm run build

# ── runtime ─────────────────────────────────────────────────────────────
# Unprivileged base: master + workers run as the non-root "nginx" user (uid
# 101) and the server listens on 8080, so the image runs under a restrictive
# podSecurityContext (runAsNonRoot) without a privileged port or root pidfile.
FROM nginxinc/nginx-unprivileged:1.27-alpine AS runtime

# The base image drops to the non-root "nginx" user (uid 101). Become root for
# the file-staging steps below — the webroot is root-owned, so the in-place
# `mv` needs write on the directory — then drop back to nginx for runtime.
USER root

# React SPA (auth / account / admin). Its index.html is renamed to app.html so
# it can act as the SPA fallback for /login, /account/*, /admin/* WITHOUT
# clobbering the public BookCard index.html that owns /. Vite assets live under
# /assets and don't clash with BookCard's /css /js /images.
COPY --from=builder /app/dist /usr/share/nginx/html
RUN mv /usr/share/nginx/html/index.html /usr/share/nginx/html/app.html
# Public BookCard static site at /: index.html (vCard) + blog + css/js/images.
COPY frontend/public-site /usr/share/nginx/html
COPY frontend/nginx.conf /etc/nginx/conf.d/default.conf

# Back to the unprivileged runtime user.
USER nginx

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --retries=3 \
    CMD wget -q -O - http://localhost:8080/healthz || exit 1
